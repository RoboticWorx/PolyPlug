#include "polyplug_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "espnow_task.h"
#include "espnow_funcs.h"
#include "gpio_task.h"

#define TAG "WIFI_TASK"

SemaphoreHandle_t xWifiReconnectSemaphore;

QueueHandle_t xWifiConnectQueue;

static wifi_mqtt_t info;

static void wifi_task(void *param)
{
	xWifiReconnectSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiReconnectSemaphore);
	
	xWifiConnectQueue = xQueueCreate(1, sizeof(wifi_mqtt_t));
	configASSERT(xWifiConnectQueue);
	
	wifi_funcs_wifi_event_init();
	wifi_funcs_mqtt_client_init();
	
	// Try to connect to previous
	wifi_mqtt_t prev_network = wifi_funcs_get_prev();
	if (strlen(prev_network.ssid) > 0) { // If previous exists
		#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Trying to connect to previous network...");
		#endif
		
		ESP_ERROR_CHECK(wifi_funcs_radio_start(prev_network.ssid, 0, prev_network.password));
				
		ESP_ERROR_CHECK(wifi_funcs_connect());
	}
	else {
		#ifdef POLYPLUG_DEBUG
			ESP_LOGW(TAG, "No previous network to connect to");
		#endif
	}
    
	while (1) {
		
		if (xQueueReceive(xWifiConnectQueue, &info, 0) == pdTRUE) {
			ESP_ERROR_CHECK(wifi_funcs_radio_start(info.ssid, 0, info.password));
				
			ESP_ERROR_CHECK(wifi_funcs_connect());
		}
		
		// Reconnect to previous known network
		if (xSemaphoreTake(xWifiReconnectSemaphore, 0) == pdTRUE) {
			#ifdef POLYPLUG_DEBUG
				ESP_LOGI(TAG, "Reconnect requested");
			#endif
			
			ESP_ERROR_CHECK(wifi_funcs_radio_start(prev_network.ssid, 0, prev_network.password));
					
			ESP_ERROR_CHECK(wifi_funcs_connect());
		}
    	
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void wifi_task_create(void)
{
    if (xTaskCreate(wifi_task, "wifi_task", 1024 * 3, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start wifi_task");
	}
}