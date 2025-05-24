#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "espnow_funcs.h"
#include "portmacro.h"
#include "espnow_task.h"

#define TAG "ESPNOW_TASK"

uint8_t received_enc_key[ENC_KEY_LEN] = {0};

QueueHandle_t xReceivedEncKeyQueue;

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
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(xReceivedEncKeyQueue, received_enc_key, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void espnow_task(void *param)
{
	xReceivedEncKeyQueue = xQueueCreate(1, sizeof(received_enc_key));
	if (xReceivedEncKeyQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xReceivedEncKeyQueue semaphore");
	}
	
	esp_lora_key_nvs_load(received_enc_key, LORA_ENC_NS, LORA_ENC_FMT);
	
	if (received_enc_key[0] != 0) {
		xQueueSend(xReceivedEncKeyQueue, received_enc_key, portMAX_DELAY);
	}
	
    // Start radio and initialize ESP-NOW
	ESP_ERROR_CHECK(esp_funcs_wifi_radio_start(WIFI_CHANNEL));
	ESP_ERROR_CHECK(esp_now_init());
	
	//ESP_ERROR_CHECK(esp_funcs_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL)); // Configures peer
	// Register receive callback
    ESP_ERROR_CHECK(esp_funcs_espnow_register_recv_cb(on_data_recv));
    
	while (1) {
		
		// Stop radio and de-initialize ESP-NOW
	    //ESP_ERROR_CHECK(esp_funcs_espnow_deinit());
	    //ESP_ERROR_CHECK(esp_funcs_wifi_radio_stop());
    
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void espnow_task_create(void)
{
    if (xTaskCreate(espnow_task, "espnow_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start espnow_task");
	}
}