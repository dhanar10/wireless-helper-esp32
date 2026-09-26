#include "wh_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "wh_cfg";

static char s_ip[WH_IP_MAX];
static char s_bssid[WH_BSSID_MAX];

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
