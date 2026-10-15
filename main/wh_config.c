#include "wh_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "aa_bt.h"

static const char *TAG = "wh_cfg";

static char s_ip[WH_IP_MAX];
static char s_bssid[WH_BSSID_MAX];

#define NVS_NAMESPACE "wh_cfg"
#define NVS_KEY_PREF_PHONE "pref_phone"

void wh_config_set_ip(const char *ip)
{
    snprintf(s_ip, sizeof(s_ip), "%s", ip ? ip : "");
    ESP_LOGI(TAG, "AA host IP set to %s", s_ip);
}

const char *wh_config_ip(void)
{
    return s_ip;
}

bool wh_config_has_aa_host(void)
{
    return s_ip[0] != '\0';
}

void wh_config_set_bssid(const uint8_t mac[6])
{
    if (!mac) {
        s_bssid[0] = '\0';
        return;
    }
    snprintf(s_bssid, sizeof(s_bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "BSSID set to %s", s_bssid);
}

void wh_config_clear_bssid(void)
{
    s_bssid[0] = '\0';
}

bool wh_config_bssid_usable(void)
{
    return s_bssid[0] != '\0';
}

const char *wh_config_bssid(void)
{
    return s_bssid;
}

void wh_config_set_preferred_phone(const esp_bd_addr_t bda)
{
    if (!bda) {
        ESP_LOGW(TAG, "set_preferred_phone: null BDA");
        return;
    }

    // Read current value to avoid unnecessary writes
    esp_bd_addr_t current;
    if (wh_config_get_preferred_phone(current)) {
        if (memcmp(current, bda, sizeof(esp_bd_addr_t)) == 0) {
            ESP_LOGD(TAG, "Preferred phone unchanged, skipping NVS write");
            return;
        }
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, NVS_KEY_PREF_PHONE, bda, sizeof(esp_bd_addr_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
    } else {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        } else {
            char bda_str[18];
            aa_bt_bda_str(bda, bda_str, sizeof(bda_str));
            ESP_LOGI(TAG, "Preferred phone saved: %s", bda_str);
        }
    }

    nvs_close(handle);
}

bool wh_config_get_preferred_phone(esp_bd_addr_t out_bda)
{
    if (!out_bda) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t required_size = sizeof(esp_bd_addr_t);
    err = nvs_get_blob(handle, NVS_KEY_PREF_PHONE, out_bda, &required_size);
    nvs_close(handle);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        }
        return false;
    }

    if (required_size != sizeof(esp_bd_addr_t)) {
        ESP_LOGW(TAG, "Preferred phone size mismatch: %u != %u", (unsigned)required_size, (unsigned)sizeof(esp_bd_addr_t));
        return false;
    }

    return true;
}
