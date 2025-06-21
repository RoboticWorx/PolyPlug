#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_random.h"
#include "esp_log.h"
#include "esp_err.h"

#include "lora_task.h"

#include "sx126x.h"
#include "sx126x_hal.h"

#include "espnow_funcs.h"
#include "espnow_task.h"

#include "gpio_task.h"
#include "gpio_funcs.h"

// Logging tag
static const char *TAG = "MAIN";

void app_main(void) {
	
	esp_err_t ret = nvs_flash_init();
	
	//nvs_flash_erase();
    
    // Error check
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
	
	// Allocate Wi-Fi buffers now without fragmentation
    ESP_ERROR_CHECK(esp_funcs_wifi_driver_init());
    // Turn off radio to save power
    ESP_ERROR_CHECK(esp_funcs_wifi_radio_stop());
	
	// Initialize
	gpio_spi_init();
	gpio_init();

	// Create tasks
	gpio_task_create();
	lora_task_create();
	espnow_task_create();

	ESP_LOGI(TAG, "Main initialized and tasks created");
	
}