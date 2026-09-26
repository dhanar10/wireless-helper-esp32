#include "wh_wifi.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "wh_config.h"
#include "wh_knock.h"

static const char *TAG = "wh_wifi";

/** Onboard LED on many ESP32 DevKit boards (active high). */
#define WH_WIFI_LED_GPIO GPIO_NUM_2

static int s_retry;
static bool s_knock_started;

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

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_led_set(false);
        s_retry++;
        wh_config_clear_bssid();
        ESP_LOGW(TAG, "Disconnected — retry %d", s_retry);
        esp_wifi_connect();
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
    s_retry = 0;

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
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

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
