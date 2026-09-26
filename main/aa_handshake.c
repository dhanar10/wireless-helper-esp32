#include "aa_handshake.h"

#include <inttypes.h>
#include <string.h>
#include "aa_proto.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wh_config.h"
#include "wh_trigger.h"
#include "sdkconfig.h"

static const char *TAG = "aa_hs";

#define RX_BUF_SIZE 512

typedef struct {
    bool active;
    uint32_t handle;
    uint8_t rx[RX_BUF_SIZE];
    size_t rx_len;
    bool start_sent;
    bool done;
} aa_hs_session_t;

static aa_hs_session_t s_session;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static void send_buf(uint32_t handle, uint8_t *buf, size_t len)
{
    if (len == 0) {
        return;
    }
    esp_err_t err = esp_spp_write(handle, (int)len, buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_spp_write failed: %s", esp_err_to_name(err));
    }
}

static void send_start_request(uint32_t handle)
{
    if (!wh_config_has_aa_host()) {
        ESP_LOGE(TAG, "No knock IP — cannot send WifiStartRequest");
        return;
    }
    uint8_t pkt[AA_PROTO_MAX_PACKET];
    size_t n = aa_proto_build_wifi_start_request(pkt, sizeof(pkt),
                                                 wh_config_ip(),
                                                 (uint16_t)AA_TCP_PORT);
    if (n == 0) {
        ESP_LOGE(TAG, "Failed to build WifiStartRequest");
        return;
    }
    send_buf(handle, pkt, n);
    ESP_LOGI(TAG, "-> WifiStartRequest (msgId=%d) %s:%u",
             AA_MSG_WIFI_START_REQUEST, wh_config_ip(),
             (unsigned)AA_TCP_PORT);
}

static void send_info_response(uint32_t handle)
{
    if (!wh_config_bssid_usable()) {
        ESP_LOGE(TAG, "No usable BSSID — aborting WifiInfoResponse");
        esp_spp_disconnect(handle);
        return;
    }
    uint8_t pkt[AA_PROTO_MAX_PACKET];
    size_t n = aa_proto_build_wifi_info_response(pkt, sizeof(pkt),
                                                 CONFIG_AA_WIFI_SSID,
                                                 CONFIG_AA_WIFI_KEY,
                                                 wh_config_bssid());
    if (n == 0) {
        ESP_LOGE(TAG, "Failed to build WifiInfoResponse");
        return;
    }
    send_buf(handle, pkt, n);
    ESP_LOGI(TAG, "-> WifiInfoResponse (msgId=%d) SSID=%s BSSID=%s",
             AA_MSG_WIFI_INFO_RESPONSE, CONFIG_AA_WIFI_SSID, wh_config_bssid());
}

static void process_messages(aa_hs_session_t *s)
{
    while (s->rx_len >= 4 && !s->done) {
        uint16_t length = ((uint16_t)s->rx[0] << 8) | s->rx[1];
        uint16_t msg_id = ((uint16_t)s->rx[2] << 8) | s->rx[3];
        size_t total = 4u + (size_t)length;
        if (total > RX_BUF_SIZE) {
            ESP_LOGE(TAG, "Message too large (%u)", (unsigned)total);
            s->done = true;
            esp_spp_disconnect(s->handle);
            wh_trigger_on_handshake_complete(false);
            return;
        }
        if (s->rx_len < total) {
            return;
        }

        const uint8_t *payload = s->rx + 4;
        aa_parsed_msg_t parsed;
        aa_proto_parse_payload(msg_id, payload, length, &parsed);

        if (msg_id == AA_MSG_WIFI_START_RESPONSE) {
            int32_t status = parsed.has_status ? parsed.status : 0;
            ESP_LOGI(TAG, "<- WifiStartResponse (msgId=%d) status=%ld",
                     AA_MSG_WIFI_START_RESPONSE, (long)status);
            if (status != 0) {
                ESP_LOGW(TAG, "WifiStartRequest failed");
                s->done = true;
                wh_trigger_on_handshake_complete(false);
            }
        } else if (msg_id == AA_MSG_WIFI_INFO_REQUEST) {
            ESP_LOGI(TAG, "<- WifiInfoRequest (msgId=%d)", AA_MSG_WIFI_INFO_REQUEST);
            send_info_response(s->handle);
        } else if (msg_id == AA_MSG_WIFI_CONNECT_STATUS) {
            int32_t status = parsed.has_status ? parsed.status : -1;
            ESP_LOGI(TAG, "<- WifiConnectStatus (msgId=%d) status=%ld",
                     AA_MSG_WIFI_CONNECT_STATUS, (long)status);
            if (parsed.has_status_text) {
                ESP_LOGI(TAG, "  status text: %s", parsed.status_text);
            }
            if (status == 0) {
                ESP_LOGI(TAG, "WiFi setup complete; phone should connect via TCP");
                wh_trigger_on_handshake_complete(true);
            } else {
                ESP_LOGW(TAG, "WiFi connection failed");
                wh_trigger_on_handshake_complete(false);
            }
            s->done = true;
        } else {
            ESP_LOGW(TAG, "Unexpected msgId=%u", (unsigned)msg_id);
        }

        memmove(s->rx, s->rx + total, s->rx_len - total);
        s->rx_len -= total;
    }
}

void aa_handshake_on_open(uint32_t handle)
{
    ensure_lock();
    lock();
    memset(&s_session, 0, sizeof(s_session));
    s_session.active = true;
    s_session.handle = handle;
    unlock();

    ESP_LOGI(TAG, "AA RFCOMM open handle=%" PRIu32, handle);
    wh_trigger_on_handshake_open();
    send_start_request(handle);

    lock();
    s_session.start_sent = true;
    unlock();
}

void aa_handshake_on_data(uint32_t handle, const uint8_t *data, uint16_t len)
{
    ensure_lock();
    lock();
    if (!s_session.active || s_session.handle != handle || s_session.done) {
        unlock();
        return;
    }
    if (s_session.rx_len + len > RX_BUF_SIZE) {
        ESP_LOGE(TAG, "RX overflow");
        s_session.done = true;
        unlock();
        esp_spp_disconnect(handle);
        wh_trigger_on_handshake_complete(false);
        return;
    }
    memcpy(s_session.rx + s_session.rx_len, data, len);
    s_session.rx_len += len;
    process_messages(&s_session);
    unlock();
}

void aa_handshake_on_close(uint32_t handle)
{
    ensure_lock();
    lock();
    bool was_active = s_session.active && s_session.handle == handle;
    bool was_done = s_session.done;
    if (was_active) {
        ESP_LOGI(TAG, "AA RFCOMM closed handle=%" PRIu32, handle);
        s_session.active = false;
    }
    unlock();

    if (was_active && !was_done) {
        wh_trigger_on_handshake_abort();
    }
}

bool aa_handshake_is_active(void)
{
    ensure_lock();
    lock();
    bool active = s_session.active && !s_session.done;
    unlock();
    return active;
}

bool aa_handshake_connection_open(void)
{
    ensure_lock();
    lock();
    bool open = s_session.active;
    unlock();
    return open;
}

void aa_handshake_force_close(void)
{
    ensure_lock();
    lock();
    uint32_t handle = 0;
    bool open = s_session.active;
    if (open) {
        handle = s_session.handle;
    }
    unlock();

    if (open && handle != 0) {
        ESP_LOGI(TAG, "Force-close AA RFCOMM handle=%" PRIu32, handle);
        esp_spp_disconnect(handle);
    }
}

void aa_hfp_on_data(uint32_t handle, const uint8_t *data, uint16_t len)
{
    bool has_at = false;
    for (uint16_t i = 0; i + 1 < len; i++) {
        if (data[i] == 'A' && data[i + 1] == 'T') {
            has_at = true;
            break;
        }
    }
    if (!has_at) {
        return;
    }
    static uint8_t ok[] = {'O', 'K', '\r', '\n'};
    esp_err_t err = esp_spp_write(handle, sizeof(ok), ok);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HFP OK write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "HFP -> OK");
    }
}
