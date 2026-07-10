#include <string.h>

#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "nvs.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "gpio_funcs.h"
#include "freertos/projdefs.h"
#include "gpio_task.h"

#include "lora_radio.h"

#define ADC_CH ADC_CHANNEL_4
#define NUM_ADC_SAMPLES 16384

#define PLUG_STATE_NS  "plug_state" // NVS namespace
#define PLUG_STATE_KEY "relay"      // Last commanded relay level (0 or 1)

// SPI device handles
spi_device_handle_t spi_sx126x; // For SX126x

// Global SX126x instance
sx126x_t sx126x;

// One-shot binary GPIO output timer
static esp_timer_handle_t gpio_out_pulse_timer = NULL;

static bool relay_on = false;

static const char *TAG = "GPIO_FUNCS";

// Allowed times
static const char *time_opts[] = {
	"1m",  "3m",  "5m",  "15m",
	"30m", "45m", "1h",  "2h",
	"3h",  "4h",  "6h",  "8h",
	"12h", "16h", "18h", "24h"
};

// Allowed times' equivalent delays in ticks
// Need to use division since pdMS_TO_TICKS macro truncates to uint32_t
static const TickType_t time_ticks[] = {
	( 1ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, //  1 m
	( 3ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, //  3 m
	( 5ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, //  5 m
	(15ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 15 m
	(30ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 30 m
	(45ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 45 m
	( 1ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 1 h
	( 2ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 2 h
	( 3ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 3 h
	( 4ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 4 h
   ( 6ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 6 h
   ( 8ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 8 h
   (12ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 12 h
   (16ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 16 h
   (18ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS, // 18 h
   (24ULL * 60ULL * 60ULL * 1000ULL) / portTICK_PERIOD_MS  // 24 h
};

void gpio_init(void)
{
	// Config tick rate check
	if (time_ticks[14] != 6480000) {
		ESP_LOGE(TAG, "18 h constant is wrong - overflow likely! Got: %" PRIu32, time_ticks[14]);
	}
	
	// Configure outputs
	gpio_config_t io_conf_out = {
		.pin_bit_mask = (1ULL << RELAY_PIN) |
						// RGB
						(1ULL << RGB_RED_PIN) |
						(1ULL << RGB_GREEN_PIN) |
						(1ULL << RGB_BLUE_PIN) |
						// GPIO
						(1ULL << GPIO_BIT_1_PIN) |
						(1ULL << GPIO_BIT_2_PIN) |
						(1ULL << GPIO_BIT_3_PIN) |
						(1ULL << GPIO_BIT_4_PIN) |
						(1ULL << GPIO_BIT_5_PIN),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE
	};
	gpio_config(&io_conf_out);
	
	// Ensure outputs start low
	gpio_set_level(RELAY_PIN, 0);
	
	gpio_set_level(RGB_RED_PIN, 0);
	gpio_set_level(RGB_GREEN_PIN, 0);
	gpio_set_level(RGB_BLUE_PIN, 0);
	
	gpio_set_level(GPIO_BIT_1_PIN, 0);
	gpio_set_level(GPIO_BIT_2_PIN, 0);
	gpio_set_level(GPIO_BIT_3_PIN, 0);
	gpio_set_level(GPIO_BIT_4_PIN, 0);
	gpio_set_level(GPIO_BIT_5_PIN, 0);

	// Configure inputs
	gpio_config_t io_conf_in = {
		.pin_bit_mask = (1ULL << PAIR_BTN1_PIN) |
						(1ULL << PAIR_BTN2_PIN),
		.mode = GPIO_MODE_INPUT,
		.intr_type = GPIO_INTR_DISABLE,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf_in);
}

void gpio_spi_init(void) 
{
	esp_err_t ret;

	// Configure the SPI bus pins
	spi_bus_config_t buscfg = {
		.miso_io_num = SPI_MISO_PIN,  // SX1262 MISO
		.mosi_io_num = SPI_MOSI_PIN,  // MOSI
		.sclk_io_num = SPI_SCLK_PIN,  // SCLK
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 512	   // only need small transfers for registers
	};
	ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
	ESP_ERROR_CHECK(ret);

	// Attach SX1262 as a half-duplex device
	spi_device_interface_config_t sx1262_devcfg = {
		.mode		   = 0,
		.clock_speed_hz = 1 * 1000 * 1000,	// 1 MHz
		.spics_io_num   = SX126X_CS_PIN,	  // CS pin for SX1262
		.queue_size	 = 1,				  // just one outstanding transaction
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

	// Enforce minimum of 1 second to prevent 0-tick busy loops
	if (total_seconds < 1) {
		total_seconds = 1;
	}

	// Convert from seconds to FreeRTOS ticks
	return pdMS_TO_TICKS((uint64_t)total_seconds * 1000ULL);
}

void gpio_relay_toggle(bool on)
{
	if (on) {
		relay_on = true;
	
		// Relay ON
		gpio_set_level(RELAY_PIN, 1);
		
		// Red RGB ON
		gpio_set_level(RGB_RED_PIN, 1);
	} else {
		relay_on = false;
	
		// Relay OFF
		gpio_set_level(RELAY_PIN, 0);
		
		// Red RGB OFF
		gpio_set_level(RGB_RED_PIN, 0);
	}

	gpio_state_save_relay(on); // Persist commanded state
}

void gpio_rgb_ready_to_rx(bool ready)
{
	// If ready to receive
	if (ready) {
		if (relay_on) {
			gpio_set_level(RGB_RED_PIN, 0); // Red off
		}
		gpio_set_level(RGB_BLUE_PIN, 1); // Set Blue
	}
	// Already received
	else {
		if (relay_on) {
			gpio_set_level(RGB_RED_PIN, 1); // Put back red
		}
		gpio_set_level(RGB_BLUE_PIN, 0); // Blue off
	}
}

void gpio_rgb_wifi_status(bool connected)
{
	// If ready to receive
	if (connected) {
		gpio_set_level(RGB_GREEN_PIN, 1); // Set green
	}
	// Already received
	else {
		gpio_set_level(RGB_GREEN_PIN, 0); // Green off
	}
}

void gpio_rgb_set(int r_on, int g_on, int b_on)
{
	gpio_set_level(RGB_RED_PIN, r_on ? 1 : 0);
	gpio_set_level(RGB_GREEN_PIN, g_on ? 1 : 0);
	gpio_set_level(RGB_BLUE_PIN, b_on ? 1 : 0);
}

void gpio_rgb_cycle_tick(uint32_t period_ms)
{
	static uint8_t phase = 0; // 0 = R, 1 = G, 2 = B
	static TickType_t last = 0;

	TickType_t now = xTaskGetTickCount();
	if (now - last < pdMS_TO_TICKS(period_ms)) {
		return; // Not time yet
	}
	last = now;

	switch (phase) {
		case 0: gpio_rgb_set(1, 0, 0); break; // Red
		case 1: gpio_rgb_set(0, 1, 0); break; // Green
		default: gpio_rgb_set(0, 0, 1); break; // Blue
	}
	phase = (phase + 1) % 3;
}

static void gpio_bus_timeout_cb(void *arg) {
	// Clear all lines low when the timer fires
	gpio_set_level(GPIO_BIT_1_PIN, 0); // MSB
	gpio_set_level(GPIO_BIT_2_PIN, 0);
	gpio_set_level(GPIO_BIT_3_PIN, 0);
	gpio_set_level(GPIO_BIT_4_PIN, 0);
	gpio_set_level(GPIO_BIT_5_PIN, 0); // LSB
}

void gpio_pulse_5bit_bus(uint8_t value, uint32_t pulse_ms)
{
	// Create the one-shot timer
	if (gpio_out_pulse_timer == NULL) {
		const esp_timer_create_args_t args = {
			.callback = gpio_bus_timeout_cb,
			.arg = NULL,
			.dispatch_method = ESP_TIMER_TASK,
			.name = "gpioOutBin"
		};
		ESP_ERROR_CHECK(esp_timer_create(&args, &gpio_out_pulse_timer));
	}

	// If a previous pulse is active, stop it (replacing)
	if (esp_timer_is_active(gpio_out_pulse_timer)) {
		ESP_ERROR_CHECK(esp_timer_stop(gpio_out_pulse_timer));
	}

	// Use low 5 bits (maps MSB -> LSB)
	uint8_t bits = value & 0b00011111;

	// Drive highs/lows together to minimize skew
	gpio_set_level(GPIO_BIT_1_PIN, (bits >> 4) & 1); // Bit4
	gpio_set_level(GPIO_BIT_2_PIN, (bits >> 3) & 1); // Bit3
	gpio_set_level(GPIO_BIT_3_PIN, (bits >> 2) & 1); // Bit2
	gpio_set_level(GPIO_BIT_4_PIN, (bits >> 1) & 1); // Bit1
	gpio_set_level(GPIO_BIT_5_PIN, (bits >> 0) & 1); // Bit0

	// Arm one-shot to clear after pulse_ms
	ESP_ERROR_CHECK(esp_timer_start_once(gpio_out_pulse_timer, (uint64_t)pulse_ms * 1000ULL));
}

void gpio_state_save_relay(bool on)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(PLUG_STATE_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS for relay state: %s", esp_err_to_name(err));
		return;
	}

	err = nvs_set_u8(h, PLUG_STATE_KEY, on ? 1 : 0);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to save relay state: %s", esp_err_to_name(err));
	}

	nvs_close(h);
}

bool gpio_state_load_relay(bool *on)
{
	if (on == NULL) {
		ESP_LOGE(TAG, "gpio_state_load_relay: Invalid argument: on pointer is NULL");
		return false; // Invalid argument
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(PLUG_STATE_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		if (err != ESP_ERR_NVS_NOT_FOUND) {
			ESP_LOGE(TAG, "gpio_state_load_relay: Failed to open NVS: %s", esp_err_to_name(err));
		}
		return false; // Namespace absent = nothing saved yet
	}

	uint8_t v = 0;
	err = nvs_get_u8(h, PLUG_STATE_KEY, &v);
	nvs_close(h);

	if (err != ESP_OK) {
		return false;
	}

	*on = (v != 0);
	return true;
}