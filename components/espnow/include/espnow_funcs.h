#ifndef ESPNOW_FUNCS_H
#define ESPNOW_FUNCS_H

#include "esp_err.h"
#include "esp_now.h"

/**
 * @brief Initialize the Wi-Fi driver and allocate Wi-Fi buffers
 *
 * @return ESP_OK on success
 */
esp_err_t espnow_funcs_wifi_driver_init(void);

/**
 * @brief Start the Wi-Fi radio
 *
 * @return ESP_OK on success
 */
esp_err_t espnow_funcs_wifi_radio_start(uint8_t channel);

/**
 * @brief Register an ESP-NOW receive callback
 *
 * @param [in] cb Function called on each incoming packet
 *
 * @return ESP_ERR
 */
esp_err_t espnow_funcs_espnow_register_recv_cb(esp_now_recv_cb_t cb);

/**
 * @brief Save a given 16B encryption key to NVS flash
 *
 * @param [in] enc_key The key to save
 *
 * @return ESP_ERR
 */
esp_err_t espnow_funcs_lora_key_nvs_load(uint8_t *enc_key);

/**
 * @brief Load a given 16B encryption key from NVS flash
 *
 * @param [in] enc_key The key to load
 *
 * @return ESP_ERR
 */
esp_err_t espnow_funcs_lora_key_nvs_save(uint8_t *enc_key);


#endif // ESPNOW_FUNCS_H