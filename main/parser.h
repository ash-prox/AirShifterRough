#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_cred.h"

bool parser_handle_packet(const char *buf, uint16_t conn_handle);

// Utility extractors
bool extract_str_field(const char *src, const char *key, char *out, size_t out_len);
bool extract_int_field(const char *src, const char *key, int *out);

#endif
