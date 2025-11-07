// biz_logix.c
#include "biz_logix.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "wifi_cred.h"
#include "nvs_flash.h"
#include "nvs.h"

/* NimBLE notification API - declare if header not included elsewhere.
   If you prefer, include the correct NimBLE header instead. */
extern void ble_gatts_chr_updated(uint16_t chr_val_handle);

static const char *TAG = "biz_logix";

/* Authentication state (per-connection) and default key */
#include "host/ble_hs.h"

#define MAX_BLE_CONNECTIONS 4

typedef struct {
    uint16_t conn_handle;
    bool authenticated;
} auth_state_t;

static auth_state_t g_auth_table[MAX_BLE_CONNECTIONS];
static const char *DEVICE_AUTH_KEY = "fan12345";  // fixed key (inside app)

static auth_state_t* get_auth_state(uint16_t conn_handle)
{
    /* look for existing */
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (g_auth_table[i].conn_handle == conn_handle)
            return &g_auth_table[i];
    }

    /* allocate new slot if available */
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (g_auth_table[i].conn_handle == BLE_HS_CONN_HANDLE_NONE || g_auth_table[i].conn_handle == 0) {
            g_auth_table[i].conn_handle = conn_handle;
            g_auth_table[i].authenticated = false;
            return &g_auth_table[i];
        }
    }

    ESP_LOGW(TAG, "No free auth slots");
    return NULL;
}

bool biz_verify_auth_key(uint16_t conn_handle, const char *key)
{
    auth_state_t *s = get_auth_state(conn_handle);
    if (!s) return false;

    if (key && strcmp(key, DEVICE_AUTH_KEY) == 0) {
        s->authenticated = true;
        ESP_LOGI(TAG, "Auth success for conn_handle=%u", conn_handle);
        return true;
    }

    ESP_LOGW(TAG, "Auth failed for conn_handle=%u", conn_handle);
    return false;
}

bool biz_is_authenticated(uint16_t conn_handle)
{
    auth_state_t *s = get_auth_state(conn_handle);
    return s ? s->authenticated : false;
}

void biz_clear_auth(uint16_t conn_handle)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (g_auth_table[i].conn_handle == conn_handle) {
            g_auth_table[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_auth_table[i].authenticated = false;
        }
    }
}

/* --- State variables --- */
/* control (desired) */
static uint32_t g_ctrl_rpm = 0;
static uint32_t g_ctrl_angle = 0;
static uint8_t  g_ctrl_light = 0;
static uint8_t  g_ctrl_power = 0;

/* status (reported) */
static uint32_t g_stat_rpm = 0;
static uint32_t g_stat_angle = 0;
static uint8_t  g_stat_light = 0;
static uint8_t  g_stat_power = 0;

/* --- Externed handles (defined in gatt_svr.c) --- */
extern uint16_t ctrl_rpm_handle;
extern uint16_t ctrl_angle_handle;
extern uint16_t ctrl_light_handle;
extern uint16_t ctrl_power_handle;

extern uint16_t stat_rpm_handle;
extern uint16_t stat_angle_handle;
extern uint16_t stat_light_handle;
extern uint16_t stat_power_handle;

/* --- Reference to queue that wifi_manager creates --- */
extern QueueHandle_t wifi_cred_queue; /* defined in wifi_manager.c */

/* Helper: notify a status characteristic (safe to call from tasks) */
static void notify_status_rpm(void)
{
    if (stat_rpm_handle) ble_gatts_chr_updated(stat_rpm_handle);
}
static void notify_status_angle(void)
{
    if (stat_angle_handle) ble_gatts_chr_updated(stat_angle_handle);
}
static void notify_status_light(void)
{
    if (stat_light_handle) ble_gatts_chr_updated(stat_light_handle);
}
static void notify_status_power(void)
{
    if (stat_power_handle) ble_gatts_chr_updated(stat_power_handle);
}

/* --- biz API implementations --- */

void biz_apply_wifi_credentials(const wifi_credentials_t *cred)
{
    if (!cred) return;

    if (wifi_cred_queue == NULL) {
        ESP_LOGE(TAG, "wifi_cred_queue not available");
        return;
    }

    /* Use a local copy to avoid lifetime issues */
    wifi_credentials_t copy = {0};
    strncpy(copy.ssid, cred->ssid, sizeof(copy.ssid)-1);
    copy.ssid_len = cred->ssid_len;
    if (cred->pass_len) {
        strncpy(copy.pass, cred->pass, sizeof(copy.pass)-1);
        copy.pass_len = cred->pass_len;
    } else {
        copy.pass_len = 0;
        copy.pass[0] = '\0';
    }

    BaseType_t ok = xQueueSend(wifi_cred_queue, &copy, pdMS_TO_TICKS(50));
    if (ok == pdTRUE) {
        ESP_LOGI(TAG, "Queued Wi-Fi credentials SSID='%s' len=%u", copy.ssid, copy.ssid_len);
    } else {
        ESP_LOGW(TAG, "Failed to queue Wi-Fi credentials (queue full?)");
    }
}

/* When parser instructs to set control values, update control var,
   also update the reported status and notify BLE subscribers. */

/* Note: using int arguments helps parser pass atoi() results directly. */
void biz_set_rpm(int rpm)
{
    if (rpm < 0) return;
    g_ctrl_rpm = (uint32_t)rpm;
    /* Simulate actuator immediate completion by mirroring to status */
    g_stat_rpm = (uint32_t)rpm;
    notify_status_rpm();
    ESP_LOGI(TAG, "biz_set_rpm -> ctrl=%u stat=%u", (unsigned)g_ctrl_rpm, (unsigned)g_stat_rpm);
}

void biz_set_angle(int angle)
{
    if (angle < 0) return;
    g_ctrl_angle = (uint32_t)angle;
    g_stat_angle = (uint32_t)angle;
    notify_status_angle();
    ESP_LOGI(TAG, "biz_set_angle -> ctrl=%u stat=%u", (unsigned)g_ctrl_angle, (unsigned)g_stat_angle);
}

void biz_set_light(int light)
{
    if (light < 0) return;
    g_ctrl_light = (uint8_t)light;
    g_stat_light = (uint8_t)light;
    notify_status_light();
    ESP_LOGI(TAG, "biz_set_light -> ctrl=%u stat=%u", (unsigned)g_ctrl_light, (unsigned)g_stat_light);
}

void biz_set_power(int power)
{
    if (power < 0) return;
    g_ctrl_power = (uint8_t)power;
    g_stat_power = (uint8_t)power;
    notify_status_power();
    ESP_LOGI(TAG, "biz_set_power -> ctrl=%u stat=%u", (unsigned)g_ctrl_power, (unsigned)g_stat_power);
}

/* Optional simple accessors */
uint32_t biz_get_ctrl_rpm(void) { return g_ctrl_rpm; }
uint32_t biz_get_stat_rpm(void) { return g_stat_rpm; }
uint32_t biz_get_ctrl_angle(void) { return g_ctrl_angle; }
uint32_t biz_get_stat_angle(void) { return g_stat_angle; }
uint8_t biz_get_ctrl_light(void) { return g_ctrl_light; }
uint8_t biz_get_stat_light(void) { return g_stat_light; }
uint8_t biz_get_ctrl_power(void) { return g_ctrl_power; }
uint8_t biz_get_stat_power(void) { return g_stat_power; }

