#include "wh_trigger.h"

#include "aa_bt.h"
#include "aa_bt_client.h"
#include "aa_handshake.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wh_config.h"

static const char *TAG = "wh_trig";

#define READY_POLL_MS 500

typedef enum {
    WH_IDLE = 0,
    WH_POKING,
    WH_HANDSHAKE,
} wh_busy_t;

static volatile wh_busy_t s_busy;
static TaskHandle_t s_poke_task;

static bool is_busy(void)
{
    return s_busy != WH_IDLE || aa_handshake_is_active();
}

static void poke_one(const esp_bd_addr_t bda)
{
    char bda_str[18];
    aa_bt_bda_str(bda, bda_str, sizeof(bda_str));
    aa_bt_client_clear_cancel();
    aa_poke_result_t r = aa_bt_client_poke(bda, (uint32_t)CONFIG_AA_WAKE_HOLD_MS);
    ESP_LOGI(TAG, "Poke [%s] result=%d", bda_str, (int)r);
}

static void poke_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "One-shot wake poke started");

    while (!aa_bt_is_ready()) {
        if (s_busy != WH_POKING) {
            goto exit;
        }
        vTaskDelay(pdMS_TO_TICKS(READY_POLL_MS));
    }

    esp_bd_addr_t list[AA_BT_MAX_BONDED];
    size_t n = aa_bt_get_bonded(list, AA_BT_MAX_BONDED);
    if (n == 0) {
        ESP_LOGW(TAG, "No bonded devices to poke");
    } else {
        ESP_LOGI(TAG, "Poking %u bonded device(s) once", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            if (s_busy != WH_POKING) {
                break;
            }
            poke_one(list[i]);
            if (s_busy != WH_POKING) {
                break;
            }
        }
    }

exit:
    ESP_LOGI(TAG, "One-shot wake poke finished (busy=%d)", (int)s_busy);
    s_poke_task = NULL;
    if (s_busy == WH_POKING) {
        s_busy = WH_IDLE;
        ESP_LOGI(TAG, "Poke finished with no handshake — idle");
    }
    vTaskDelete(NULL);
}

static esp_err_t start_oneshot_poke(void)
{
    aa_bt_client_init();
    if (s_poke_task != NULL) {
        ESP_LOGW(TAG, "Poke already running");
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = WH_POKING;
    aa_bt_client_clear_cancel();
    BaseType_t ok = xTaskCreate(poke_task, "wh_poke", 4096, NULL, 5, &s_poke_task);
    if (ok != pdPASS) {
        s_busy = WH_IDLE;
        s_poke_task = NULL;
        ESP_LOGE(TAG, "Failed to create poke task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wh_trigger_on_knock(const char *source_ip)
{
    if (!source_ip || source_ip[0] == '\0') {
        ESP_LOGW(TAG, "Knock ignored — empty source IP");
        return;
    }
    if (is_busy()) {
        ESP_LOGI(TAG, "Knock from %s ignored — busy", source_ip);
        return;
    }
    if (!wh_config_bssid_usable()) {
        ESP_LOGW(TAG, "Knock from %s ignored — no AP BSSID yet", source_ip);
        return;
    }

    wh_config_set_ip(source_ip);

    if (aa_handshake_connection_open()) {
        ESP_LOGI(TAG, "Disconnecting AA RFCOMM before poke");
        aa_handshake_force_close();
    }

    ESP_LOGI(TAG, "Knock accepted from %s — one-shot wake poke", source_ip);
    if (start_oneshot_poke() != ESP_OK) {
        s_busy = WH_IDLE;
    }
}

void wh_trigger_on_handshake_open(void)
{
    ESP_LOGI(TAG, "Handshake open — cancelling poke");
    s_busy = WH_HANDSHAKE;
    aa_bt_client_cancel();
}

void wh_trigger_on_handshake_complete(bool success)
{
    ESP_LOGI(TAG, "Handshake complete success=%d — idle", (int)success);
    aa_bt_client_cancel();
    s_busy = WH_IDLE;
}

void wh_trigger_on_handshake_abort(void)
{
    ESP_LOGI(TAG, "Handshake abort — idle");
    aa_bt_client_cancel();
    s_busy = WH_IDLE;
}
