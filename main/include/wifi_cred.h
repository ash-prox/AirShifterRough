#pragma once
#include <stdint.h>

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64

//extern QueueHandle_t wifi_cred_queue;

typedef struct {
    uint8_t ssid_len;
    uint8_t pass_len;
    char ssid[WIFI_SSID_MAX_LEN + 1]; // nul-terminated
    char pass[WIFI_PASS_MAX_LEN + 1]; // nul-terminated
} wifi_credentials_t;
