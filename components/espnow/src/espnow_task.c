#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "espnow_funcs.h"
#include "espnow_task.h"

#define TAG "ESPNOW_TASK"

//static const uint8_t UNIVERSAL_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int data_len) {
    char buf[251];
    int len = data_len < sizeof(buf) ? data_len : (sizeof(buf)-1);
    memcpy(buf, data, len);
    buf[len] = '\0';

    const uint8_t *mac = info->src_addr;
    ESP_LOGI("ESPNOW_RX",
             "From %02X:%02X:%02X:%02X:%02X:%02X   \"%s\"",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
             buf);
}

static void espnow_task(void *param)
{
    // Start radio and initialize ESP-NOW
	ESP_ERROR_CHECK(esp_funcs_wifi_radio_start(WIFI_CHANNEL));
	ESP_ERROR_CHECK(esp_now_init());
	
	//ESP_ERROR_CHECK(esp_funcs_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL)); // Configures peer
    
	while (1) {
		
	    // Register receive callback
    	ESP_ERROR_CHECK(esp_funcs_espnow_register_recv_cb(on_data_recv));
		
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