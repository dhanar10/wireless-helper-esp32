#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_bt_defs.h"
#include "esp_err.h"
#include "esp_sdp_api.h"
#include "esp_spp_api.h"

typedef enum {
    AA_POKE_ANSWERED = 0, /**< Connected and held (or cancelled mid-hold after connect). */
    AA_POKE_FAILED,       /**< All target UUIDs failed. */
    AA_POKE_CANCELLED,    /**< Cancelled before / during connect. */
} aa_poke_result_t;

esp_err_t aa_bt_client_init(void);

/**
 * SDP search + RFCOMM connect + hold for HFP-AG then HSP-AG.
 * Must run from a FreeRTOS task (blocks on semaphores / delays).
 */
aa_poke_result_t aa_bt_client_poke(const esp_bd_addr_t bda, uint32_t hold_ms);

/** Request cancel of in-flight poke (disconnect + abort waits). */
void aa_bt_client_cancel(void);

/** Clear cancel flag before a new poke. */
void aa_bt_client_clear_cancel(void);

/** Event hooks called from aa_bt.c callbacks. */
void aa_bt_client_on_sdp_search(esp_sdp_cb_param_t *param);
void aa_bt_client_on_spp_cl_init(esp_spp_cb_param_t *param);
void aa_bt_client_on_spp_open(esp_spp_cb_param_t *param);
void aa_bt_client_on_spp_close(esp_spp_cb_param_t *param);
