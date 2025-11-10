#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include "wifi_cred.h"   // contains wifi_credentials_t

#ifdef __cplusplus
extern "C" {
#endif

/* Create and start the wifi manager task and queue. Call from app_main(). */
void wifi_manager_init(void);

/* The Wi-Fi manager creates this queue handle; gatt_svr.c references it as extern. */
extern QueueHandle_t wifi_cred_queue;

#ifdef __cplusplus
}
#endif
