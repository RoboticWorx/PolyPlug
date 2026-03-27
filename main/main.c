#include "polyplug_macros.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_random.h"
#include "esp_log.h"
#include "esp_err.h"

#include "sx126x.h"
#include "sx126x_hal.h"

#include "lora_task.h"
#include "espnow_funcs.h"
#include "espnow_task.h"
#include "gpio_task.h"
#include "gpio_funcs.h"
#include "wifi_task.h"

// Logging tag
static const char *TAG = "MAIN";

void app_main(void)
{
	esp_err_t ret = nvs_flash_init();
	
	//nvs_flash_erase();
    
    // Error check
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		#ifdef POLYPLUG_DEBUG
	    ESP_LOGW(TAG, "Erasing NVS partition...");
        #endif
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    ESP_ERROR_CHECK(ret);
    #ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "NVS initialized");
    #endif
	
	// Allocate Wi-Fi buffers now without fragmentation
    ESP_ERROR_CHECK(espnow_funcs_wifi_driver_init());
    ESP_ERROR_CHECK(espnow_funcs_wifi_radio_start(WIFI_CHANNEL));
	
	// Initialize
	gpio_spi_init();
	gpio_init();

	// Create tasks
	gpio_task_create();
	lora_task_create();
	espnow_task_create();
	wifi_task_create();

	#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "Main initialized and tasks created");
	#endif
}