#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "espnow_funcs.h"
#include "gpio_funcs.h"
#include "portmacro.h"
#include "espnow_task.h"

#define TAG "ESPNOW_TASK"

static volatile bool listen_triggered = false;

uint8_t received_enc_key[ENC_KEY_LEN] = {0};

QueueHandle_t xEspReceivedEncKeyQueue;

//static const uint8_t UNIVERSAL_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int data_len) {
    // Only take up to ENC_KEY_LEN bytes
    int copy_len = data_len < ENC_KEY_LEN ? data_len : ENC_KEY_LEN;

    // Copy received bytes into your key array
    memcpy(received_enc_key, data, copy_len);

    // Now print the stored key in hex
    ESP_LOGI("ESPNOW_RX", "Stored key (%d bytes):", copy_len);
    ESP_LOG_BUFFER_HEX("ESPNOW_RX", received_enc_key, ENC_KEY_LEN);
    
    // Save received key to flash
    esp_lora_key_nvs_save(received_enc_key, LORA_ENC_NS, LORA_ENC_FMT);
    
    listen_triggered = true;
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(xEspReceivedEncKeyQueue, received_enc_key, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void espnow_task(void *param)
{
	xEspReceivedEncKeyQueue = xQueueCreate(1, sizeof(received_enc_key));
	if (xEspReceivedEncKeyQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xEspReceivedEncKeyQueue semaphore");
	}
	
	esp_err_t err = esp_lora_key_nvs_load(received_enc_key, LORA_ENC_NS, LORA_ENC_FMT);
	if (err == ESP_OK) {
	    // If key existed, send
	    xQueueSend(xEspReceivedEncKeyQueue, received_enc_key, portMAX_DELAY);
	}
	    
	while (1) {
		
		// If ESPNOW pair button pressed
		if (gpio_get_level(PAIR_BTN1_PIN) == 0) {
			vTaskDelay(pdMS_TO_TICKS(50)); // Ignore bounce window
			
			// Start radio and initialize ESP-NOW
			ESP_ERROR_CHECK(esp_funcs_wifi_radio_start(WIFI_CHANNEL));
			ESP_ERROR_CHECK(esp_now_init());
			
			// Register receive callback
    		ESP_ERROR_CHECK(esp_funcs_espnow_register_recv_cb(on_data_recv));
    		
    		gpio_set_level(RGB_GREEN_PIN, 1); // Set green
		}
		
		if (listen_triggered) {
			// Stop radio and de-initialize ESP-NOW
		    ESP_ERROR_CHECK(esp_funcs_espnow_deinit());
		    ESP_ERROR_CHECK(esp_funcs_wifi_radio_stop());
		    
		    gpio_set_level(RGB_GREEN_PIN, 0); // Green off
		    
		    listen_triggered = false;
		}
    
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void espnow_task_create(void)
{
    if (xTaskCreate(espnow_task, "espnow_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start espnow_task");
	}
}