#include "aa_bt_client.h"

#include <inttypes.h>
#include <string.h>
#include "aa_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "aa_bt_cli";

#define UUID16_HFP_AG  0x111F
#define UUID16_HSP_AG  0x1112

#define SDP_WAIT_MS       10000
#define CONNECT_WAIT_MS   20000
#define HOLD_POLL_MS      100

static const uint16_t s_poke_uuids[] = { UUID16_HFP_AG, UUID16_HSP_AG };

static SemaphoreHandle_t s_sdp_sem;
static SemaphoreHandle_t s_conn_sem;

static volatile bool s_cancel;

static int32_t s_sdp_scn; /* -1 = fail */
static esp_spp_status_t s_open_status;
static uint32_t s_open_handle;
static bool s_open_ok;
static uint32_t s_client_handle;
static bool s_client_open;

static const esp_spp_sec_t s_sec = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t s_role = ESP_SPP_ROLE_MASTER;

static const char *profile_name(uint16_t uuid16)
{
    if (uuid16 == UUID16_HFP_AG) {
        return "HFP-AG";
    }
    if (uuid16 == UUID16_HSP_AG) {
        return "HSP-AG";
    }
    return "UUID";
}

static void ensure_sync(void)
{
    if (!s_sdp_sem) {
        s_sdp_sem = xSemaphoreCreateBinary();
    }
    if (!s_conn_sem) {
        s_conn_sem = xSemaphoreCreateBinary();
    }
}

esp_err_t aa_bt_client_init(void)
{
    ensure_sync();
    s_cancel = false;
    s_client_handle = 0;
    s_client_open = false;
    return ESP_OK;
}

void aa_bt_client_cancel(void)
{
    ensure_sync();
    s_cancel = true;
    if (s_client_open && s_client_handle != 0) {
        ESP_LOGI(TAG, "Cancel: disconnecting client handle=%" PRIu32, s_client_handle);
        esp_spp_disconnect(s_client_handle);
    }
    if (s_sdp_sem) {
        xSemaphoreGive(s_sdp_sem);
    }
    if (s_conn_sem) {
        xSemaphoreGive(s_conn_sem);
    }
}

void aa_bt_client_clear_cancel(void)
{
    s_cancel = false;
}

void aa_bt_client_on_sdp_search(esp_sdp_cb_param_t *param)
{
    ensure_sync();
    s_sdp_scn = -1;
    if (param->search.status == ESP_SDP_SUCCESS && param->search.record_count > 0 &&
        param->search.records != NULL) {
        for (int i = 0; i < param->search.record_count; i++) {
            int32_t scn = param->search.records[i].hdr.rfcomm_channel_number;
            if (scn >= 1 && scn <= 30) {
                s_sdp_scn = scn;
                ESP_LOGI(TAG, "SDP found RFCOMM SCN %ld (record %d/%d)",
                         (long)scn, i + 1, param->search.record_count);
                break;
            }
        }
        if (s_sdp_scn < 0) {
            ESP_LOGW(TAG, "SDP records=%d but no RFCOMM channel", param->search.record_count);
        }
    } else {
        ESP_LOGW(TAG, "SDP search status=%d records=%d",
                 param->search.status, param->search.record_count);
    }
    if (s_sdp_sem) {
        xSemaphoreGive(s_sdp_sem);
    }
}

void aa_bt_client_on_spp_cl_init(esp_spp_cb_param_t *param)
{
    ESP_LOGI(TAG, "ESP_SPP_CL_INIT_EVT status=%d handle=%" PRIu32 " sec=%d",
             param->cl_init.status, param->cl_init.handle, (int)param->cl_init.sec_id);
    if (param->cl_init.status != ESP_SPP_SUCCESS) {
        s_open_ok = false;
        s_open_status = param->cl_init.status;
        if (s_conn_sem) {
            xSemaphoreGive(s_conn_sem);
        }
    }
}

void aa_bt_client_on_spp_open(esp_spp_cb_param_t *param)
{
    char bda_str[18];
    ESP_LOGI(TAG, "ESP_SPP_OPEN_EVT status=%d handle=%" PRIu32 " rem=[%s]",
             param->open.status, param->open.handle,
             aa_bt_bda_str(param->open.rem_bda, bda_str, sizeof(bda_str)));
    s_open_status = param->open.status;
    s_open_handle = param->open.handle;
    s_open_ok = (param->open.status == ESP_SPP_SUCCESS);
    if (s_open_ok) {
        s_client_handle = param->open.handle;
        s_client_open = true;
    }
    if (s_conn_sem) {
        xSemaphoreGive(s_conn_sem);
    }
}

void aa_bt_client_on_spp_close(esp_spp_cb_param_t *param)
{
    if (s_client_open && param->close.handle == s_client_handle) {
        ESP_LOGI(TAG, "Client ESP_SPP_CLOSE_EVT handle=%" PRIu32, param->close.handle);
        s_client_open = false;
        s_client_handle = 0;
        if (s_conn_sem) {
            xSemaphoreGive(s_conn_sem);
        }
    }
}

static int32_t sdp_find_scn(const esp_bd_addr_t bda, uint16_t uuid16)
{
    esp_bt_uuid_t uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = { .uuid16 = uuid16 },
    };

    while (xSemaphoreTake(s_sdp_sem, 0) == pdTRUE) {
    }

    s_sdp_scn = -1;
    esp_err_t err = esp_sdp_search_record((uint8_t *)bda, uuid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sdp_search_record(%s): %s", profile_name(uuid16),
                 esp_err_to_name(err));
        return -1;
    }

    if (xSemaphoreTake(s_sdp_sem, pdMS_TO_TICKS(SDP_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "SDP search timeout for %s", profile_name(uuid16));
        return -1;
    }
    if (s_cancel) {
        return -1;
    }
    return s_sdp_scn;
}

static bool connect_scn(const esp_bd_addr_t bda, uint8_t scn)
{
    while (xSemaphoreTake(s_conn_sem, 0) == pdTRUE) {
    }

    s_open_ok = false;
    s_open_handle = 0;
    s_open_status = ESP_SPP_FAILURE;

    esp_err_t err = esp_spp_connect(s_sec, s_role, scn, (uint8_t *)bda);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_spp_connect scn=%u: %s", (unsigned)scn, esp_err_to_name(err));
        return false;
    }

    bool got = xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(CONNECT_WAIT_MS)) == pdTRUE;

    if (s_cancel) {
        if (s_client_open && s_client_handle != 0) {
            esp_spp_disconnect(s_client_handle);
        }
        return false;
    }
    if (!got) {
        ESP_LOGW(TAG, "Connect timeout scn=%u", (unsigned)scn);
        return false;
    }
    return s_open_ok;
}

static void hold_connection(uint32_t hold_ms)
{
    uint32_t waited = 0;
    while (waited < hold_ms) {
        if (s_cancel || !s_client_open) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(HOLD_POLL_MS));
        waited += HOLD_POLL_MS;
    }
    if (s_client_open && s_client_handle != 0) {
        ESP_LOGI(TAG, "Hold done — disconnecting handle=%" PRIu32, s_client_handle);
        esp_spp_disconnect(s_client_handle);
        xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(2000));
    }
}

aa_poke_result_t aa_bt_client_poke(const esp_bd_addr_t bda, uint32_t hold_ms)
{
    ensure_sync();
    char bda_str[18];
    aa_bt_bda_str(bda, bda_str, sizeof(bda_str));

    if (s_cancel) {
        return AA_POKE_CANCELLED;
    }

    for (size_t i = 0; i < sizeof(s_poke_uuids) / sizeof(s_poke_uuids[0]); i++) {
        if (s_cancel) {
            return AA_POKE_CANCELLED;
        }
        uint16_t uuid16 = s_poke_uuids[i];
        ESP_LOGI(TAG, "Poke [%s] via %s (0x%04X)...", bda_str, profile_name(uuid16),
                 (unsigned)uuid16);

        int32_t scn = sdp_find_scn(bda, uuid16);
        if (s_cancel) {
            return AA_POKE_CANCELLED;
        }
        if (scn < 1) {
            ESP_LOGI(TAG, "Poke via %s to [%s] — no SCN", profile_name(uuid16), bda_str);
            continue;
        }

        ESP_LOGI(TAG, "Connecting to [%s] %s SCN %ld...", bda_str, profile_name(uuid16),
                 (long)scn);
        if (!connect_scn(bda, (uint8_t)scn)) {
            if (s_cancel) {
                return AA_POKE_CANCELLED;
            }
            ESP_LOGI(TAG, "Poke via %s to [%s] failed", profile_name(uuid16), bda_str);
            continue;
        }

        ESP_LOGI(TAG, "Poked [%s] via %s — holding %" PRIu32 "ms", bda_str,
                 profile_name(uuid16), hold_ms);
        hold_connection(hold_ms);
        return s_cancel ? AA_POKE_CANCELLED : AA_POKE_ANSWERED;
    }

    return s_cancel ? AA_POKE_CANCELLED : AA_POKE_FAILED;
}
