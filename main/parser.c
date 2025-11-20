#include <inttypes.h>

// parser.c  -- jsmn-based packet parser with fallback
/*
 Rudimentary packet JSON design:
 {
     "device": "device-id-123",   // optional device identifier
     "ts": 1699152000,             // optional epoch timestamp (seconds)
     "speed": 120,                 // rpm or speed
     "angle": 45,
     "light": 1,
     "power": 1,
     // for Wi-Fi provisioning
     "ssid": "MyAP",
     "pass": "secret"
 }

 Parser entry points:
 - parser_handle_packet(buf, conn_handle): handle a NUL-terminated packet; conn_handle=0 for MQTT
 - parser_handle_packet_buf(buf,len,conn_handle): handle raw buffer (BLE)
 - parser_handle_mqtt(payload,len): convenience for MQTT payloads
*/
#include "parser.h"
#include "biz_logix.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "wifi_cred.h"

/* jsmn header - include jsmn.h in your project and build jsmn.c too */
#include "jsmn.h"

static const char *TAG = "parser";

/* How many tokens to allow for a typical packet. Increase if your JSON is larger. */
#define JSMN_TOKEN_MAX 128

/* Helper: compare token (in buf) to a null-terminated key, case-insensitive.
 * Returns 1 if equal, 0 otherwise.
 */
static int token_equals_key(const char *buf, const jsmntok_t *tok, const char *key)
{
    size_t keylen = strlen(key);
    if ((size_t)(tok->end - tok->start) != keylen) return 0; //if the length is all wonky, ret 0
    for (size_t i = 0; i < keylen; ++i) {
        char a = buf[tok->start + i]; //
        char b = key[i];
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return 0; //comparing key with the token (b is key, a contains the token char)
    }
    return 1;
}

/* Helper: copy token text to a null-terminated buffer safely */
static void token_to_cstr(const char *buf, const jsmntok_t *tok, char *out, size_t out_len)
{
    size_t len = (size_t)(tok->end - tok->start);
    if (len >= out_len) len = out_len - 1;
    if (len > 0) memcpy(out, buf + tok->start, len);
    out[len] = '\0';
}

/* Helper: parse integer from token. Returns true on success. Handles numbers or strings. */
static int token_to_int(const char *buf, const jsmntok_t *tok, int *out)
{
    char tmp[32];
    size_t len = (size_t)(tok->end - tok->start);
    if (len == 0 || len >= sizeof(tmp)) return 0;
    memcpy(tmp, buf + tok->start, len);
    tmp[len] = '\0';
    /* skip leading spaces */
    char *p = tmp;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p == '\0') return 0;
    /* accept "-"/digits */
    char *endptr = NULL;
    long v = strtol(p, &endptr, 10);
    if (p == endptr) return 0; /* no conversion */
    *out = (int)v;
    return 1;
}

/* Core JSON parser using jsmn */
static bool parser_handle_packet_json(const char *buf, uint16_t conn_handle)
{
    jsmn_parser p;
    jsmntok_t toks[JSMN_TOKEN_MAX];
    jsmn_init(&p);
    int r = jsmn_parse(&p, buf, strlen(buf), toks, JSMN_TOKEN_MAX);
    if (r < 0) {
        ESP_LOGW(TAG, "jsmn_parse failed (%d) - not JSON or too many tokens", r);
        return false;
    }
    if (r == 0) {
        /* no tokens */
        return false;
    }

    /* We expect top-level object(s). Walk tokens. jsmn returns tokens in array order.
       When we see a string token in object position, next token is the value. */
    bool handled = false;
    char parsed_device[64] = {0};
    bool got_device = false;
    int64_t parsed_ts = 0;
    bool got_ts = false;

    for (int i = 0; i < r; ++i) {
        jsmntok_t *t = &toks[i];

        /* find object key tokens (type STRING inside an object) */
        if (t->type == JSMN_STRING) {
            /* value token should be next (if within bounds) */
            if (i + 1 >= r) continue;
            jsmntok_t *val = &toks[i + 1];

            /* keys we care about (case-insensitive) */
            if (token_equals_key(buf, t, "ssid") || token_equals_key(buf, t, "SSID")) {
                char ssid[sizeof(((wifi_credentials_t*)0)->ssid)];
                token_to_cstr(buf, val, ssid, sizeof(ssid));
                if (ssid[0]) {
                    wifi_credentials_t cred = {0};
                    strncpy(cred.ssid, ssid, sizeof(cred.ssid)-1);
                    cred.ssid_len = (uint8_t)strlen(cred.ssid);
                    /* look if pass already present elsewhere; here we try to find PASS in same object */
                    /* We'll search for pass below; for now queue cred if pass absent (allowed) */
                    /* To avoid queuing twice, we will collect and apply only after scanning entire JSON.
                       So store into temporary place instead. */
                    /* We'll implement a simple scan: gather ssid/pass variables locally. */
                }
            }
            /* Increment i to skip the value token in next iterate - but because we need to
               scan for multiple keys, we shouldn't skip entirely; we'll let loop increment normally.
               But to avoid re-reading the value as key, increment i here. */
            /* Actually, jsmn returns tokens in order, so following value token could be another key
               only if current token was a key in an object; safe to skip the value token. */
            ++i; /* skip value token */
        }
    }

    /* The above loop is just a template - we need to do a proper gather pass so SSID and PASS can both be read.
       Do a second pass which actually applies keys. */

    char found_ssid[sizeof(((wifi_credentials_t*)0)->ssid)] = {0};
    char found_pass[sizeof(((wifi_credentials_t*)0)->pass)] = {0};
    bool got_ssid = false;
    bool got_pass = false;

    for (int i = 0; i < r; ++i) {
        jsmntok_t *t = &toks[i];
        if (t->type != JSMN_STRING) continue;
        if (i + 1 >= r) continue;
        jsmntok_t *val = &toks[i + 1];

        if (token_equals_key(buf, t, "ssid") ||
            token_equals_key(buf, t, "SSID") ||
            token_equals_key(buf, t, "wifi") ||
            token_equals_key(buf, t, "Wifi") ) {
            token_to_cstr(buf, val, found_ssid, sizeof(found_ssid));
            if (found_ssid[0]) got_ssid = true;
            ++i; continue;
        }

        if (token_equals_key(buf, t, "pass") ||
            token_equals_key(buf, t, "PASS") ||
            token_equals_key(buf, t, "password") ||
            token_equals_key(buf, t, "pwd")) {
            token_to_cstr(buf, val, found_pass, sizeof(found_pass));
            if (found_pass[0]) got_pass = true;
            ++i; continue;
        }

        /* Control keys */
        if (token_equals_key(buf, t, "speed") ||
            token_equals_key(buf, t, "Speed")) {
            int v = 0;
            if (token_to_int(buf, val, &v)) {
                biz_set_rpm(v);
                handled = true;
            }
            ++i; continue;
        }
        if (token_equals_key(buf, t, "angle") ||
            token_equals_key(buf, t, "Angle")) {
            int v = 0;
            if (token_to_int(buf, val, &v)) {
                biz_set_angle(v);
                handled = true;
            }
            ++i; continue;
        }
        if (token_equals_key(buf, t, "light") ||
            token_equals_key(buf, t, "Light")) {
            int v = 0;
            if (token_to_int(buf, val, &v)) {
                biz_set_light(v);
                handled = true;
            }
            ++i; continue;
        }
        if (token_equals_key(buf, t, "power") ||
            token_equals_key(buf, t, "Power")) {
            int v = 0;
            if (token_to_int(buf, val, &v)) {
                biz_set_power(v);
                handled = true;
            }
            ++i; continue;
        }

        /* device id */
        if (token_equals_key(buf, t, "device") || token_equals_key(buf, t, "device_id") || token_equals_key(buf, t, "id")) {
            token_to_cstr(buf, val, parsed_device, sizeof(parsed_device));
            if (parsed_device[0]) got_device = true;
            ++i; continue;
        }

        /* timestamp (epoch seconds) */
        if (token_equals_key(buf, t, "ts") || token_equals_key(buf, t, "time") || token_equals_key(buf, t, "epoch")) {
            int tmp = 0;
            if (token_to_int(buf, val, &tmp)) {
                parsed_ts = (int64_t)tmp;
                got_ts = true;
            }
            ++i; continue;
        }

        /* Otherwise ignore unknown keys */
    }

    /* If SSID present, apply wifi credentials */
    if (got_ssid) {
        wifi_credentials_t cred = {0};
        strncpy(cred.ssid, found_ssid, sizeof(cred.ssid)-1);
        cred.ssid_len = (uint8_t)strlen(cred.ssid);
        if (got_pass) {
            strncpy(cred.pass, found_pass, sizeof(cred.pass)-1);
            cred.pass_len = (uint8_t)strlen(cred.pass);
        } else {
            cred.pass_len = 0;
            cred.pass[0] = '\0';
        }
        ESP_LOGI(TAG, "Parsed WiFi credentials (JSON): SSID='%s' pass_len=%u", cred.ssid, cred.pass_len);
        biz_apply_wifi_credentials(&cred);
        handled = true;
    }

    return handled;
}

/* --- existing legacy extract helpers (kept for fallback). These are simplified versions
   of your previous functions so we can fallback when buffer isn't JSON. --- */

static bool legacy_extract_str_field(const char *src, const char *key, char *out, size_t out_len)
{
    if (!src || !key || !out) return false;
    const char *pos = strstr(src, key);
    if (!pos) return false;
    pos += strlen(key);
    while (*pos && (*pos == ':' || *pos == '=' || isspace((unsigned char)*pos))) pos++;
    if (*pos == '"' || *pos == '\'') {
        char q = *pos++;
        size_t i = 0;
        while (*pos && *pos != q && i + 1 < out_len) out[i++] = *pos++;
        out[i] = '\0';
        return i > 0;
    }
    size_t i = 0;
    while (*pos && *pos != ',' && !isspace((unsigned char)*pos) && i + 1 < out_len) out[i++] = *pos++;
    out[i] = '\0';
    return i > 0;
}

static bool legacy_extract_int_field(const char *src, const char *key, int *out)
{
    if (!src || !key || !out) return false;
    const char *pos = strstr(src, key);
    if (!pos) return false;
    pos += strlen(key);
    while (*pos && (*pos == ':' || *pos == '=' || isspace((unsigned char)*pos))) pos++;
    if (!*pos) return false;
    if (!isdigit((unsigned char)*pos) && *pos != '-') return false;
    *out = atoi(pos);
    return true;
}

/* Main public API - tries JSON first, falls back to legacy parsing. */
bool parser_handle_packet(const char *buf, uint16_t conn_handle)
{
    if (!buf) return false;

    /* Skip leading whitespace */
    const char *p = buf;
    while (*p && isspace((unsigned char)*p)) ++p;
    bool handled = false;

    /* If it looks like JSON (starts with '{' or '['), use jsmn */
    if (*p == '{' || *p == '[') {
        handled = parser_handle_packet_json(p, conn_handle);
        if (handled) return true;
        /* if jsmn failed to find keys, fall through to legacy */
    }

    /* Legacy (non-JSON) processing to remain compatible with older clients */
    /* Wifi/provisioning */
    if (strstr(buf, "Wifi") || strstr(buf, "SSID") || strstr(buf, "ssid")) {
        wifi_credentials_t cred = {0};
        char ssid[33] = {0};
        char pass[65] = {0};

        bool got_ssid = legacy_extract_str_field(buf, "SSID", ssid, sizeof(ssid));
        if (!got_ssid) got_ssid = legacy_extract_str_field(buf, "ssid", ssid, sizeof(ssid));
        if (!got_ssid) got_ssid = legacy_extract_str_field(buf, "Wifi", ssid, sizeof(ssid));
        if (!got_ssid) got_ssid = legacy_extract_str_field(buf, "wifi", ssid, sizeof(ssid));

        bool got_pass = legacy_extract_str_field(buf, "PASS", pass, sizeof(pass)) ||
                        legacy_extract_str_field(buf, "Pass", pass, sizeof(pass)) ||
                        legacy_extract_str_field(buf, "pass", pass, sizeof(pass)) ||
                        legacy_extract_str_field(buf, "password", pass, sizeof(pass));

        if (got_ssid) {
            strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
            cred.ssid_len = strlen(cred.ssid);

            if (got_pass) {
                strncpy(cred.pass, pass, sizeof(cred.pass) - 1);
                cred.pass_len = strlen(cred.pass);
            }

            ESP_LOGI(TAG, "Parsed WiFi credentials (legacy): SSID='%s' pass_len=%u", cred.ssid, cred.pass_len);
            biz_apply_wifi_credentials(&cred);
            return true;
        }
    }

    /* Control commands (legacy) */
    int val;
    if (legacy_extract_int_field(buf, "Speed", &val) || legacy_extract_int_field(buf, "speed", &val)) {
        biz_set_rpm(val);
        handled = true;
    }
    if (legacy_extract_int_field(buf, "Angle", &val) || legacy_extract_int_field(buf, "angle", &val)) {
        biz_set_angle(val);
        handled = true;
    }
    if (legacy_extract_int_field(buf, "Light", &val) || legacy_extract_int_field(buf, "light", &val)) {
        biz_set_light(val);
        handled = true;
    }
    if (legacy_extract_int_field(buf, "Power", &val) || legacy_extract_int_field(buf, "power", &val)) {
        biz_set_power(val);
        handled = true;
    }

    return handled;
}


/* Handle a raw buffer (not NUL-terminated) from BLE characteristic writes */
bool parser_handle_packet_buf(const uint8_t *buf, size_t len, uint16_t conn_handle)
{
    if (!buf || len == 0) return false;
    /* limit to reasonable size to avoid huge allocs */
    size_t max = len;
    char *tmp = malloc(max + 1);
    if (!tmp) return false;
    memcpy(tmp, buf, max);
    tmp[max] = '\0';
    bool res = parser_handle_packet(tmp, conn_handle);
    free(tmp);
    return res;
}

/* Handle MQTT payloads (payload may not be NUL-terminated) */
bool parser_handle_mqtt(const char *payload, size_t len)
{
    if (!payload || len == 0) return false;
    char *tmp = malloc(len + 1);
    if (!tmp) return false;
    memcpy(tmp, payload, len);
    tmp[len] = '\0';
    /* conn_handle == 0 indicates non-BLE source */
    bool res = parser_handle_packet(tmp, 0);
    free(tmp);
    return res;
}

/* Public wrappers for legacy extractors */
bool extract_str_field(const char *src, const char *key, char *out, size_t out_len)
{
    return legacy_extract_str_field(src, key, out, out_len);
}

bool extract_int_field(const char *src, const char *key, int *out)
{
    return legacy_extract_int_field(src, key, out);
}
