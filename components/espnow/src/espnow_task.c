#include "polyplug_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "portmacro.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "espnow_funcs.h"
#include "gpio_funcs.h"
#include "espnow_task.h"
#include "wifi_funcs.h"
#include "wifi_task.h"

#define TAG "ESPNOW_TASK"

static volatile bool listen_triggered = false;
static volatile bool toggle_rx = true;

uint8_t received_enc_key[ENC_KEY_LEN] = {0};


QueueHandle_t xEspReceivedEncKeyQueue;

//static const uint8_t UNIVERSAL_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int data_len) {
	#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Received data of len: %d", data_len);
	#endif
	
	// If data is lora enc key
	if (data_len == ENC_KEY_LEN) {	
	    // Copy received bytes into your key array
	    memcpy(received_enc_key, data, ENC_KEY_LEN);
	
	    // Now print the stored key in hex
	    #ifdef POLYPLUG_DEBUG
		    ESP_LOG_BUFFER_HEX("RECEIVED ENC KEY", received_enc_key, ENC_KEY_LEN);
	    #endif
	    
	    // Save received key to flash
	    espnow_funcs_lora_key_nvs_save(received_enc_key, LORA_ENC_NS, LORA_ENC_FMT);
	    
	    listen_triggered = true;
	    toggle_rx = !toggle_rx;
	    
	    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	    xQueueSendFromISR(xEspReceivedEncKeyQueue, received_enc_key, &xHigherPriorityTaskWoken);
	    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
	// Else receiving a MAC address and network info for MQTT link
	else if (data_len <= MQTT_MAX_LEN) {
	    #ifdef POLYPLUG_DEBUG
		    ESP_LOGI(TAG, "Received MQTT: %s", (char *)data);
	    #endif
	    
	    wifi_mqtt_t network;
	    
	    int n = sscanf((char *)data, "%32[^:]:%64[^:]:%12s", network.ssid, network.password, network.mac);
		if (n != 3) {
			#ifdef POLYPLUG_DEBUG
			    ESP_LOGW(TAG, "Failed to parse MQTT payload (%d fields)", n);
		    #endif
		}
		else {
			#ifdef POLYPLUG_DEBUG
			    ESP_LOGI(TAG, "Parsed SSID=%s, PASS=%s, MAC=%s", network.ssid, network.password, network.mac);
		    #endif
		}
		
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	    xQueueSendFromISR(xWifiConnectQueue, &network, &xHigherPriorityTaskWoken);
	    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	    
	    listen_triggered = true;
	    toggle_rx = !toggle_rx;
	}
}

static void espnow_task(void *param)
{
	xEspReceivedEncKeyQueue = xQueueCreate(1, sizeof(received_enc_key));
	if (xEspReceivedEncKeyQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xEspReceivedEncKeyQueue semaphore");
	}
	configASSERT(xEspReceivedEncKeyQueue);
	
	esp_err_t err = espnow_funcs_lora_key_nvs_load(received_enc_key, LORA_ENC_NS, LORA_ENC_FMT);
	if (err == ESP_OK) {
	    // If key existed, send
	    xQueueSend(xEspReceivedEncKeyQueue, received_enc_key, portMAX_DELAY);
	}
	    
	while (1) {
		
		// If ESPNOW pair button pressed
		if (gpio_get_level(PAIR_BTN1_PIN) == 0) {
			if (toggle_rx) {
				// Disconnect from wifi if connected
				ESP_ERROR_CHECK(wifi_funcs_radio_stop());
				
				// Start radio and initialize ESP-NOW
				ESP_ERROR_CHECK(espnow_funcs_wifi_radio_start(WIFI_CHANNEL));
				ESP_ERROR_CHECK(esp_now_init());
				
				// Register receive callback
	    		ESP_ERROR_CHECK(espnow_funcs_espnow_register_recv_cb(on_data_recv));
	    		
	    		gpio_rgb_ready_to_rx(true); // Tell RGB we're ready to receive
			}
			else {
				// Stop radio and de-initialize ESP-NOW
			    ESP_ERROR_CHECK(espnow_funcs_espnow_deinit());
			    ESP_ERROR_CHECK(espnow_funcs_wifi_radio_stop());
			    
			    gpio_rgb_ready_to_rx(false); // Back out RGB
			    
			    // Try to reconnect to Wi-Fi
			    xSemaphoreGive(xWifiReconnectSemaphore);
			}
			
			toggle_rx = !toggle_rx;
			
			vTaskDelay(pdMS_TO_TICKS(250)); // Ignore bounce
		}
		
		
		
		if (listen_triggered) {
			// Stop radio and de-initialize ESP-NOW
		    ESP_ERROR_CHECK(espnow_funcs_espnow_deinit());
		    ESP_ERROR_CHECK(espnow_funcs_wifi_radio_stop());
		    
		    gpio_rgb_ready_to_rx(false); // Tell RGB we received
		    
		    listen_triggered = false;
		}
    
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void espnow_task_create(void)
{
    if (xTaskCreate(espnow_task, "espnow_task", 1024 * 3, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start espnow_task");
	}
}