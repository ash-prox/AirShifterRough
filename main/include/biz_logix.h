// biz_logix.h
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "wifi_cred.h" /* defines wifi_credentials_t */

/* --- Characteristic value handles (defined/initialized in gatt_svr.c) --- */
extern uint16_t ctrl_rpm_handle;
extern uint16_t ctrl_angle_handle;
extern uint16_t ctrl_light_handle;
extern uint16_t ctrl_power_handle;

extern uint16_t stat_rpm_handle;
extern uint16_t stat_angle_handle;
extern uint16_t stat_light_handle;
extern uint16_t stat_power_handle;

/* --- Business API used by parser.c --- */
/* Apply Wi-Fi credentials (will send to wifi_manager via queue) */
void biz_apply_wifi_credentials(const wifi_credentials_t *cred);

/* Apply control values (called by parser). These update control vars and
   update status and notify BLE subscribers. */
void biz_set_rpm(int rpm);
void biz_set_angle(int angle);
void biz_set_light(int light);
void biz_set_power(int power);

/* Authentication API (per-connection) */
bool biz_verify_auth_key(uint16_t conn_handle, const char *key);
bool biz_is_authenticated(uint16_t conn_handle);
void biz_clear_auth(uint16_t conn_handle);

/* Optional accessors (if other modules need to read) */
uint32_t biz_get_ctrl_rpm(void);
uint32_t biz_get_stat_rpm(void);
uint32_t biz_get_ctrl_angle(void);
uint8_t  biz_get_ctrl_light(void);
uint8_t  biz_get_ctrl_power(void);
uint32_t biz_get_stat_angle(void);
uint8_t  biz_get_stat_light(void);
uint8_t  biz_get_stat_power(void);
