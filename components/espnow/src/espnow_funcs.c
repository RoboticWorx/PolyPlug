#include <string.h>

#include "nvs.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_err.h"

#include "espnow_funcs.h"
#include "espnow_task.h"
#include "lora_pcp.h"

#define TAG "ESP_FUNCS"

#define LORA_ENC_NS "es_lo_ns" // NVS namespace
#define LORA_ENC_NS_KEY "es_lo_fmt" // FMT

extern uint8_t received_enc_key[LORA_PCP_ENC_KEY_LEN];

esp_err_t espnow_funcs_wifi_driver_init(void)
{
	// Bring up TCP/IP stack and default event loop
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	
	// Create netif object
	esp_netif_create_default_wifi_sta();
	
	// Initialize Wi-Fi driver
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	
	// Set as a station
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	
	return ESP_OK;
}

esp_err_t espnow_funcs_wifi_radio_start(uint8_t channel)
{
	// Start Wi-Fi
	ESP_ERROR_CHECK(esp_wifi_start());
	
	// Set the channel
	ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
	
	return ESP_OK;
}

esp_err_t espnow_funcs_espnow_register_recv_cb(esp_now_recv_cb_t cb)
{
	// Register receiver callback
	esp_err_t err = esp_now_register_recv_cb(cb);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "recv_cb register failed: %s", esp_err_to_name(err));
	}
	
	return err;
}

esp_err_t espnow_funcs_lora_key_nvs_save(uint8_t *enc_key)
{
	nvs_handle_t h;

	// Open NVS
	esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS for writing LoRa key: %s", esp_err_to_name(err));
		return err;
	}
		
	// Store the key
	err = nvs_set_blob(h, LORA_ENC_NS_KEY, enc_key, LORA_PCP_ENC_KEY_LEN);
	if (err != ESP_OK) {
		nvs_close(h);
		ESP_LOGE(TAG, "Failed to write LoRa key to NVS: %s", esp_err_to_name(err));
		return err;
	}

	// Flush pending writes to flash
	err = nvs_commit(h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to commit LoRa key to NVS: %s", esp_err_to_name(err));
	}

	// Close NVS
	nvs_close(h);
	
	return err;
}

esp_err_t espnow_funcs_lora_key_nvs_load(uint8_t *enc_key)
{
	nvs_handle_t h;
		
	// Open NVS
	esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS for reading LoRa key: %s", esp_err_to_name(err));
		return err;
	}

	size_t len = LORA_PCP_ENC_KEY_LEN;

	err = nvs_get_blob(h, LORA_ENC_NS_KEY, enc_key, &len);
	
	// Close NVS
	nvs_close(h);
	
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		return ESP_OK;
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to read LoRa key from NVS: %s", esp_err_to_name(err));
		return err;
	}
	if (len != LORA_PCP_ENC_KEY_LEN) {
		ESP_LOGE(TAG, "Invalid LoRa key length in NVS: %u", (unsigned)len);
		memset(enc_key, 0, LORA_PCP_ENC_KEY_LEN);
		return ESP_ERR_INVALID_SIZE;
	}

	return ESP_OK;
}

