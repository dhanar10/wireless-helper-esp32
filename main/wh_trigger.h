#pragma once

#include <stdbool.h>

/** Accept knock from IPv4 peer; ignored if busy or no BSSID. */
void wh_trigger_on_knock(const char *source_ip);

/** Handshake lifecycle (from aa_handshake). */
void wh_trigger_on_handshake_open(void);
void wh_trigger_on_handshake_complete(bool success);
void wh_trigger_on_handshake_abort(void);
