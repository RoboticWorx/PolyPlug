#include "ota_update.h"
#include "polyplug_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "espnow_task.h"
#include "espnow_funcs.h"
#include "gpio_task.h"

#define TAG "WIFI_TASK"

SemaphoreHandle_t xWifiReconnectSemaphore;

QueueHandle_t xWifiConnectQueue;

static wifi_mqtt_t info;

bool connected_to_network = false;
bool using_espnow = false;

static void wifi_task(void *param)
{
	xWifiReconnectSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiReconnectSemaphore);
	
	xWifiConnectQueue = xQueueCreate(1, sizeof(wifi_mqtt_t));
	configASSERT(xWifiConnectQueue);
	
	wifi_funcs_mac_nvs_load(); // Load last sender MAC
	
	wifi_funcs_wifi_event_init();
	wifi_funcs_mqtt_client_init();
	
	// Let everything else initialize
	vTaskDelay(pdMS_TO_TICKS(2000));

	// Get here without crashing -> This is a valid OTA app
	ota_update_mark_app_valid();

	/* Update NVS FW version */
	// If NVS version doesn't exist yet, set it
	char dummy[64];
	if (ota_update_get_nvs_version(dummy, sizeof(dummy)) != ESP_OK) {
		// Read the current app's version string from the embedded app descriptor
		const esp_app_desc_t *running = esp_app_get_description();
		const char *cur = running ? running->version : "";
		
		// Save that version to NVS
		ota_update_set_nvs_version(cur);
		
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Setting first time FW version: %s", cur);
		#endif
	}
	else {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Using pre-set PolyPlug FW version '%s'", dummy);
		#endif
	}
	
	// Try to connect to last known network
	wifi_mqtt_t prev_network = wifi_funcs_get_prev();
	if (strlen(prev_network.ssid) > 0) { // If previous exists
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Trying to connect to previous network...");
		#endif
		
		// Connect
		ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_funcs_set_config(prev_network.ssid, 0, prev_network.password));
				
		ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_funcs_connect());
	}
	else {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "No previous network to connect to");
		#endif
	}
	
	while (1) {
		
		if (xQueueReceive(xWifiConnectQueue, &info, 0) == pdTRUE) {
			// Switching from ESP-NOW: both false
			connected_to_network = false;
			using_espnow = false;
			
			// Update config
			ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_funcs_set_config(info.ssid, 0, info.password));
			
			// Save sender MAC to NVS
			wifi_funcs_mac_nvs_save(info.key);
		}
		// Reconnect to previous known network
		else if (xSemaphoreTake(xWifiReconnectSemaphore, 0) == pdTRUE) {
			// Switching from ESP-NOW: both false
			connected_to_network = false;
			using_espnow = false;
	
			#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Reconnect requested");
			#endif
			
			// Update prev
			wifi_mqtt_t prev_network = wifi_funcs_get_prev();
						
			// Update config
			ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_funcs_set_config(prev_network.ssid, 0, prev_network.password));
		}
		
		// Poll reconnect (don't abort on transient errors)
		esp_err_t err = wifi_funcs_connect();
		if (err != ESP_OK) {
#ifdef POLYPLUG_DEBUG
			ESP_LOGW(TAG, "wifi_funcs_connect: %s", esp_err_to_name(err));
#endif
		}
		
		vTaskDelay(pdMS_TO_TICKS(250));
	}
}

void wifi_task_create(void)
{
	if (xTaskCreate(wifi_task, "wifi_task", 1024 * 5, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start wifi_task");
	}
}