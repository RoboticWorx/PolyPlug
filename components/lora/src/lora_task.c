#include "polyplug_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_system.h"
#include "espnow_task.h"

#include "sx126x.h"
#include "lora_task.h"
#include "lora_pcp.h"
#include "lora_radio.h"

static const char *TAG = "LORA_TASK";

static SemaphoreHandle_t xLoraEventSemaphore;
static SemaphoreHandle_t xTXDoneSemaphore;

// Re-arm request flag:
// Only lora_task arms the radio and clears this flag; the handler and recovery paths only ever set it
static atomic_bool rx_needs_rearm = false;

// Handle of lora_task so the handler can wake it immediately for a re-arm
static TaskHandle_t s_lora_task_handle = NULL;

// Consecutive IRQ-recovery failures tolerated before rebooting to recover
#define LORA_IRQ_RECOVERY_MAX_FAILS 20

static void lora_event_handler_task(void *pvParameters);

// Request lora_task to (re)arm RX. Single-writer entry point for the handler
static void lora_request_rearm(void)
{
	atomic_store(&rx_needs_rearm, true);
	if (s_lora_task_handle != NULL) {
		xTaskNotifyGive(s_lora_task_handle);
	}
}

static uint8_t enc_key_buf[LORA_PCP_ENC_KEY_LEN] = {0};

// ISR handler for DIO1
static void IRAM_ATTR dio1_isr_handler(void *arg)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	// Signal the event handler task
	xSemaphoreGiveFromISR(xLoraEventSemaphore, &xHigherPriorityTaskWoken);
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// LoRa Task
static void lora_task(void *pvParameters)
{
	// Create the semaphore for LoRa events
	xLoraEventSemaphore = xSemaphoreCreateBinary();
	if (xLoraEventSemaphore == NULL) {
		ESP_LOGE(TAG, "Failed to create LoRa event semaphore");
	}
	configASSERT(xLoraEventSemaphore);

	xTXDoneSemaphore = xSemaphoreCreateBinary();
	if (xTXDoneSemaphore == NULL) {
		ESP_LOGE(TAG, "Failed to create TX_DONE semaphore");
	}
	configASSERT(xTXDoneSemaphore);

	// Create the LoRa event handler task
	if (xTaskCreate(lora_event_handler_task, "lora_event_handler", 1024 * 3, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to create lora_event_handler_task");
	}

	// Spreading factor synced from the remote (defaults to SF7 until the first pairing)
	uint8_t lora_sf = lora_pcp_load_sf_nvs();

	sx126x_mod_params_lora_t lora_mod_params = {
		.sf = (sx126x_lora_sf_t)lora_sf, // Spreading factor (higher value sends further but takes more time)
		.bw = SX126X_LORA_BW_125, // Bandwidth
		.cr = SX126X_LORA_CR_4_5, // Error correction
		.ldro = (lora_sf > SX126X_LORA_SF10) ? 1 : 0, // 1 if SF > 10 (SF11/SF12 at BW125)
	};

	sx126x_pkt_params_lora_t lora_pkt_params = {
		.preamble_len_in_symb = 12,
		.header_type = SX126X_LORA_PKT_EXPLICIT,
		.pld_len_in_bytes = LORA_PCP_PAYLOAD_LENGTH,
		.crc_is_on = true,
		.invert_iq_is_on = false,
	};

	// Define the PA configuration parameters
	sx126x_pa_cfg_params_t pa_config = {
		.pa_duty_cycle = 0x04, // Duty cycle setting
		.hp_max = 0x07, // Maximum output power
		.device_sel = 0x00, // Select SX1262-specific PA configuration
		.pa_lut = 0x01 // Default LUT (Look-Up Table)
	};

	sx126x_hal_reset(NULL);

	vTaskDelay(pdMS_TO_TICKS(10));

	sx126x_status_t status = sx126x_init_retention_list(NULL);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to init retention list");
	}

	status = sx126x_set_reg_mode(NULL, SX126X_REG_MODE_LDO);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set reg mode");
	}

	status = sx126x_set_dio2_as_rf_sw_ctrl(NULL, true);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set dio2 as rf switch");
	}

	status = sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set standby");
	}

	status = sx126x_cal(NULL, SX126X_CAL_ALL);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to calibrate");
	}

	status = sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set packet type");
	}

	status = sx126x_set_rf_freq(NULL, 915000000);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set frequency");
	}

	status = sx126x_cal_img_in_mhz(NULL, 902, 928);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to calibrate image");
    }

	status = sx126x_set_pa_cfg(NULL, &pa_config);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set PA configuration");
	}

	sx126x_ramp_time_t ramp_time = SX126X_RAMP_200_US; // 200 us ramp time
	status = sx126x_set_tx_params(NULL, (int8_t)22, ramp_time); // 22dBm
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set TX params");
	}
	
	status = sx126x_cfg_tx_clamp(NULL); // SX1262 §15.2 PA-clamp init workaround
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to configure TX clamp");
    }

	//sx126x_set_rx_tx_fallback_mode // Default is RC standby

	status = sx126x_cfg_rx_boosted(
		NULL, true); // More sensitive RX at cost of more power
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to configure RX boost mode");
	}

	status = sx126x_set_lora_mod_params(NULL, &lora_mod_params);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set LoRa modulation parameters");
	}

	status = sx126x_set_lora_pkt_params(NULL, &lora_pkt_params);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set LoRa packet parameters");
	}

	status = sx126x_set_lora_sync_word(NULL, 0x62);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set LoRa sync word");
	}

	status = sx126x_set_dio_irq_params(
		NULL,
		SX126X_IRQ_ALL, // Enable all IRQs
		SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT | SX126X_IRQ_HEADER_ERROR | SX126X_IRQ_CRC_ERROR, // Enable IRQ finished
		SX126X_IRQ_NONE, // No IRQs mapped to DIO2
		SX126X_IRQ_NONE // No IRQs mapped to DIO3
	);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set DIO IRQ parameters");
	}
	sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL); // Clear IRQs at start

	// Set up DIO1 interrupt for RX
	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << SX126X_DIO1_PIN),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_POSEDGE, // Trigger on rising edge
	};
	gpio_config(&io_conf);

	gpio_install_isr_service(0);
	gpio_isr_handler_add(SX126X_DIO1_PIN, dio1_isr_handler, NULL);
	
	lora_pcp_init(); // Load persisted replay counter

	int rearm_fails = 0;

	// Enter RX mode now; the loop retries on failure
	if (!lora_radio_set_rx_mode(LORA_PCP_PAYLOAD_LENGTH)) {
		atomic_store(&rx_needs_rearm, true);
	}

	while (1) {
		// Wake immediately on a re-arm request from the handler, otherwise every 50ms
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

		// If new encryption key received (will always happen at least once on boot)
		if (xQueueReceive(xEspReceivedEncKeyQueue, enc_key_buf, 0) == pdTRUE) {
			lora_pcp_set_key(enc_key_buf);

			// A pairing may also have changed the spreading factor (saved to NVS by the ESP-NOW receive path)
			// Re-apply it live so the plug's RX keeps matching the remote's TX without a reboot
			uint8_t new_sf = lora_pcp_load_sf_nvs();
			if (new_sf != lora_sf) {
				// Only enter the write sequence if standby actually took
				if (sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC) == SX126X_STATUS_OK) {
					lora_mod_params.sf = (sx126x_lora_sf_t)new_sf;
					lora_mod_params.ldro = (new_sf > SX126X_LORA_SF10) ? 1 : 0;

					// Commit the new SF only once the write confirms
					if (sx126x_set_lora_mod_params(NULL, &lora_mod_params) == SX126X_STATUS_OK) {
						lora_sf = new_sf;
					} else {
						ESP_LOGE(TAG, "Failed to write new LoRa SF %u", new_sf);
					}

					// Re-arm regardless so we never get stuck in standby
					atomic_store(&rx_needs_rearm, true);
				} else {
					ESP_LOGE(TAG, "Failed to enter standby for LoRa SF change");
				}
			}
		}

		// lora_task is the sole owner of set_rx_mode and of clearing rx_needs_rearm
		if (atomic_exchange(&rx_needs_rearm, false)) {
			if (lora_radio_set_rx_mode(LORA_PCP_PAYLOAD_LENGTH)) {
				rearm_fails = 0;
			} else {
				atomic_store(&rx_needs_rearm, true); // Keep the request pending
				if (++rearm_fails >= LORA_IRQ_RECOVERY_MAX_FAILS) {
					ESP_LOGE(TAG, "RX re-arm wedged %d times, restarting", rearm_fails);
					esp_restart();
				}
			}
		}

		// DIO1 is edge-triggered, so a missed or latched IRQ can leave the line high
		if (gpio_get_level(SX126X_DIO1_PIN)) { // Nudge the handler to read/clear the pending IRQ
			xSemaphoreGive(xLoraEventSemaphore);
		}
	}
}

static void lora_event_handler_task(void *pvParameters)
{
	int irq_recovery_fails = 0;

	while (1) {
		// Wait for an event from the ISR
		if (xSemaphoreTake(xLoraEventSemaphore, portMAX_DELAY) == pdTRUE) {
			// Read IRQ flags
			// A failed read leaves radio state unknown: clear
			uint16_t irq_flags = 0;
			if (sx126x_get_irq_status(NULL, &irq_flags) != SX126X_STATUS_OK) {
				ESP_LOGE(TAG, "Failed to read IRQ status, recovering RX");
				if (sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL) == SX126X_STATUS_OK) {
					// Latched IRQ released - have lora_task re-arm RX
					irq_recovery_fails = 0;
					lora_request_rearm();
				} else if (++irq_recovery_fails >= LORA_IRQ_RECOVERY_MAX_FAILS) {
					// SPI/BUSY bus is wedged - re-arming would fail too, so reboot to recover
					ESP_LOGE(TAG, "IRQ recovery failed %d times, restarting", irq_recovery_fails);
					esp_restart();
				} else {
					// Transient - retry the read/clear sequence shortly
					vTaskDelay(pdMS_TO_TICKS(50));
					xSemaphoreGive(xLoraEventSemaphore);
				}
				continue;
			}
			irq_recovery_fails = 0; // Healthy read - reset the failure counter

			// Successfully read the radio's IRQ register, but found nothing pending
			// Clear any stray latch but do not force a re-arm - the radio is still armed
			if (irq_flags == 0) {
				sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL);
				continue;
			}

			// Clear read flags up front so a co-latched flag can never be left set
			sx126x_clear_irq_status(NULL, irq_flags);

			// Check RX errors first - SX126X sets both CRC_ERROR and RX_DONE
			// on a corrupted packet, so errors must take priority
			if (irq_flags & SX126X_IRQ_HEADER_ERROR) {
				#ifdef POLYPLUG_DEBUG
				ESP_LOGE(TAG, "Header error in received packet");
				#endif
				lora_request_rearm();
			} else if (irq_flags & SX126X_IRQ_CRC_ERROR) {
				#ifdef POLYPLUG_DEBUG
				ESP_LOGE(TAG, "CRC error in received packet");
				#endif
				lora_request_rearm();
			} else if (irq_flags & SX126X_IRQ_TX_DONE) { // If transmission complete
				#ifdef POLYPLUG_DEBUG
				ESP_LOGI(TAG, "Transmission completed");
				#endif
				lora_request_rearm();
			} else if (irq_flags & SX126X_IRQ_RX_DONE) { // RX complete (no CRC/header error)
				// Read the received packet

				uint8_t rx_buffer[LORA_PCP_PAYLOAD_LENGTH];
				uint8_t rx_size = 0;

				sx126x_rx_buffer_status_t rx_status;

				// Check RX
				sx126x_get_rx_buffer_status(NULL, &rx_status);

				// Get size of packet
				rx_size = rx_status.pld_len_in_bytes;

				// Validate size
				if (rx_size == 0 || rx_size > sizeof(rx_buffer)) {
					#ifdef POLYPLUG_DEBUG
					ESP_LOGW(TAG, "Invalid RX size %d, discarding", rx_size);
					#endif
					lora_request_rearm();
					continue;
				}

				// Read data into buffer
				sx126x_read_buffer(NULL, rx_status.buffer_start_pointer, rx_buffer, rx_size);

				#ifdef POLYPLUG_DEBUG
				ESP_LOGI(TAG, "Received packet of size %d", rx_size);
				#endif

				// Process received
				lora_pcp_process_received_message(rx_buffer, rx_size);

				// Log RSSI
				#ifdef POLYPLUG_DEBUG
				sx126x_pkt_status_lora_t pkt_status;
				if (sx126x_get_lora_pkt_status(NULL, &pkt_status) == SX126X_STATUS_OK) {
					ESP_LOGI(TAG, "Packet RSSI: %d dBm, SignalRSSI: %d dBm, SNR: %d dB",
							pkt_status.rssi_pkt_in_dbm,
							pkt_status.signal_rssi_pkt_in_dbm,
							pkt_status.snr_pkt_in_db);
				}
				else {
					ESP_LOGW(TAG, "Failed to get packet status");
				}
				#endif

				// Send ACK if valid; if no TX started, re-arm RX now
				if (!lora_pcp_send_receipt()) {
					lora_request_rearm();
				}
			} else if (irq_flags & SX126X_IRQ_TIMEOUT) {
				#ifdef POLYPLUG_DEBUG
				ESP_LOGW(TAG, "RX timeout occurred");
				#endif
				lora_request_rearm();
			}
		}
	}
}

// Function to create the LoRa task
void lora_task_create(void)
{
	// Create the LoRa task
	if (xTaskCreate(lora_task, "lora_task", 1024 * 2, NULL, tskIDLE_PRIORITY + 1, &s_lora_task_handle) != pdPASS) {
		ESP_LOGE(TAG, "Failed to create lora_task");
	}
}