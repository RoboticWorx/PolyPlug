#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"

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

void gpio_init(void)
{
	// Configure outputs
	gpio_config_t io_conf_out = {
	    .pin_bit_mask = (1ULL << RELAY_PIN), 
	    .mode           = GPIO_MODE_OUTPUT,
	    .pull_up_en     = GPIO_PULLUP_DISABLE,
	    .pull_down_en   = GPIO_PULLDOWN_DISABLE,
	    .intr_type      = GPIO_INTR_DISABLE
	};
	gpio_config(&io_conf_out);
	
	gpio_set_level(RELAY_PIN, 0);
	
	// Configure inputs
	/*gpio_config_t io_conf_in = {
	    .pin_bit_mask = (1ULL << TCA9535_INT_GPIO),
	    .mode         = GPIO_MODE_INPUT,
	    .intr_type    = GPIO_INTR_NEGEDGE,
	    .pull_up_en     = GPIO_PULLUP_DISABLE,
	    .pull_down_en   = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf_in);*/
	
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
