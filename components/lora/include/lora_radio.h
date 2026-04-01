#ifndef LORA_RADIO_H
#define LORA_RADIO_H

#include <stdbool.h>
#include <stdint.h>

#include "sx126x.h"
#include "sx126x_hal.h"

typedef struct sx126x_s {
    void *context;
    sx126x_hal_status_t (*hal_write)(const void *context, const uint8_t *command, const uint16_t command_length, const uint8_t *data, const uint16_t data_length);
    sx126x_hal_status_t (*hal_read)(const void *context, const uint8_t *command, const uint16_t command_length, uint8_t *data, const uint16_t data_length);
    sx126x_hal_status_t (*hal_reset)(const void *context);
    sx126x_hal_status_t (*hal_wakeup)(const void *context);
} sx126x_t;

/**
 * @brief Sets SX1262 radio in one-shot receive mode
 *
 * @param [in] max_payload_len Maximum expected payload length in bytes
 *
 * @return True on success, false on failure (caller should retry)
 */
bool lora_radio_set_rx_mode(uint8_t max_payload_len);

/**
 * @brief Transmit raw data over LoRa
 *
 * @param [in] tx_data The data to transmit
 * @param [in] data_len Length of the data to transmit
 *
 * @return True on TX success, false on failure
 */
bool lora_radio_tx(uint8_t tx_data[], uint8_t data_len);

#endif // LORA_RADIO_H
