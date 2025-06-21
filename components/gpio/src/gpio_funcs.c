#include <string.h>

#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"
#include "esp_random.h"

#include "gpio_funcs.h"
#include "gpio_task.h"

#include "lora_task.h"

#define SPI_MOSI_PIN 7 // Shared MOSI
#define SPI_SCLK_PIN 6 // Shared SCLK
#define SPI_MISO_PIN 2 // MISO for SX126x (optional for ST7789)

// SPI device handles
spi_device_handle_t spi_sx126x; // For SX126x

// Global SX126x instance
sx126x_t sx126x;

static const char *TAG = "GPIO_FUNCS";

// Allowed times
static const char *time_opts[] = {
    "1m",  "3m",  "5m",  "15m",
    "30m", "45m", "1h",  "2h",
    "3h",  "4h",  "6h",  "8h",
    "12h", "16h", "18h", "24h"
};

// Allowed times' equivalent delays in ticks
static const TickType_t time_ticks[] = {
    pdMS_TO_TICKS(1 * 60 * 1000UL), //  1m
    pdMS_TO_TICKS(3 * 60 * 1000UL), //  3m
    pdMS_TO_TICKS(5 * 60 * 1000UL), //  5m
    pdMS_TO_TICKS(15 * 60 * 1000UL), // 15m
    pdMS_TO_TICKS(30 * 60 * 1000UL), // 30m
    pdMS_TO_TICKS(45 * 60 * 1000UL), // 45m
    pdMS_TO_TICKS(1 * 60 * 60 * 1000UL), // 1h
    pdMS_TO_TICKS(2 * 60 * 60 * 1000UL), // 2h
    pdMS_TO_TICKS(3 * 60 * 60 * 1000UL), // 3h
    pdMS_TO_TICKS(4 * 60 * 60 * 1000UL), // 4h
    pdMS_TO_TICKS(6 * 60 * 60 * 1000UL), // 6h
    pdMS_TO_TICKS(8 * 60 * 60 * 1000UL), // 8h
    pdMS_TO_TICKS(12 * 60 * 60 * 1000UL), // 12h
    pdMS_TO_TICKS(16 * 60 * 60 * 1000UL), // 16h
    pdMS_TO_TICKS(18 * 60 * 60 * 1000UL), // 18h
    pdMS_TO_TICKS(24 * 60 * 60 * 1000UL) // 24h
};

void gpio_init(void)
{
	// Configure outputs
	gpio_config_t io_conf_out = {
	    .pin_bit_mask = (1ULL << RELAY_PIN) |
	    				(1ULL << RGB_RED_PIN) |
	    				(1ULL << RGB_GREEN_PIN) |
	    				(1ULL << RGB_BLUE_PIN),
	    .mode = GPIO_MODE_OUTPUT,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	    .intr_type = GPIO_INTR_DISABLE
	};
	gpio_config(&io_conf_out);
	
	gpio_set_level(RELAY_PIN, 0);
	
	// Configure inputs
	gpio_config_t io_conf_in = {
	    .pin_bit_mask = (1ULL << PAIR_BTN1_PIN),
	    .mode = GPIO_MODE_INPUT,
	    .intr_type = GPIO_INTR_DISABLE,
	    .pull_up_en = GPIO_PULLUP_ENABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf_in);
	
}

void gpio_spi_init(void) 
{
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
    
    	// Initialize the SX126x HAL with the SPI handle
	sx126x_hal_init(spi_sx126x);

	// Initialize the sx126x_t structure
	sx126x.context = NULL; // Set context to NULL
	sx126x.hal_reset = sx126x_hal_reset;
	sx126x.hal_wakeup = sx126x_hal_wakeup;
	sx126x.hal_write = sx126x_hal_write;
	sx126x.hal_read = sx126x_hal_read;
}

// Lookup helper
TickType_t gpio_lookup_time_ticks(const char *s)
{
    for (size_t i = 0; i < sizeof(time_opts)/sizeof(time_opts[0]); i++) {
        if (strcmp(s, time_opts[i]) == 0) {
            return time_ticks[i];
        }
    }
    return 0; // Invalid: Zero delay
}

TickType_t gpio_get_random_ticks_from_range(int min_m, int max_m)
{
    // Convert minutes to seconds:
    int min_s = min_m * 60;
    int max_s = max_m * 60;
    
    // If passed in wrong order, swap
    if (min_s > max_s) {
		int t = min_s;
		min_s = max_s;
		max_s = t; 
	}

	// Pick a random integer in min_s to max_s
    int total_seconds = (esp_random() % (max_s - min_s + 1)) + min_s;
    
    // Convert from seconds to FreeRTOS ticks
    return pdMS_TO_TICKS((uint64_t)total_seconds * 1000ULL);
}
