#include "wifi_cred.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>
/* start mqtt after wifi connects */
#include "mqtt.h"
#include "esp_bridge.h"
#include "esp_mesh_lite_core.h"
#include "esp_mesh_lite.h"


int do_ping_cmd(void);
static const char *TAG = "wifi_mgr";
static const char *TAG2 = "mesh_lite";

/* Event group bits used by wifi manager */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* retry policy */
#define WIFI_MAX_RETRY 4



/* Exported queue handle so BLE code can post credentials */
QueueHandle_t wifi_cred_queue;
EventGroupHandle_t wifi_event_group;

/* retry counter for connection attempts */
static int s_retry_num = 0;

/* guard to ensure MQTT is started only once after IP */
static bool mqtt_started = false;

/* NVS namespace & keys */
static const char *NVS_NAMESPACE = "wifi";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "pass";

/* forward */
// static void wifi_event_handler(void* arg, esp_event_base_t event_base,
//                                 int32_t event_id, void* event_data);

/* Helper: store credentials into NVS (blocking) */
// static esp_err_t store_credentials_nvs(const char *ssid, const char *pass)
// {
//     nvs_handle_t h;
//     esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
//         return err;
//     }
//     err = nvs_set_str(h, NVS_KEY_SSID, ssid);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "nvs_set_str(ssid) failed: %s", esp_err_to_name(err));
//         nvs_close(h);
//         return err;
//     }
//     err = nvs_set_str(h, NVS_KEY_PASS, pass);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "nvs_set_str(pass) failed: %s", esp_err_to_name(err));
//         nvs_close(h);
//         return err;
//     }
//     err = nvs_commit(h);
//     nvs_close(h);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
//         return err;
//     }
//     ESP_LOGI(TAG, "Credentials saved to NVS");
//     return ESP_OK;
// }

// /* Helper: read credentials from NVS; returns ESP_OK if both present */
// static esp_err_t load_credentials_nvs(wifi_credentials_t *out)
// {
//     nvs_handle_t h;
//     esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
//     if (err != ESP_OK) return err;

//     size_t ssid_len = sizeof(out->ssid);
//     size_t pass_len = sizeof(out->pass);
//     err = nvs_get_str(h, NVS_KEY_SSID, out->ssid, &ssid_len);
//     if (err != ESP_OK) {
//         nvs_close(h);
//         return err;
//     }
//     err = nvs_get_str(h, NVS_KEY_PASS, out->pass, &pass_len);
//     nvs_close(h);
//     if (err != ESP_OK) return err;
//     out->ssid_len = (uint8_t)strlen(out->ssid);
//     out->pass_len = (uint8_t)strlen(out->pass);
//     return ESP_OK;
// }

// /* Wi-Fi config + start function (task context) */
// static esp_err_t wifi_start_with_creds(const wifi_credentials_t *cred)
// {
//     wifi_config_t wifi_config = { 0 };
//     // copy ssid, pass into config (ensure null-termination)
//     strncpy((char*)wifi_config.sta.ssid, cred->ssid, sizeof(wifi_config.sta.ssid) - 1);
//     strncpy((char*)wifi_config.sta.password, cred->pass, sizeof(wifi_config.sta.password) - 1);

//     ESP_LOGI(TAG2, "Setting Wi-Fi mesh lite config SSID='%s' (len=%u)", cred->ssid, cred->ssid_len);


//     //esp_mesh_lite_set_router(cred->ssid, cred->pass);
//     //esp_mesh_lite_connect();
//     ESP_LOGI(TAG2, "Wi-Fi connection initiated");

//     // COMMENTED OUT: let mesh-lite handle wifi start/connect
//     // esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
//     // if (err != ESP_OK) {
//     //     ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
//     //     return err;
//     // }
//     // err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
//     // if (err != ESP_OK) {
//     //     ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
//     //     return err;
//     // }
//     // err = esp_wifi_start();
//     // if (err != ESP_OK) {
//     //     ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
//     //     return err;
//     // }
//     // err = esp_wifi_connect();
//     // if (err != ESP_OK) {
//     //     ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
//     //     return err;
//     // }


//     ESP_LOGI(TAG, "Wi-Fi connection initiated");
//     ESP_LOGI(TAG, "Wi-Fi start requested");
//     return ESP_OK;
// }

/* This task owns Wi-Fi init/connect and NVS writes */
static void wifi_manager_task(void *arg)
{
    wifi_credentials_t cred;
    esp_err_t err;

    /* Ensure NVS and esp-event/wifi are initialised */
    // nvs_flash_init should have been called in app_main already
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    //esp_netif_create_default_wifi_sta();


    esp_bridge_create_all_netif();

    esp_mesh_lite_config_t mesh_lite_config = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&mesh_lite_config);

    esp_mesh_lite_start();

    ESP_LOGI(TAG2, "Wi-Fi mesh lite task started");

    esp_mesh_lite_connect();
    ESP_LOGI(TAG2, "Wi-Fi mesh lite connect called");

    //wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    //ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handler to get IP / connect/disconnect events */
    // ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    // ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Create event group (shared) */
    wifi_event_group = xEventGroupCreate();

    /* Check if credentials already in NVS. If found, auto-connect */
    // if (load_credentials_nvs(&cred) == ESP_OK) {
    //     ESP_LOGI(TAG, "Found credentials in NVS, starting Wi-Fi");
    //         //wifi_start_with_creds(&cred);
    // } else {
    //     ESP_LOGI(TAG, "No credentials in NVS - waiting for BLE provisioning");
    //     // keep waiting for BLE-provisioned creds
    // }

    // for (;;) {
    //     /* Wait for a credential message from BLE (blocking) */
    //     if (xQueueReceive(wifi_cred_queue, &cred, portMAX_DELAY) == pdTRUE) {
    //         ESP_LOGI(TAG, "Got credentials from BLE: ssid_len=%u pass_len=%u", cred.ssid_len, cred.pass_len);

    //         // Validate lengths
    //         if (cred.ssid_len == 0 || cred.ssid_len > WIFI_SSID_MAX_LEN ||
    //             cred.pass_len > WIFI_PASS_MAX_LEN) {
    //             ESP_LOGW(TAG, "Invalid credential lengths, ignoring.");
    //             continue;
    //         }

    //         // Store to NVS
    //         err = store_credentials_nvs(cred.ssid, cred.pass);
    //         if (err != ESP_OK) {
    //             ESP_LOGE(TAG, "Failed to store credentials");
    //             continue;
    //         }

    //         // Start Wi-Fi with new config
    //         err = wifi_start_with_creds(&cred);
    //         if (err != ESP_OK) {
    //             ESP_LOGE(TAG2, "Failed to start Wi-Fi Mesh lite connection: %s", esp_err_to_name(err));
    //             continue;
    //         }

    //         // Optionally block here until connection or failure if you want to wait.
    //         // But typically you let event handler set wifi_event_group bits.

    //     }
    // }
}

/* event handler receives connect/disconnect/got_ip */
// static void wifi_event_handler(void* arg, esp_event_base_t event_base,
//                                int32_t event_id, void* event_data)
// {
//     if (event_base == WIFI_EVENT) {
//         if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
//             ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
//             if (s_retry_num < WIFI_MAX_RETRY) {
//                 ESP_LOGI(TAG, "Retrying to connect to AP (attempt %d of %d)", s_retry_num + 1, WIFI_MAX_RETRY);
//                 esp_err_t rc = esp_wifi_connect();
//                 if (rc != ESP_OK) {
//                     ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(rc));
//                 }
//                 s_retry_num++;
//             } else {
//                 xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
//                 ESP_LOGI(TAG, "Exceeded max retries (%d). Giving up.", WIFI_MAX_RETRY);
//                 /* reset retry counter so future provisioning can try again */
//                 s_retry_num = 0;
//             }
//         }
//     } else if (event_base == IP_EVENT) {
//         if (event_id == IP_EVENT_STA_GOT_IP) {
//             /* reset retry counter on success */
//             s_retry_num = 0;
//             xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
//             ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
//             ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
//             /* start MQTT exactly once now that we have an IP */
//             if (!mqtt_started) {
//                 mqtt_started = true;
//                 mqtt5_app_start();
//             }
//             //do_ping_cmd();
//         }
//     }
// }

/* Called by app_main once at startup */
void wifi_manager_init(void)
{
    // create the credentials queue: holds wifi_credentials_t objects
    //wifi_cred_queue = xQueueCreate(2, sizeof(wifi_credentials_t));
    // create the manager task
    //xTaskCreatePinnedToCore(wifi_manager_task, "wifi_manager", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    wifi_manager_task(NULL);
}
