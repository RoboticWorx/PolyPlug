#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_log.h"
#include "esp_err.h"

#include "lora_task.h"

#include "sx126x.h"
#include "sx126x_hal.h"

#include "espnow_funcs.h"
#include "espnow_task.h"

// Logging tag
static const char *TAG = "MAIN";

#define SPI_MOSI_PIN 7 // Shared MOSI
#define SPI_SCLK_PIN 6 // Shared SCLK
#define SPI_MISO_PIN 2 // MISO for SX126x (optional for ST7789)

// SPI device handles
spi_device_handle_t spi_sx126x; // For SX126x
spi_device_handle_t spi_st7789; // For ST7789

// Global SX126x instance
sx126x_t sx126x;

// Only sx1262 for now
static void spi_shared_init(void) {
    esp_err_t ret;

    // 1) Configure the SPI bus pins
    spi_bus_config_t buscfg = {
        .miso_io_num = SPI_MISO_PIN,  // SX1262 MISO
        .mosi_io_num = SPI_MOSI_PIN,  // MOSI
        .sclk_io_num = SPI_SCLK_PIN,  // SCLK
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512       // only need small transfers for registers
    };
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    // 2) Attach SX1262 as a half-duplex device
    spi_device_interface_config_t sx1262_devcfg = {
        .mode           = 0,
        .clock_speed_hz = 1 * 1000 * 1000,    // 1 MHz
        .spics_io_num   = SX126X_CS_PIN,      // CS pin for SX1262
        .queue_size     = 1,                  // just one outstanding transaction
    };
    ret = spi_bus_add_device(SPI2_HOST, &sx1262_devcfg, &spi_sx126x);
    ESP_ERROR_CHECK(ret);

    // (No ST7789 setup here)
}

void app_main(void) {
	
	gpio_config_t io_conf = {
        .pin_bit_mask   = 1ULL << 26,    // bit mask for the pin(s)
        .mode           = GPIO_MODE_OUTPUT,   // set as output
        .pull_up_en     = GPIO_PULLUP_DISABLE,
        .pull_down_en   = GPIO_PULLDOWN_DISABLE,
        .intr_type      = GPIO_INTR_DISABLE,  // no interrupt
    };
    gpio_config(&io_conf);
	
	esp_err_t ret = nvs_flash_init();
    
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
	
	// Initialize SPI
	spi_shared_init();
	//gpio_init();

	// Initialize the SX126x HAL with the SPI handle
	sx126x_hal_init(spi_sx126x);

	// Initialize the sx126x_t structure
	sx126x.context = NULL; // Set context to NULL
	sx126x.hal_reset = sx126x_hal_reset;
	sx126x.hal_wakeup = sx126x_hal_wakeup;
	sx126x.hal_write = sx126x_hal_write;
	sx126x.hal_read = sx126x_hal_read;

	// Create tasks
	lora_task_create();
	espnow_task_create();

	ESP_LOGI(TAG, "Main initialized and tasks created");
	
}