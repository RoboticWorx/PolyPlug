#include "polyplug_macros.h"

#include "esp_log.h"

#include "portmacro.h"
#include "sx126x_hal.h"
#include "lora_radio.h"
#include "gpio_funcs.h"

static const char *TAG = "LORA_RADIO";

void lora_radio_set_rx_mode(uint8_t max_payload_len)
{
	// Poll for SX1262 to be ready
	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	// Restore max payload length for receiving
	sx126x_pkt_params_lora_t pkt_params = {
		.preamble_len_in_symb = 12,
		.header_type = SX126X_LORA_PKT_EXPLICIT,
		.pld_len_in_bytes = max_payload_len,
		.crc_is_on = true,
		.invert_iq_is_on = false,
	};
	sx126x_status_t status = sx126x_set_lora_pkt_params(NULL, &pkt_params);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set RX packet params");
		return;
	}

	// Enter RX mode
	status = sx126x_set_rx(NULL, SX126X_RX_SINGLE_MODE);

	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to enter RX mode\n");
		return;
	}
}

void lora_radio_tx(uint8_t tx_data[], uint8_t data_len)
{
	// Poll for SX1262 to be ready
	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	// Update payload length for this transmission
	sx126x_pkt_params_lora_t pkt_params = {
		.preamble_len_in_symb = 12,
		.header_type = SX126X_LORA_PKT_EXPLICIT,
		.pld_len_in_bytes = data_len,
		.crc_is_on = true,
		.invert_iq_is_on = false,
	};
	sx126x_status_t status = sx126x_set_lora_pkt_params(NULL, &pkt_params);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set packet params");
		return;
	}

	status = sx126x_write_buffer(NULL, 0, tx_data, data_len);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to write to buffer\n");
	}

	// Start transmission
	status = sx126x_set_tx(NULL, SX126X_MAX_TIMEOUT_IN_MS);

	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to start transmission\n");
	}
}
