#include "security.h"
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
//#include "os_mbuf.h"

/* You must export/declare this in gatt_svr.c:
 * extern uint16_t auth_char_handle;
 * The auth characteristic should have notify + write properties (val_handle stored in auth_char_handle).
 */

/* If you prefer the server to hold the value buffer itself, keep this static */
static const char *TAG = "security";

/* Default private key (for testing). Replace with secure storage in production. */
static uint8_t *s_priv_key = NULL;
static size_t s_priv_key_len = 0;

/* Per-connection state — small fixed table keyed by conn_handle.
 * NimBLE conn_handle fits in 16-bit; however we keep a small mapping
 * of up to N concurrent connections (N = configurable).
 */
#define MAX_SEC_CONNS 6
static struct {
    uint16_t conn_handle;
    uint8_t nonce[32]; /* allow up to 32 byte nonces */
    size_t nonce_len;
    uint64_t created_ts_ms; /* for expiry */
    bool authenticated;
} s_conns[MAX_SEC_CONNS];

/* default nonce and HMAC sizes */
static size_t s_nonce_len = 16;
static const size_t HMAC_SHA256_LEN = 32;

/* nonce lifetime in ms */
static const uint64_t NONCE_LIFETIME_MS = 60 * 1000 * 5; /* 5 minutes */

/* helper: get current millis */
static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

/* helper: find or allocate entry for conn_handle */
static int find_entry(uint16_t conn_handle)
{
    for (int i = 0; i < MAX_SEC_CONNS; i++) {
        if (s_conns[i].conn_handle == conn_handle) return i;
    }
    return -1;
}
static int alloc_entry(uint16_t conn_handle)
{
    for (int i = 0; i < MAX_SEC_CONNS; i++) {
        if (s_conns[i].conn_handle == 0) {
            s_conns[i].conn_handle = conn_handle;
            s_conns[i].authenticated = false;
            s_conns[i].nonce_len = 0;
            s_conns[i].created_ts_ms = 0;
            return i;
        }
    }
    return -1;
}
static void free_entry_index(int idx)
{
    if (idx < 0 || idx >= MAX_SEC_CONNS) return;
    memset(&s_conns[idx], 0, sizeof(s_conns[idx]));
}

/* generate nonce into entry */
static esp_err_t generate_nonce_for_index(int idx)
{
    if (idx < 0 || idx >= MAX_SEC_CONNS) return ESP_ERR_INVALID_ARG;
    if (s_nonce_len > sizeof(s_conns[idx].nonce)) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < s_nonce_len; i += 4) {
        uint32_t r = esp_random();
        size_t rem = (s_nonce_len - i) >= 4 ? 4 : (s_nonce_len - i);
        memcpy(&s_conns[idx].nonce[i], &r, rem);
    }
    s_conns[idx].nonce_len = s_nonce_len;
    s_conns[idx].created_ts_ms = now_ms();
    s_conns[idx].authenticated = false;
    return ESP_OK;
}

/* compute HMAC-SHA256 into out (must be at least 32 bytes) */
static bool compute_hmac_sha256(const uint8_t *key, size_t key_len,
                                const uint8_t *data, size_t data_len,
                                uint8_t *out /* 32 bytes */)
{
    if (!key || !data || !out) return false;
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return false;
    int rc = mbedtls_md_hmac(md_info, key, (int)key_len, data, data_len, out);
    return rc == 0;
}

/* update the auth characteristic value and notify subscribers.
 * This writes the nonce bytes as the characteristic value and calls
 * ble_gatts_chr_updated(auth_char_handle) to trigger notify (if subscribed).
 *
 * gatt_svr must declare `extern uint16_t auth_char_handle;`
 */
// extern uint16_t auth_char_handle; /* must be defined in gatt_svr.c */
// extern uint8_t auth_value[32];
// extern uint16_t auth_val_len;
// extern uint16_t auth_char_handle;   /* must be defined in gatt_svr.c */

static void update_auth_char_value_and_notify(uint16_t conn_handle,
                                              const uint8_t *nonce, size_t nonce_len)
{
    /* NimBLE will use the characteristic's value buffer that you set via regular GATT flow.
       Simple approach: write the server attribute value by calling ble_gatts_chr_write() does not exist;
       Instead, we update the characteristic storage used by your gatt implementation.
       The minimal approach below assumes your GATT characteristic read handler reads
       from an application buffer. Alternatively, call ble_gatts_chr_updated() (which notifies
       subscribed centrals with current characteristic value stored by the stack).
       How to set the value so the stack has it: write into the attribute's value in the GATT store.
       NimBLE provides ble_gatts_set_value() in some implementations; if not available, update your
       gatt code to use a backing buffer that this module can write to.
    */

    /* Practical approach: use os_mbuf to send a notification directly */
    // struct os_mbuf *om = ble_hs_mbuf_from_flat((const void *)nonce, nonce_len);
    // if (!om) {
    //     ESP_LOGW(TAG, "Failed to allocate ombuf for nonce notify");
    //     return;
    // }
    // int rc = ble_gatts_notify_custom(conn_handle, auth_char_handle, om);
    // if (rc != 0) {
    //     ESP_LOGW(TAG, "ble_gatts_notify_custom failed rc=%d", rc);
    // } else {
    //     ESP_LOGI(TAG, "Nonce notified to conn=%u len=%u", conn_handle, (unsigned)nonce_len);
    // }

        if (nonce_len > sizeof(auth_value)) nonce_len = sizeof(auth_value);
    memcpy(auth_value, nonce, nonce_len);
    auth_val_len = nonce_len;
    ble_gatts_chr_updated(auth_char_handle);  // notify all subscribed clients
    ESP_LOGI(TAG, "Nonce updated & notification sent (%u bytes)", (unsigned)nonce_len);
}


/* Public functions */

void sec_init(void)
{
    memset(s_conns, 0, sizeof(s_conns));

    /* Default private key (replace for production) */
    static uint8_t default_key[] = "fan12345"; /* example; override with sec_set_private_key() */
    s_priv_key_len = sizeof(default_key) - 1;
    s_priv_key = malloc(s_priv_key_len);
    memcpy(s_priv_key, default_key, s_priv_key_len);

    ESP_LOGI(TAG, "sec_init done (nonce_len=%u)", (unsigned)s_nonce_len);
}

void sec_set_nonce_len(size_t nonce_len)
{
    if (nonce_len == 0 || nonce_len > sizeof(s_conns[0].nonce)) return;
    s_nonce_len = nonce_len;
}

void sec_set_private_key(const uint8_t *key, size_t key_len)
{
    if (!key || key_len == 0) return;
    if (s_priv_key) free(s_priv_key);
    s_priv_key = malloc(key_len);
    memcpy(s_priv_key, key, key_len);
    s_priv_key_len = key_len;
    ESP_LOGI(TAG, "Private key updated (len=%u)", (unsigned)s_priv_key_len);
}

void sec_on_connect(uint16_t conn_handle)
{
    /* allocate or find slot */
    int idx = find_entry(conn_handle);
    if (idx < 0) idx = alloc_entry(conn_handle);
    if (idx < 0) {
        ESP_LOGW(TAG, "sec_on_connect: no slot for conn %u", conn_handle);
        return;
    }
    /* generate nonce */
    if (generate_nonce_for_index(idx) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to generate nonce");
        return;
    }
    /* notify the client with new nonce (if subscribed) */
    update_auth_char_value_and_notify(conn_handle, s_conns[idx].nonce, s_conns[idx].nonce_len);

    ESP_LOGI(TAG, "Nonce generated and (attempted) notify for conn=%u", conn_handle);
}

bool sec_handle_auth_response(uint16_t conn_handle, const uint8_t *hmac, size_t hmac_len)
{
    int idx = find_entry(conn_handle);
    if (idx < 0) {
        ESP_LOGW(TAG, "sec_handle_auth_response: unknown conn %u", conn_handle);
        return false;
    }

    /* check nonce expiry */
    if (now_ms() - s_conns[idx].created_ts_ms > NONCE_LIFETIME_MS) {
        ESP_LOGW(TAG, "sec_handle_auth_response: nonce expired for conn %u", conn_handle);
        return false;
    }

    if (!hmac || hmac_len != HMAC_SHA256_LEN) {
        ESP_LOGW(TAG, "sec_handle_auth_response: invalid hmac length %u", (unsigned)hmac_len);
        return false;
    }

    uint8_t expected[HMAC_SHA256_LEN];
    if (!compute_hmac_sha256(s_priv_key, s_priv_key_len, s_conns[idx].nonce, s_conns[idx].nonce_len, expected)) {
        ESP_LOGE(TAG, "HMAC compute failed");
        return false;
    }

    /* constant-time compare */
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < HMAC_SHA256_LEN; ++i) {
        diff |= expected[i] ^ hmac[i];
    }
    if (diff == 0) {
        s_conns[idx].authenticated = true;
        ESP_LOGI(TAG, "Conn %u authenticated successfully", conn_handle);
        return true;
    } else {
        ESP_LOGW(TAG, "Conn %u authentication failed (HMAC mismatch)", conn_handle);
        return false;
    }
}

bool sec_is_conn_authenticated(uint16_t conn_handle)
{
    int idx = find_entry(conn_handle);
    if (idx < 0) return false;
    /* Check expiry too */
    if (s_conns[idx].authenticated && now_ms() - s_conns[idx].created_ts_ms <= NONCE_LIFETIME_MS) {
        return true;
    }
    return false;
}

void sec_clear_conn(uint16_t conn_handle)
{
    int idx = find_entry(conn_handle);
    if (idx >= 0) free_entry_index(idx);
}

void sec_send_nonce_notify(uint16_t conn_handle)
{
    int idx = find_entry(conn_handle);
    if (idx < 0) return;
    update_auth_char_value_and_notify(conn_handle, s_conns[idx].nonce, s_conns[idx].nonce_len);
}

int sec_get_auth_value(uint16_t conn_handle, uint8_t *out_nonce)
{
    if (!out_nonce) return -1;
    int idx = find_entry(conn_handle);
    if (idx < 0) return -1;
    if (s_conns[idx].nonce_len == 0) return -1;
    size_t len = s_conns[idx].nonce_len;
    memcpy(out_nonce, s_conns[idx].nonce, len);
    return (int)len;
}

