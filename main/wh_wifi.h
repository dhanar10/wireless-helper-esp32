#pragma once

#include "esp_err.h"

/** Start WiFi STA using Kconfig SSID/key. Starts knock on GOT_IP. */
esp_err_t wh_wifi_start(void);
