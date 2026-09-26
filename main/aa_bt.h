#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_bt_defs.h"
#include "esp_err.h"

#define AA_BT_MAX_BONDED 16

/** Initialize classic BT, GAP (SSP auto-confirm), SPP servers, and SDP records. */
esp_err_t aa_bt_start(void);

/** True once SPP servers and local SDP records are up. */
bool aa_bt_is_ready(void);

/** Fill out[] with bonded device addresses. Returns count written (≤ max). */
size_t aa_bt_get_bonded(esp_bd_addr_t *out, size_t max);

/** Format BDA into "aa:bb:…" (needs ≥18 bytes). */
char *aa_bt_bda_str(const esp_bd_addr_t bda, char *str, size_t size);
