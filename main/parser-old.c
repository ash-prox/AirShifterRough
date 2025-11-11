    #include "parser.h"
    #include "biz_logix.h"
    #include "esp_log.h"
    #include <string.h>
    #include <stdlib.h>
    #include <ctype.h>
    //#include "biz_logix.h"
    #include "wifi_cred.h"


    static const char *TAG = "parser";

    //
    // ────────────────────────────────
    //  Utility parsing functions
    // ────────────────────────────────
    //

    // Extracts a quoted string or word following a key (e.g. "SSID:MyNet")
    bool extract_str_field(const char *src, const char *key, char *out, size_t out_len)
    {
        if (!src || !key || !out) return false;

        const char *pos = strstr(src, key);
        if (!pos) return false;

        pos += strlen(key);

        // Skip delimiters (':' or '=' or space)
        while (*pos && (*pos == ':' || *pos == '=' || isspace((unsigned char)*pos)))
            pos++;

        // If string starts with a quote
        if (*pos == '\'' || *pos == '"') {
            char quote = *pos++;
            const char *end = strchr(pos, quote);
            if (!end) end = pos + strlen(pos);
            size_t len = end - pos;
            if (len >= out_len) len = out_len - 1;
            strncpy(out, pos, len);
            out[len] = '\0';
            return true;
        }

        // Otherwise copy until comma or space
        const char *end = pos;
        while (*end && *end != ',' && !isspace((unsigned char)*end))
            end++;
        size_t len = end - pos;
        if (len >= out_len) len = out_len - 1;
        strncpy(out, pos, len);
        out[len] = '\0';
        return true;
    }

    // Extracts an integer after a key (e.g. "Speed:120")
    bool extract_int_field(const char *src, const char *key, int *out)
    {
        if (!src || !key || !out) return false;

        const char *pos = strstr(src, key);
        if (!pos) return false;

        pos += strlen(key);
        /* Skip separators, whitespace and optional quotes/braces/comma between key and value
         * Allows matching formats like: "Speed" : 100  or  Speed:100  or  "Speed": "100"
         */
        while (*pos && (
            *pos == ':' || *pos == '=' || isspace((unsigned char)*pos) ||
            *pos == '"' || *pos == '\'' || *pos == '{' || *pos == '}' || *pos == ','))
        {
            pos++;
        }

        if (!*pos) return false;

        /* Now pos should point at a digit (or optional minus sign). If it's a quote, skip it. */
        if (*pos == '"' || *pos == '\'') {
            pos++;
        }

        if (!isdigit((unsigned char)*pos) && *pos != '-') return false;

        *out = atoi(pos);
        return true;
    }

    //
    // ────────────────────────────────
    //  Core parser function
    // ────────────────────────────────
    //

    /* Normalize packet formats such as: {{Fan1},{"Speed": 100}} -> {"Speed": 100}
     * If the packet contains a '},{' sequence we assume the payload follows it.
     */
    static void normalize_packet(const char *src, char *out, size_t out_len)
    {
        if (!src || !out || out_len == 0) return;
        const char *p = strstr(src, "},{");
        if (p) {
            p += 3; // skip '},{'
        } else {
            p = src;
        }
        while (*p && isspace((unsigned char)*p)) p++;

        size_t total = strlen(p);
        if (total == 0) {
            out[0] = '\0';
            return;
        }

        /* Count trailing '}' characters */
        size_t trailing = 0;
        for (size_t i = total; i > 0; i--) {
            if (p[i-1] == '}') trailing++; else break;
        }

        size_t copy_len = total;
        /* If there are multiple trailing braces keep one, drop extras */
        if (trailing > 1) copy_len = total - (trailing - 1);
        if (copy_len >= out_len) copy_len = out_len - 1;
        strncpy(out, p, copy_len);
        out[copy_len] = '\0';
    }

    bool parser_handle_packet(const char *buf, uint16_t conn_handle)
    {
        bool handled = false;
        if (!buf) return false;

        char norm[256];
        normalize_packet(buf, norm, sizeof(norm));
        const char *p = norm;
        ESP_LOGI(TAG, "Parsing packet (normalized): %s", p);

        /* Check for auth key first - handle both nested and flat JSON */
        char keybuf[32];
        if (strstr(p, "AuthKey")) {
            /* Try more flexible auth key extraction */
            const char *auth_start = strstr(p, "AuthKey");
            if (auth_start) {
                /* Skip to the value part */
                auth_start = strchr(auth_start, ':');
                if (auth_start) {
                    /* Skip : and any whitespace */
                    auth_start++;
                    while (*auth_start && (*auth_start == ' ' || *auth_start == '"'))
                        auth_start++;
                    
                    /* Find the end (quote or brace or comma) */
                    const char *auth_end = auth_start;
                    while (*auth_end && *auth_end != '"' && *auth_end != '}' && *auth_end != ',')
                        auth_end++;

                    /* Copy the key value */
                    size_t key_len = auth_end - auth_start;
                    if (key_len > 0 && key_len < sizeof(keybuf)) {
                        strncpy(keybuf, auth_start, key_len);
                        keybuf[key_len] = '\0';
                        ESP_LOGI(TAG, "Found AuthKey: %s", keybuf);
                        handled = biz_verify_auth_key(conn_handle, keybuf);
                        return handled;
                    }
                }
            }
        }

    if (strstr(p, "Wifi") || strstr(p, "SSID") || strstr(p, "ssid")) {
            wifi_credentials_t cred = {0};
            char ssid[33] = {0};
            char pass[65] = {0};

            bool got_ssid = extract_str_field(p, "SSID", ssid, sizeof(ssid));
            if (!got_ssid) got_ssid = extract_str_field(p, "ssid", ssid, sizeof(ssid));

            bool got_pass = extract_str_field(p, "PASS", pass, sizeof(pass)) ||
                            extract_str_field(p, "Pass", pass, sizeof(pass)) ||
                            extract_str_field(p, "pass", pass, sizeof(pass));

            if (got_ssid) {
                strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
                cred.ssid_len = strlen(cred.ssid);

                if (got_pass) {
                    strncpy(cred.pass, pass, sizeof(cred.pass) - 1);
                    cred.pass_len = strlen(cred.pass);
                }

                ESP_LOGI(TAG, "Parsed WiFi credentials: SSID='%s'", cred.ssid);
                biz_apply_wifi_credentials(&cred);
                handled = true;
            }
            return handled;
        }

        // // Control command parsing (require per-connection auth)
        // if (!biz_is_authenticated(conn_handle)) {
        //     ESP_LOGW(TAG, "Rejected command: not authenticated (conn=%u)", conn_handle);
        //     return false;
        // }
        int val;
        if (extract_int_field(p, "Speed", &val) || extract_int_field(p, "speed", &val)) {
            biz_set_rpm(val);
            handled = true;
        }
        if (extract_int_field(p, "Angle", &val) || extract_int_field(p, "angle", &val)) {
            biz_set_angle(val);
            handled = true;
        }
        if (extract_int_field(p, "Light", &val) || extract_int_field(p, "light", &val)) {
            biz_set_light(val);
            handled = true;
        }
        if (extract_int_field(p, "Power", &val) || extract_int_field(p, "power", &val)) {
            biz_set_power(val);
            handled = true;
        }

        return handled;
    }
