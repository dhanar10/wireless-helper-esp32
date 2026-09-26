#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AA_SCN_HSP   1
#define AA_SCN_AA    8
#define AA_SCN_HFP   15

/** Called from SPP callback when AA (SCN 8) connection opens. */
void aa_handshake_on_open(uint32_t handle);

/** Feed received bytes for an AA connection. */
void aa_handshake_on_data(uint32_t handle, const uint8_t *data, uint16_t len);

/** Connection closed. */
void aa_handshake_on_close(uint32_t handle);

/** True while an AA RFCOMM session is active and not finished. */
bool aa_handshake_is_active(void);

/** True if an AA RFCOMM handle is still open (even after handshake done). */
bool aa_handshake_connection_open(void);

/** Disconnect AA RFCOMM if a handle is open (for knock re-trigger). */
void aa_handshake_force_close(void);

/** HFP: respond OK to AT commands. */
void aa_hfp_on_data(uint32_t handle, const uint8_t *data, uint16_t len);
