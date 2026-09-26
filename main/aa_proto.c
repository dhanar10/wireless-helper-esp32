#include "aa_proto.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "aa_proto";

static size_t encode_varint(uint8_t *out, size_t out_len, uint32_t value)
{
    size_t n = 0;
    while (value >= 0x80) {
        if (n >= out_len) {
            return 0;
        }
        out[n++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (n >= out_len) {
        return 0;
    }
    out[n++] = (uint8_t)(value & 0x7F);
    return n;
}

static size_t encode_string(uint8_t *out, size_t out_len, int field, const char *value)
{
    size_t vlen = strlen(value);
    size_t n = 0;
    size_t k = encode_varint(out + n, out_len - n, (uint32_t)((field << 3) | 2));
    if (k == 0) {
        return 0;
    }
    n += k;
    k = encode_varint(out + n, out_len - n, (uint32_t)vlen);
    if (k == 0) {
        return 0;
    }
    n += k;
    if (n + vlen > out_len) {
        return 0;
    }
    memcpy(out + n, value, vlen);
    return n + vlen;
}

static size_t encode_uint32(uint8_t *out, size_t out_len, int field, uint32_t value)
{
    size_t n = 0;
    size_t k = encode_varint(out + n, out_len - n, (uint32_t)((field << 3) | 0));
    if (k == 0) {
        return 0;
    }
    n += k;
    k = encode_varint(out + n, out_len - n, value);
    if (k == 0) {
        return 0;
    }
    return n + k;
}

static size_t encode_enum(uint8_t *out, size_t out_len, int field, uint32_t value)
{
    return encode_uint32(out, out_len, field, value);
}

static size_t wrap_packet(uint8_t *out, size_t out_len, uint16_t msg_id,
                          const uint8_t *payload, size_t payload_len)
{
    if (out_len < 4 + payload_len) {
        return 0;
    }
    out[0] = (uint8_t)((payload_len >> 8) & 0xFF);
    out[1] = (uint8_t)(payload_len & 0xFF);
    out[2] = (uint8_t)((msg_id >> 8) & 0xFF);
    out[3] = (uint8_t)(msg_id & 0xFF);
    memcpy(out + 4, payload, payload_len);
    return 4 + payload_len;
}

size_t aa_proto_build_wifi_start_request(uint8_t *out, size_t out_len,
                                         const char *ip, uint16_t port)
{
    uint8_t payload[128];
    size_t n = 0;
    size_t k = encode_string(payload + n, sizeof(payload) - n, 1, ip);
    if (k == 0) {
        return 0;
    }
    n += k;
    k = encode_uint32(payload + n, sizeof(payload) - n, 2, port);
    if (k == 0) {
        return 0;
    }
    n += k;
    return wrap_packet(out, out_len, AA_MSG_WIFI_START_REQUEST, payload, n);
}

size_t aa_proto_build_wifi_info_response(uint8_t *out, size_t out_len,
                                         const char *ssid, const char *key,
                                         const char *bssid)
{
    uint8_t payload[256];
    size_t n = 0;
    size_t k;

    k = encode_string(payload + n, sizeof(payload) - n, 1, ssid);
    if (k == 0) {
        return 0;
    }
    n += k;
    k = encode_string(payload + n, sizeof(payload) - n, 2, key);
    if (k == 0) {
        return 0;
    }
    n += k;
    k = encode_string(payload + n, sizeof(payload) - n, 3, bssid);
    if (k == 0) {
        return 0;
    }
    n += k;
    k = encode_enum(payload + n, sizeof(payload) - n, 4, AA_SECURITY_WPA2_PERSONAL);
    if (k == 0) {
        return 0;
    }
    n += k;
    k = encode_enum(payload + n, sizeof(payload) - n, 5, AA_AP_TYPE_STATIC);
    if (k == 0) {
        return 0;
    }
    n += k;
    return wrap_packet(out, out_len, AA_MSG_WIFI_INFO_RESPONSE, payload, n);
}

static bool read_varint(const uint8_t *data, size_t len, size_t *offset, uint32_t *value)
{
    uint32_t val = 0;
    int shift = 0;
    while (*offset < len) {
        uint8_t b = data[(*offset)++];
        val |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            *value = val;
            return true;
        }
        shift += 7;
        if (shift > 28) {
            return false;
        }
    }
    return false;
}

bool aa_proto_parse_payload(uint16_t msg_id, const uint8_t *payload, size_t len,
                            aa_parsed_msg_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->msg_id = msg_id;

    size_t offset = 0;
    while (offset < len) {
        uint32_t tag = 0;
        if (!read_varint(payload, len, &offset, &tag)) {
            ESP_LOGW(TAG, "varint tag parse failed");
            return false;
        }
        uint32_t field = tag >> 3;
        uint32_t wire = tag & 0x7;

        if (wire == 0) {
            uint32_t val = 0;
            if (!read_varint(payload, len, &offset, &val)) {
                return false;
            }
            if (msg_id == AA_MSG_WIFI_CONNECT_STATUS && field == 1) {
                out->status = (int32_t)val;
                out->has_status = true;
            } else if (msg_id == AA_MSG_WIFI_START_RESPONSE && field == 3) {
                /* signed zigzag may apply; treat as raw status for logging */
                out->status = (int32_t)val;
                out->has_status = true;
            }
        } else if (wire == 2) {
            uint32_t slen = 0;
            if (!read_varint(payload, len, &offset, &slen)) {
                return false;
            }
            if (offset + slen > len) {
                return false;
            }
            if (msg_id == AA_MSG_WIFI_CONNECT_STATUS && field == 2) {
                size_t copy = slen < sizeof(out->status_text) - 1 ? slen : sizeof(out->status_text) - 1;
                memcpy(out->status_text, payload + offset, copy);
                out->status_text[copy] = '\0';
                out->has_status_text = true;
            }
            offset += slen;
        } else {
            ESP_LOGW(TAG, "unsupported wire type %u field %u", (unsigned)wire, (unsigned)field);
            return false;
        }
    }
    return true;
}
