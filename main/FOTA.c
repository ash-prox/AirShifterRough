/*
############################################################################################################################################
############################################################################################################################################


headers


############################################################################################################################################
############################################################################################################################################
*/
// standard library imports
#include "string.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "protocol_examples_common.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

// third party imports

// local application imports
#include "nvs_blob_rw.h"

/*
############################################################################################################################################
############################################################################################################################################


global variable
* capital_letter : global_varibale
* small_letter : local_variable


############################################################################################################################################
############################################################################################################################################
*/
#define TAG "ota"
#define OTA_URL "http://ota.iotronix.co.in?ota_token="
#define OTA_TOKEN_LENGTH 33

/*
############################################################################################################################################
############################################################################################################################################


_http_event_handler


############################################################################################################################################
############################################################################################################################################
*/
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
	switch (evt->event_id)
	{
	case HTTP_EVENT_ERROR:
		ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
		break;

	case HTTP_EVENT_ON_CONNECTED:
		ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
		break;

	case HTTP_EVENT_HEADER_SENT:
		ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
		break;

	case HTTP_EVENT_ON_HEADER:
		ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
		break;

	case HTTP_EVENT_ON_DATA:
		ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
		break;

	case HTTP_EVENT_ON_FINISH:
		ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
		break;

	case HTTP_EVENT_DISCONNECTED:
		ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
		break;
	}
	return ESP_OK;
}

/*
############################################################################################################################################
############################################################################################################################################


ota


############################################################################################################################################
############################################################################################################################################
*/
void ota_task(uint8_t *nvs_blob_rw_payload)
{
	// Now add ota_token to URL
	char url_temp[256] = OTA_URL;
	char ota_token_temp[33];

	// Copy token
	for (uint8_t i = 0; i < 32; i++)
	{
		ota_token_temp[i] = *(nvs_blob_rw_payload + i + 1);
		ota_token_temp[i + 1] = 0;
	}

	// Now add token to URL
	strcat(url_temp, ota_token_temp);

	// Log URL
	ESP_LOGI(TAG, "URL : %s", url_temp);

	esp_http_client_config_t config =
		{
			.url = url_temp,
			.event_handler = _http_event_handler,
		};

	esp_err_t ret = esp_https_ota(&config);

	// Log status
	ESP_LOGI(TAG, "%s", (ret == ESP_OK ? "Firmware upgraded!" : "Firmware upgrade failed!"));

	// Update ota flag to NVS

	// Change ota flag value. 2 = success, 3 = failed.
	(ret == ESP_OK ? (nvs_blob_rw_payload[0] = 0x32) : (nvs_blob_rw_payload[0] = 0x33));

	esp_err_t err = nvs_blob_write_api("ota", nvs_blob_rw_payload, OTA_TOKEN_LENGTH);

	// Check for error in ota flag write
	if (err != ESP_OK)
		ESP_LOGE(TAG, "Error (%s) in writing ota flag write.", esp_err_to_name(err));

	// Wait for 10 sec and restart
	for (uint8_t i = 10; i > 0; i--)
	{
		printf("Restarting in %d seconds...\n", i);
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}

	// Restart now
	esp_restart();
}

/*
############################################################################################################################################
############################################################################################################################################


ota_init


############################################################################################################################################
############################################################################################################################################
*/
void ota_init(uint8_t *nvs_blob_rw_payload)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(example_connect());

#if CONFIG_EXAMPLE_CONNECT_WIFI
	/* Ensure to disable any WiFi power save mode, this allows best throughput
	 * and hence timings for overall OTA operation.
	 */
	esp_wifi_set_ps(WIFI_PS_NONE);
#endif // CONFIG_EXAMPLE_CONNECT_WIFI

	// Start OTA task handler
	ota_task(nvs_blob_rw_payload);
}

/*
############################################################################################################################################
############################################################################################################################################


check_ota_update


############################################################################################################################################
############################################################################################################################################
*/
void check_ota_update(void)
{
	/* Read OTA flag and ota_token from NVS.	*/
	uint8_t ota_token[OTA_TOKEN_LENGTH] = {0};

	// Read
	esp_err_t err = nvs_blob_read_api("ota", ota_token, OTA_TOKEN_LENGTH);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "Error (%s) in reading OTA data. Resuming normal operation.", esp_err_to_name(err));
		return;
	}

	// Process OTA update if ota update flag=1.
	if (ota_token[0] == 0x31)
	{
		// print read data
		ESP_LOGI(TAG, "OTA update flag found, starting OTA process...");
		ESP_LOG_BUFFER_HEX(TAG, ota_token, OTA_TOKEN_LENGTH);

		// Start OTA process
		ota_init(ota_token);

		// Infinite loop to hold program here only
		while (1)
		{
			vTaskDelay(10 * 1000 / portTICK_RATE_MS);
		}
	}
}


