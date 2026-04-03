#include "polyplug_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "portmacro.h"

#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include "espnow_funcs.h"
#include "gpio_funcs.h"
#include "espnow_task.h"
#include "wifi_funcs.h"
#include "wifi_task.h"
#include "lora_pcp.h"

#define TAG "ESPNOW_TASK"

static volatile bool listen_triggered = false;
static volatile bool espnow_inited = false;

uint8_t received_enc_key[LORA_PCP_ENC_KEY_LEN] = {0};

QueueHandle_t xEspReceivedEncKeyQueue;

//static const uint8_t UNIVERSAL_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
	if (!data || data_len <= 0) {
		return;
	}

	#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "Received data of len: %d", data_len);
	#endif

	// If data is lora enc key
	if (data_len == LORA_PCP_ENC_KEY_LEN) {
		// Copy received bytes into your key array
		memcpy(received_enc_key, data, LORA_PCP_ENC_KEY_LEN);
	
		// Now print the stored key in hex
		#ifdef POLYPLUG_DEBUG
			ESP_LOG_BUFFER_HEX("RECEIVED ENC KEY", received_enc_key, LORA_PCP_ENC_KEY_LEN);
		#endif
		
		// Save received key to flash
		espnow_funcs_lora_key_nvs_save(received_enc_key);
		
		listen_triggered = true;

#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Sending new encryption key to LoRa task");
#endif
		// Send received enc key
		xQueueOverwrite(xEspReceivedEncKeyQueue, received_enc_key);
		
		// Try to reconnect to Wi-Fi with previously saved network
		xSemaphoreGive(xWifiReconnectSemaphore);
	}
	// Else receiving a MAC address and network info for MQTT link
	else if (data_len <= MQTT_MAX_LEN) {
		// ESP-NOW data is not guaranteed NUL-terminated; copy and terminate
		char data_str[MQTT_MAX_LEN + 1];
		int copy_len = data_len < MQTT_MAX_LEN ? data_len : MQTT_MAX_LEN;
		memcpy(data_str, data, copy_len);
		data_str[copy_len] = '\0';

		#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Received MQTT: %s", data_str);
		#endif

		wifi_mqtt_t network;

		int n = sscanf(data_str, "%32[^:]:%64[^:]:%32s", network.ssid, network.password, network.key);
		if (n != 3) {
			#ifdef POLYPLUG_DEBUG
			ESP_LOGW(TAG, "Failed to parse MQTT payload (%d fields)", n);
			#endif
		}
		else {
			#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Parsed SSID=%s, PASS=%s, KEY=%s", network.ssid, network.password, network.key);
			ESP_LOGI(TAG, "Sending Wi-Fi info to wifi_task");
			#endif

			xQueueOverwrite(xWifiConnectQueue, &network);

			listen_triggered = true;
		}
	}
}

static void espnow_task(void *param)
{
	xEspReceivedEncKeyQueue = xQueueCreate(1, sizeof(received_enc_key));
	if (xEspReceivedEncKeyQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xEspReceivedEncKeyQueue semaphore");
	}
	configASSERT(xEspReceivedEncKeyQueue);
	
	espnow_funcs_lora_key_nvs_load(received_enc_key);

#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "Sending initial loaded encryption key to LoRa task");
#endif
	xQueueSend(xEspReceivedEncKeyQueue, received_enc_key, portMAX_DELAY); // MUST ALWAYS SEND INITIAL
   
	while (1) {
		// If ESPNOW pair button pressed
		if (gpio_get_level(PAIR_BTN1_PIN) == 0) {
			if (!espnow_inited) {
				wifi_funcs_wifi_disconnect(); // Disconnect Wi-Fi if connected

				// Initialize ESP-NOW on listening channel
				ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, 0) );
				ESP_ERROR_CHECK(esp_now_init());

				// Register receive callback
				ESP_ERROR_CHECK(espnow_funcs_espnow_register_recv_cb(on_data_recv));

				espnow_inited = true;
				gpio_rgb_ready_to_rx(true); // Tell RGB we're ready to receive
			}
			else {
				// De-initialize ESP-NOW
				ESP_ERROR_CHECK(esp_now_deinit());
				espnow_inited = false;
				listen_triggered = false;

				gpio_rgb_ready_to_rx(false); // Back out RGB

				// Try to reconnect to Wi-Fi with previously saved network
				xSemaphoreGive(xWifiReconnectSemaphore);
			}

			vTaskDelay(pdMS_TO_TICKS(250)); // Ignore bounce
		}

		if (listen_triggered) {
			listen_triggered = false;

			if (espnow_inited) {
				// De-initialize ESP-NOW
				ESP_ERROR_CHECK(esp_now_deinit());
				espnow_inited = false;
			}

			gpio_rgb_ready_to_rx(false); // Tell RGB we received
		}

		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void espnow_task_create(void)
{
	if (xTaskCreate(espnow_task, "espnow_task", 1024 * 3, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start espnow_task");
	}
}