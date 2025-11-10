#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include "host/ble_hs.h" /* for conn handle type */
#include "wifi_cred.h"   /* if you want to reuse wifi_cred_t, not required */

extern uint8_t auth_value[32];
extern uint16_t auth_val_len;
extern uint16_t auth_char_handle;






#ifdef __cplusplus
extern "C" {
#endif

/* Initialize security module (call once at startup) */
void sec_init(void);

/* Called when a new BLE connection is established (conn_handle from NimBLE) */
void sec_on_connect(uint16_t conn_handle);

/* Handle auth response received from client (raw HMAC bytes) */
bool sec_handle_auth_response(uint16_t conn_handle, const uint8_t *hmac, size_t hmac_len);

/* Query if a given connection is authenticated */
bool sec_is_conn_authenticated(uint16_t conn_handle);

/* Clear authentication state for a given connection (call on disconnect) */
void sec_clear_conn(uint16_t conn_handle);

/* If you want to change key at runtime */
void sec_set_private_key(const uint8_t *key, size_t key_len);

/* Configure nonce size (default 16 bytes) */
void sec_set_nonce_len(size_t nonce_len);

/* Optional: send nonce explicitly for a connection (forces update of auth characteristic) */
void sec_send_nonce_notify(uint16_t conn_handle);

/* Get the current auth nonce value for a connection */
int sec_get_auth_value(uint16_t conn_handle, uint8_t *out_nonce);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_H */
