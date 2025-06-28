#ifndef ESPNOW_FUNCS_H
#define ESPNOW_FUNCS_H

#include "esp_err.h"
#include "esp_now.h"

#define LORA_ENC_NS "es_lo_ns" // NVS namespace
#define LORA_ENC_FMT "es_lo_fmt" // FMT

esp_err_t espnow_funcs_lora_key_nvs_load(uint8_t *enc_key, const char* ns, const char* fmt);
esp_err_t espnow_funcs_lora_key_nvs_save(uint8_t *enc_key, const char* ns, const char* fmt);

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
 * @brief Stop the Wi-Fi radio
 *
 * @return ESP_OK on success
 */
esp_err_t espnow_funcs_wifi_radio_stop(void);

/**
 * @brief De-initialize ESP-NOW
 *
 * @return ESP_OK on success
 */
esp_err_t espnow_funcs_espnow_deinit(void);

/**
 * @brief Register an ESP-NOW receive callback
 *
 * @param cb Function called on each incoming packet
 *
 * @return ESP_OK on success
 */
esp_err_t espnow_funcs_espnow_register_recv_cb(esp_now_recv_cb_t cb);


#endif // ESPNOW_FUNCS_H