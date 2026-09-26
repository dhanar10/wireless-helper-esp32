#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Message IDs from BtProtocol.java / reference/bt_handshake.py */
#define AA_MSG_WIFI_START_REQUEST   1
#define AA_MSG_WIFI_INFO_REQUEST    2
#define AA_MSG_WIFI_INFO_RESPONSE   3
#define AA_MSG_WIFI_CONNECT_STATUS  6
#define AA_MSG_WIFI_START_RESPONSE  7

#define AA_SECURITY_WPA2_PERSONAL   8
#define AA_AP_TYPE_STATIC           1

#define AA_PROTO_MAX_PACKET         512

typedef struct {
    uint16_t msg_id;
    int32_t status;          /* field_1 for ConnectStatus, field_3 for StartResponse */
    bool has_status;
    char status_text[64];    /* field_2 for ConnectStatus */
    bool has_status_text;
} aa_parsed_msg_t;

/** Build WifiStartRequest (msgId=1). Returns packet length or 0 on error. */
size_t aa_proto_build_wifi_start_request(uint8_t *out, size_t out_len,
                                         const char *ip, uint16_t port);

/** Build WifiInfoResponse (msgId=3). Returns packet length or 0 on error. */
size_t aa_proto_build_wifi_info_response(uint8_t *out, size_t out_len,
                                         const char *ssid, const char *key,
                                         const char *bssid);

/**
 * Parse protobuf payload for a known msg_id.
 * Packet wire format is separate: [len:u16be][msg_id:u16be][payload].
 */
bool aa_proto_parse_payload(uint16_t msg_id, const uint8_t *payload, size_t len,
                            aa_parsed_msg_t *out);
