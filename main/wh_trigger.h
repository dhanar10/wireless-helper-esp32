#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_bt_defs.h"

/** Accept knock from IPv4 peer; ignored if busy or no BSSID. */
void wh_trigger_on_knock(const char *source_ip);

/** Handshake lifecycle (from aa_handshake). */
void wh_trigger_on_handshake_open(const esp_bd_addr_t bda);
void wh_trigger_on_handshake_complete(bool success);
void wh_trigger_on_handshake_abort(void);
