/* wifi_manager.c - mesh-capable wifi manager (fixed compile errors)
 *
 * - Uses esp-mesh-lite
 * - Accepts router creds via BLE queue (wifi_cred_queue) and also enables
 *   mesh-lite provisioning manager (BLE / softAP).
 *
 * Notes:
 *  - Ensure your project links mesh_lite component and wifi_prov_mgr (menuconfig).
 *  - If mesh_lite's mesh_lite_sta_config_t has different field names in your version,
 *    adapt accordingly.
 */

#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_bridge.h"


#include "esp_mesh_lite.h"
#if defined(CONFIG_MESH_LITE_PROV_TRANSPORT_BLE)
#include "wifi_prov_mgr.h"
#endif
#if defined(CONFIG_MESH_LITE_PROV_ENABLE)
#include "zero_provisioning.h"
#endif

/* Fallback server definitions if not provided by sdkconfig */
#ifndef CONFIG_SERVER_IP
#define CONFIG_SERVER_IP "192.168.4.100"
#endif
#ifndef CONFIG_SERVER_PORT
#define CONFIG_SERVER_PORT 9000
#endif

/* External queue supplied by your BLE parser */
//extern QueueHandle_t wifi_cred_queue;f
QueueHandle_t wifi_cred_queue;

/* Credentials structure used on the queue */
typedef struct {
    char ssid[33];
    char password[65];
} wifi_credentials_t;

// typedef struct {
//     char ssid[33];
//     char password[65];
// } wifi_credentials_t;

static const char *TAG = "wifi_manager";
static int g_sockfd = -1;




/* debug helper (include <ctype.h>) */
static void log_string_bytes(const char *tag, const char *s, size_t maxlen) {
    size_t len = strnlen(s, maxlen);
    ESP_LOGI(tag, "String (len=%d): '%.*s'", (int)len, (int)len, s);
    /* hexdump the whole maxlen area to show trailing/leading non-printables */
    ESP_LOG_BUFFER_HEXDUMP(tag, s, maxlen, ESP_LOG_INFO);
}




/* ---------- TCP client (adapted) ---------- */
static int socket_tcp_client_create(const char *ip, uint16_t port)
{
    ESP_LOGD(TAG, "Create tcp client ip=%s port=%u", ip, port);
    int sockfd = -1;
    struct ifreq iface;
    memset(&iface, 0, sizeof(iface));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip),
    };

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "socket create failed: %d", sockfd);
        return -1;
    }

    esp_netif_get_netif_impl_name(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), iface.ifr_name);
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(struct ifreq)) != 0) {
        ESP_LOGW(TAG, "Bind to device %s failed", iface.ifr_name);
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGD(TAG, "connect failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }
    return sockfd;
}

void tcp_client_write_task(void *arg)
{
    const char *server_ip = CONFIG_SERVER_IP;
    const uint16_t server_port = CONFIG_SERVER_PORT;
    uint8_t sta_mac[6] = {0};
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);

    ESP_LOGI(TAG, "tcp_client_write_task started");
    int count = 0;

    while (1) {
        if (g_sockfd == -1) {
            g_sockfd = socket_tcp_client_create(server_ip, server_port);
            if (g_sockfd == -1) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(3000));

        char *payload = NULL;
        int level = esp_mesh_lite_get_level();
        size_t len = asprintf(&payload, "{\"src_addr\":\"" MACSTR "\",\"data\":\"Hello\",\"level\":%d,\"count\":%d}\r\n",
                              MAC2STR(sta_mac), level, count++);
        if (!payload) continue;

        ssize_t written = write(g_sockfd, payload, len);
        free(payload);
        if (written <= 0) {
            ESP_LOGW(TAG, "tcp write failed (%zd), closing socket", written);
            close(g_sockfd);
            g_sockfd = -1;
            continue;
        }
    }
    if (g_sockfd != -1) { close(g_sockfd); g_sockfd = -1; }
    vTaskDelete(NULL);
}

/* ---------- IP event handler ---------- */
static void ip_event_sta_got_ip_handler(void *arg, esp_event_base_t event_base,
                                        int32_t event_id, void *event_data)
{
    static bool tcp_task_started = false;
    if (!tcp_task_started) {
        xTaskCreate(tcp_client_write_task, "tcp_client_write_task", 4 * 1024, NULL, 5, NULL);
        tcp_task_started = true;
    }
}

/* ---------- SoftAP info (same behavior) ---------- */
void app_wifi_set_softap_info(void)
{
    char softap_ssid[33] = {0};
    char softap_psw[65] = {0};
    uint8_t softap_mac[6] = {0};
    size_t ssid_size = sizeof(softap_ssid);
    size_t psw_size  = sizeof(softap_psw);

    esp_wifi_get_mac(WIFI_IF_AP, softap_mac);

    if (esp_mesh_lite_get_softap_ssid_from_nvs(softap_ssid, &ssid_size) == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP SSID from NVS: %s", softap_ssid);
    } else {
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
        snprintf(softap_ssid, sizeof(softap_ssid), "%.25s_%02x%02x%02x", CONFIG_BRIDGE_SOFTAP_SSID, softap_mac[3], softap_mac[4], softap_mac[5]);
#else
        snprintf(softap_ssid, sizeof(softap_ssid), "%.32s", CONFIG_BRIDGE_SOFTAP_SSID);
#endif
        ESP_LOGI(TAG, "SoftAP SSID default: %s", softap_ssid);
    }

    if (esp_mesh_lite_get_softap_psw_from_nvs(softap_psw, &psw_size) == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP password retrieved from NVS.");
    } else {
        strlcpy(softap_psw, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(softap_psw));
        ESP_LOGI(TAG, "SoftAP password default used.");
    }

    esp_mesh_lite_set_softap_info(softap_ssid, softap_psw);
}

/* ---------- Apply router credentials via mesh-lite ---------- */
static void mesh_wifi_credentials_apply(const wifi_credentials_t *creds)
{
    if (!creds) return;

    /* mesh_lite expects mesh_lite_sta_config_t (not wifi_sta_config_t) */
    mesh_lite_sta_config_t ml_conf;
    memset(&ml_conf, 0, sizeof(ml_conf));

    /* Most mesh-lite versions use ssid/password char arrays similar to wifi_sta_config_t.
     * If your installed version differs, adapt these field names accordingly. */
    strlcpy((char *)ml_conf.ssid, creds->ssid, sizeof(ml_conf.ssid));
    strlcpy((char *)ml_conf.password, creds->password, sizeof(ml_conf.password));

    ESP_LOGI(TAG, "Applying router creds SSID='%s' (password hidden)", creds->ssid);

    esp_err_t err = esp_mesh_lite_set_router_config(&ml_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mesh_lite_set_router_config failed: %s", esp_err_to_name(err));
        return;
    }

    /* After setting router config, ask mesh-lite to start/refresh connection */
    esp_mesh_lite_connect();
}

/* ---------- Credentials queue task ---------- */
static void creds_queue_task(void *arg)
{
    wifi_credentials_t creds;
    ESP_LOGI(TAG, "Credentials queue task running - waiting for BLE creds");
    while (1) {

        if (xQueueReceive(wifi_cred_queue, &creds, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Received WiFi creds via queue");
            log_string_bytes(TAG, creds.ssid, sizeof(creds.ssid));
            log_string_bytes(TAG, creds.password, sizeof(creds.password));
            /* Option A: hand creds to mesh-lite directly (works) */
            mesh_wifi_credentials_apply(&creds);

            /* Option B (alternative): if you'd prefer to re-use wifi_prov_mgr API to
             * simulate provisioning flow, you can call wifi_prov_mgr_provision() here
             * (if your build exposes that API). Many apps simply call mesh-lite set_router_config */
        }
    }
    vTaskDelete(NULL);
}

/* ---------- System info timer ---------- */
static void print_system_info_timercb(TimerHandle_t timer)
{
    uint8_t primary = 0;
    uint8_t sta_mac[6] = {0};
    wifi_ap_record_t ap_info = {0};
    wifi_second_chan_t second = 0;
    wifi_sta_list_t wifi_sta_list = {0};

    esp_wifi_sta_get_ap_info(&ap_info);
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    esp_wifi_ap_get_sta_list(&wifi_sta_list);
    esp_wifi_get_channel(&primary, &second);

    ESP_LOGI(TAG, "System info channel:%u layer:%d self:" MACSTR " parent:" MACSTR " rssi:%d free_heap:%u",
             primary, esp_mesh_lite_get_level(), MAC2STR(sta_mac), MAC2STR(ap_info.bssid),
             (ap_info.rssi != 0 ? ap_info.rssi : -120), esp_get_free_heap_size());

#if CONFIG_MESH_LITE_NODE_INFO_REPORT
    ESP_LOGI(TAG, "All node number: %u", esp_mesh_lite_get_mesh_node_number());
#endif

    for (int i = 0; i < wifi_sta_list.num; i++) {
        ESP_LOGI(TAG, "Child mac: " MACSTR, MAC2STR(wifi_sta_list.sta[i].mac));
    }
}

/* ---------- Storage init ---------- */
static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/* ---------- wifi_manager_start (main entry) ---------- */
void wifi_manager_start(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_ERROR_CHECK(esp_storage_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_bridge_create_all_netif();

    /* SoftAP base config for bridge (kept as before) */
    {
        wifi_config_t apcfg = {
            .ap = {
                .ssid = CONFIG_BRIDGE_SOFTAP_SSID,
                .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
            }
        };
        esp_bridge_wifi_set_config(WIFI_IF_AP, &apcfg);
    }

    /* mesh-lite init and start */
    esp_mesh_lite_config_t mesh_lite_config = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&mesh_lite_config);

    app_wifi_set_softap_info();
    esp_mesh_lite_start();

#if defined(CONFIG_MESH_LITE_PROV_ENABLE)
    zero_prov_init(NULL, NULL);
#endif

#if defined(CONFIG_MESH_LITE_PROV_TRANSPORT_BLE)
    esp_mesh_lite_wifi_prov_mgr_init();
#endif

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &ip_event_sta_got_ip_handler, NULL, NULL));

    TimerHandle_t timer = xTimerCreate("print_system_info", 10000 / portTICK_PERIOD_MS, pdTRUE, NULL, print_system_info_timercb);
    xTimerStart(timer, 0);

    if (wifi_cred_queue != NULL) {
        xTaskCreate(creds_queue_task, "creds_queue_task", 4 * 1024, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "wifi_cred_queue NULL - BLE queue provisioning disabled");
    }
}



void wifi_manager_init(void)
{
    // create the credentials queue: holds wifi_credentials_t objects
    wifi_cred_queue = xQueueCreate(2, sizeof(wifi_credentials_t));
    // create the manager task
    //xTaskCreatePinnedToCore(wifi_manager_task, "wifi_manager", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    wifi_manager_start();
}
