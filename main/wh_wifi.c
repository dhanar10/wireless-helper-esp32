#include "wh_wifi.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wh_config.h"
#include "wh_knock.h"

static const char *TAG = "wh_wifi";

/** Onboard LED on many ESP32 DevKit boards (active high). */
#define WH_WIFI_LED_GPIO GPIO_NUM_2

#define INITIAL_RETRY_DELAY_MS 2000
#define RECONNECT_DELAY_MS 10000
#define DHCP_TIMEOUT_MS 15000

static bool s_knock_started;
static bool s_have_ip;
static bool s_had_ip_once;
static esp_netif_t *s_sta_netif;
static TaskHandle_t s_retry_task;

static void wifi_led_set(bool on)
{
    gpio_set_level(WH_WIFI_LED_GPIO, on ? 1 : 0);
}

static void wifi_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WH_WIFI_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    wifi_led_set(false);
}

static void wifi_retry_task(void *pv)
{
    while (1) {
        /* Wait for disconnect signal OR DHCP timeout */
        BaseType_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DHCP_TIMEOUT_MS));

        if (notified) {
            /* Explicit disconnect event received */
            ESP_LOGW(TAG, "Disconnected — reconnecting");
        } else if (!s_have_ip) {
            /* Timeout: connected but no IP */
            ESP_LOGW(TAG, "DHCP timeout (%d ms) — forcing reconnect", DHCP_TIMEOUT_MS);
        } else {
            /* Timeout but we have IP — just continue waiting */
            continue;
        }

        wifi_led_set(false);
        wh_config_clear_bssid();

        /* Full stack reset for reconnect (after first IP) */
        if (s_had_ip_once) {
            ESP_LOGI(TAG, "Full stack reset (DHCP stop -> WiFi disconnect -> %d ms delay -> WiFi connect -> DHCP start)",
                     RECONNECT_DELAY_MS);
            esp_netif_dhcpc_stop(s_sta_netif);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            esp_wifi_connect();
            esp_netif_dhcpc_start(s_sta_netif);
        } else {
            /* First connect: simple retry */
            ESP_LOGI(TAG, "Initial retry in %d ms", INITIAL_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(INITIAL_RETRY_DELAY_MS));
            esp_wifi_connect();
        }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = data;
        wifi_led_set(false);
        s_have_ip = false;
        wh_config_clear_bssid();

        uint8_t reason = disc ? disc->reason : 0;
        ESP_LOGW(TAG, "Disconnected reason=%u — signaling retry task", (unsigned)reason);

        if (s_retry_task) {
            xTaskNotifyGive(s_retry_task);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
    wifi_led_set(true);
    s_have_ip = true;
    s_had_ip_once = true;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        wh_config_set_bssid(ap.bssid);
    } else {
        wh_config_clear_bssid();
        ESP_LOGE(TAG, "Could not read AP BSSID — AA handshake will abort");
    }

    if (!s_knock_started) {
        esp_err_t err = wh_knock_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Knock start failed: %s", esp_err_to_name(err));
        } else {
            s_knock_started = true;
        }
    }
}

esp_err_t wh_wifi_start(void)
{
    if (CONFIG_AA_WIFI_SSID[0] == '\0' || CONFIG_AA_WIFI_KEY[0] == '\0') {
        ESP_LOGE(TAG, "WiFi SSID/key not configured");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_led_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Create retry task */
    xTaskCreate(wifi_retry_task, "wifi_retry", 2048, NULL, 3, &s_retry_task);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_AA_WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_AA_WIFI_KEY,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA connecting to SSID=%s", CONFIG_AA_WIFI_SSID);
    return ESP_OK;
}
