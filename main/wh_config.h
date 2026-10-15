#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_bt_defs.h"

#define WH_IP_MAX     16
#define WH_BSSID_MAX  18  /* "AA:BB:CC:DD:EE:FF" + NUL */

/** Phone AA TCP port (WifiStartRequest). */
#define AA_TCP_PORT   5288
/** Headunit knock listen port (subnet scan finds this open). */
#define AA_KNOCK_PORT 5289

/** Set AA TCP host IP from knock. */
void wh_config_set_ip(const char *ip);

/** Current knock IP, or "" if none. */
const char *wh_config_ip(void);

/** True when knock IP is set. */
bool wh_config_has_aa_host(void);

/** Store BSSID from STA associate (6 raw bytes → upper-case string). */
void wh_config_set_bssid(const uint8_t mac[6]);

void wh_config_clear_bssid(void);

/** True when a BSSID has been set from STA. */
bool wh_config_bssid_usable(void);

/** Current BSSID string, or "". */
const char *wh_config_bssid(void);

/** Store preferred phone BDA (6 bytes) on successful handshake.
 *  Optimized: skips NVS write if BDA unchanged. */
void wh_config_set_preferred_phone(const esp_bd_addr_t bda);

/** Get preferred phone BDA; returns true if set. */
bool wh_config_get_preferred_phone(esp_bd_addr_t out_bda);
