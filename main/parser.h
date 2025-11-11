#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_cred.h"

// Main entry: handle a NUL-terminated packet. conn_handle may be 0 for non-BLE sources (MQTT).
bool parser_handle_packet(const char *buf, uint16_t conn_handle);

// Raw buffer entry (BLE write might give a non-terminated buffer)
bool parser_handle_packet_buf(const uint8_t *buf, size_t len, uint16_t conn_handle);

// MQTT payload entry (payload pointer and length). Uses conn_handle == 0.
bool parser_handle_mqtt(const char *payload, size_t len);

// Utility extractors (public wrappers)
bool extract_str_field(const char *src, const char *key, char *out, size_t out_len);
bool extract_int_field(const char *src, const char *key, int *out);

#endif
