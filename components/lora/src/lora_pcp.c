#include "polyplug_macros.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "portmacro.h"
#include "lora_pcp.h"
#include "lora_radio.h"
#include "lora_task.h"
#include "espnow_task.h"
#include "gpio_funcs.h"
#include "gpio_task.h"

#include "psa/crypto.h"

static const char *TAG = "LORA_PCP";

static relay_t relay_tx;

static psa_key_id_t pcp_key_id = 0;
static bool key_initialized = false;

static bool valid_data_rec = false;
static uint32_t last_msg_id = 0;
static SemaphoreHandle_t xPcpMutex = NULL;

#define PCP_NVS_NS    "pcp"
#define PCP_NVS_RX_ID "last_msg_id"

#define LORA_CFG_NVS_NS     "lora_cfg" // Radio config kept separate from the replay counter
#define LORA_CFG_NVS_SF     "sf"       // Persisted spreading factor (received from the remote)
#define LORA_CFG_NVS_REGION "region"   // Persisted region / RF band (received from the remote)

#define PCP_CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, LORA_PCP_MIC_LENGTH)

uint8_t lora_pcp_load_sf_nvs(void)
{
	nvs_handle_t h;
	uint8_t sf = LORA_PCP_SF_DEFAULT; // Default matches the remote when nothing is stored yet

	// Open read-only; a missing namespace/key or out-of-range value falls back to the default
	if (nvs_open(LORA_CFG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
		uint8_t stored;
		if (nvs_get_u8(h, LORA_CFG_NVS_SF, &stored) == ESP_OK &&
		    stored >= LORA_PCP_SF_MIN && stored <= LORA_PCP_SF_MAX) {
			sf = stored;
		}
		nvs_close(h);
	}

#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "Loaded LoRa SF=%u", sf);
#endif

	return sf;
}

esp_err_t lora_pcp_save_sf_nvs(uint8_t sf)
{
	// Clamp to the supported range so a bad value can never reach the radio
	if (sf < LORA_PCP_SF_MIN) {
		sf = LORA_PCP_SF_MIN;
	} else if (sf > LORA_PCP_SF_MAX) {
		sf = LORA_PCP_SF_MAX;
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(LORA_CFG_NVS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_sf_nvs: NVS open failed: %s", esp_err_to_name(err));
		return err;
	}

	err = nvs_set_u8(h, LORA_CFG_NVS_SF, sf);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_sf_nvs: NVS write failed: %s", esp_err_to_name(err));
	}

	nvs_close(h);
	return err;
}

lora_region_t lora_pcp_load_region_nvs(void)
{
	nvs_handle_t h;
	lora_region_t region = LORA_REGION_DEFAULT; // Default matches the remote when nothing is stored yet

	// Open read-only; a missing namespace/key or out-of-range value falls back to the default
	if (nvs_open(LORA_CFG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
		uint8_t stored;
		if (nvs_get_u8(h, LORA_CFG_NVS_REGION, &stored) == ESP_OK &&
		    stored < LORA_REGION_COUNT) {
			region = (lora_region_t)stored;
		}
		nvs_close(h);
	}

#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "Loaded LoRa region=%s", region == LORA_REGION_EU ? "EU" : "US");
#endif

	return region;
}

esp_err_t lora_pcp_save_region_nvs(lora_region_t region)
{
	// Clamp to a valid region so a bad value can never reach the radio
	if (region >= LORA_REGION_COUNT) {
		region = LORA_REGION_DEFAULT;
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(LORA_CFG_NVS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_region_nvs: NVS open failed: %s", esp_err_to_name(err));
		return err;
	}

	err = nvs_set_u8(h, LORA_CFG_NVS_REGION, (uint8_t)region);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_region_nvs: NVS write failed: %s", esp_err_to_name(err));
	}

	nvs_close(h);
	return err;
}

static void save_msg_id_nvs(void)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(PCP_NVS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "save_msg_id_nvs: NVS open failed: %s", esp_err_to_name(err));
		return;
	}

	err = nvs_set_u32(h, PCP_NVS_RX_ID, last_msg_id);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "save_msg_id_nvs: NVS set failed: %s", esp_err_to_name(err));
	}

	err = nvs_commit(h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "save_msg_id_nvs: NVS commit failed: %s", esp_err_to_name(err));
	}

	nvs_close(h);
}

void lora_pcp_init(void)
{
	xPcpMutex = xSemaphoreCreateMutex();
	configASSERT(xPcpMutex);

	// Initialize PSA crypto subsystem
	psa_status_t status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
	}

	// Load last accepted msg_id from NVS to prevent replay across reboots
	nvs_handle_t h;
	esp_err_t err = nvs_open(PCP_NVS_NS, NVS_READONLY, &h);
	if (err == ESP_OK) {
		err = nvs_get_u32(h, PCP_NVS_RX_ID, &last_msg_id);
		if (err == ESP_ERR_NVS_NOT_FOUND) {
#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "lora_pcp_init: No persisted last_msg_id, starting at 0");
#endif
			last_msg_id = 0;
		} else if (err != ESP_OK) {
			ESP_LOGE(TAG, "lora_pcp_init: NVS get failed: %s", esp_err_to_name(err));
		}
		nvs_close(h);
	} else if (err != ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGE(TAG, "lora_pcp_init: NVS open failed: %s", esp_err_to_name(err));
	}

#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "lora_pcp_init: Loaded last_msg_id=%" PRIu32, last_msg_id);
#endif
}

void lora_pcp_set_key(const uint8_t *key)
{
	static uint8_t enc_key_cmp[LORA_PCP_ENC_KEY_LEN] = {0}; // For checking if key changes

	xSemaphoreTake(xPcpMutex, portMAX_DELAY);

	// Import into a temporary ID first - preserve old key on failure
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, PCP_CCM_ALG);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, LORA_PCP_ENC_KEY_LEN * 8);

	psa_key_id_t new_key_id = 0;
	psa_status_t status = psa_import_key(&attr, key, LORA_PCP_ENC_KEY_LEN, &new_key_id);
	if (status != PSA_SUCCESS) {
		ESP_LOGE(TAG, "psa_import_key failed: %d, keeping old key", (int)status);
		xSemaphoreGive(xPcpMutex);
		return;
	}

	// Import succeeded - commit all atomically
	if (pcp_key_id != 0) {
		status = psa_destroy_key(pcp_key_id);
		if (status != PSA_SUCCESS) {
			ESP_LOGE(TAG, "psa_destroy_key failed: %d (key slot leak)", (int)status);
		}
	}
	pcp_key_id = new_key_id;

	// Reset replay counter only when key actually changes
	// key_initialized prevents first load at boot from resetting counter
	if (key_initialized && memcmp(enc_key_cmp, key, LORA_PCP_ENC_KEY_LEN) != 0) {
		last_msg_id = 0;
		save_msg_id_nvs();

#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Key rotated, replay counter reset");
#endif
	}

	memcpy(enc_key_cmp, key, LORA_PCP_ENC_KEY_LEN);
	key_initialized = true;

	xSemaphoreGive(xPcpMutex);
}

void lora_pcp_process_received_message(uint8_t *message, size_t message_len)
{
	// Minimum valid packet: nonce + ACK ciphertext + MIC
	size_t min_len = LORA_PCP_NONCE_LENGTH + LORA_PCP_ACK_CIPHERTEXT_LEN + LORA_PCP_MIC_LENGTH;
	if (message_len < min_len) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Received message too short!");
		#endif

		return;
	}

	xSemaphoreTake(xPcpMutex, portMAX_DELAY);

	if (pcp_key_id == 0) {
		ESP_LOGE(TAG, "No key set, ignoring message");
		xSemaphoreGive(xPcpMutex);
		return;
	}

	// Extract nonce (first 13 bytes) and ciphertext+tag (rest)
	const uint8_t *nonce      = message;
	const uint8_t *ct_and_tag = message + LORA_PCP_NONCE_LENGTH;
	size_t ct_and_tag_len     = message_len - LORA_PCP_NONCE_LENGTH;

	// Decrypt and authenticate (PSA expects ciphertext || tag as one buffer)
	uint8_t plaintext[LORA_PCP_CIPHERTEXT_LENGTH] = {0};
	size_t plaintext_len = 0;

	psa_status_t status = psa_aead_decrypt(
			pcp_key_id, PCP_CCM_ALG,
			nonce, LORA_PCP_NONCE_LENGTH,
			NULL, 0,
			ct_and_tag, ct_and_tag_len,
			plaintext, sizeof(plaintext),
			&plaintext_len);

	if (status != PSA_SUCCESS) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "CCM auth failed: %d", (int)status);
		#endif
		xSemaphoreGive(xPcpMutex);
		return;
	}

	#ifdef POLYPLUG_DEBUG
	ESP_LOG_BUFFER_HEX("LORA_PCP: Decrypted", plaintext, plaintext_len);
	#endif

	uint8_t msg_type = plaintext[0];

	if (msg_type != LORA_PCP_COMMAND || plaintext_len != LORA_PCP_CMD_CIPHERTEXT_LEN) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Bad type (0x%02X) or length (%u)", msg_type, (unsigned)plaintext_len);
		#endif
		xSemaphoreGive(xPcpMutex);
		return;
	}

	// Parse binary command
	lora_pcp_cmd_msg_t cmd_msg;
	memcpy(&cmd_msg, plaintext, sizeof(cmd_msg));
	cmd_msg.instr[sizeof(cmd_msg.instr) - 1] = '\0'; // Ensure null termination

	#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "Parsed msg_id=%" PRIu32 ", cmd=%d, instr=\"%s\"", cmd_msg.msg_id, cmd_msg.index, cmd_msg.instr);
	ESP_LOGI(TAG, "Have last_msg_id=%" PRIu32, last_msg_id);
	#endif

	// Reject replay: msg_id must be strictly greater than last accepted
	if (cmd_msg.msg_id > last_msg_id) {
		relay_tx.index = cmd_msg.index;

		// Simple toggle
		if (cmd_msg.index == 0) {
			xQueueOverwrite(xRelayToggleQueue, &relay_tx);

			// Send receipt
			valid_data_rec = true;
		}
		// Loop with specific times
		else if (cmd_msg.index == 1) {
			char on_arg[LOOP_LEN], off_arg[LOOP_LEN];

			if (sscanf(cmd_msg.instr, "on %3s off %3s", on_arg, off_arg) == 2) {
				memset(relay_tx.loop_on,  0, LOOP_LEN);
				memset(relay_tx.loop_off, 0, LOOP_LEN);
				strncpy(relay_tx.loop_on,  on_arg,  LOOP_LEN - 1);
				strncpy(relay_tx.loop_off, off_arg, LOOP_LEN - 1);

				xQueueOverwrite(xRelayToggleQueue, &relay_tx);

				// Send receipt
				valid_data_rec = true;
			}
			else {
				ESP_LOGE(TAG, "Bad loop args: \"%s\"", cmd_msg.instr);
			}
		}
		// Plan mode
		else if (cmd_msg.index == 2) {
			char days_buf[PLAN_DAYS_LEN] = {0};
			char on_buf[PLAN_TIME_LEN] = {0};
			char off_buf[PLAN_TIME_LEN] = {0};

			// If parsed successfully
			if (sscanf(cmd_msg.instr, "d %7[^ ] o %6[^ ] f %6[^ ]", days_buf, on_buf, off_buf) == 3) {
				// Start fresh
				memset(relay_tx.plan_days, 0, sizeof(relay_tx.plan_days));
				memset(relay_tx.plan_on, 0, sizeof(relay_tx.plan_on));
				memset(relay_tx.plan_off, 0, sizeof(relay_tx.plan_off));

				// Save and send
				strncpy(relay_tx.plan_days, days_buf, sizeof(relay_tx.plan_days) - 1);
				strncpy(relay_tx.plan_on, on_buf, sizeof(relay_tx.plan_on) - 1);
				strncpy(relay_tx.plan_off, off_buf, sizeof(relay_tx.plan_off) - 1);
				xQueueOverwrite(xRelayToggleQueue, &relay_tx);

				#ifdef POLYPLUG_DEBUG
				ESP_LOGI(TAG, "Plan parsed: days='%s' on='%s' off='%s'",
						relay_tx.plan_days, relay_tx.plan_on, relay_tx.plan_off);
				#endif

				// Send receipt
				valid_data_rec = true;
			}
			else {
				ESP_LOGE(TAG, "Bad plan args: '%s'", cmd_msg.instr);
			}
		}
		// Away mode
		else if (cmd_msg.index == 3) {
			int away_min, away_max;

			// Try to scan out the instruction
			if (sscanf(cmd_msg.instr, "away %d-%dm", &away_min, &away_max) == 2) {
				#ifdef POLYPLUG_DEBUG
				ESP_LOGI(TAG, "Parsed away range: %d to %d minutes", away_min, away_max);
				#endif

				// Save and send
				relay_tx.away_min = away_min;
				relay_tx.away_max = away_max;
				xQueueOverwrite(xRelayToggleQueue, &relay_tx);

				// Send receipt
				valid_data_rec = true;
			}
			else {
				ESP_LOGE(TAG, "Bad away args: \"%s\"", cmd_msg.instr);
			}
		}
		// GPIO mode
		else if (cmd_msg.index == 4) {
			int gpio_cmd;

			// Try to scan out the instruction
			if (sscanf(cmd_msg.instr, "gpio %d", &gpio_cmd) == 1) {
				#ifdef POLYPLUG_DEBUG
				ESP_LOGI(TAG, "Parsed GPIO command: %d", gpio_cmd);
				#endif

				// Save and send
				relay_tx.gpio_cmd = gpio_cmd;
				xQueueOverwrite(xRelayToggleQueue, &relay_tx);

				// Send receipt
				valid_data_rec = true;
			}
			else {
				ESP_LOGE(TAG, "Bad GPIO args: \"%s\"", cmd_msg.instr);
			}
		}
		else {
			#ifdef POLYPLUG_DEBUG
			ESP_LOGW(TAG, "Unknown command %d", cmd_msg.index);
			#endif
		}

		// Only commit counter after successful command acceptance
		if (valid_data_rec) {
			last_msg_id = cmd_msg.msg_id;
			save_msg_id_nvs();
		} else {
			ESP_LOGE(TAG, "valid_data_rec flag not set on accepted command: Bad args?");
		}
	}
	else if (cmd_msg.msg_id == last_msg_id) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Duplicate msg, re-ACK only");
		#endif

		// Re-ACK only — command was already executed on first accept.
		valid_data_rec = true;
	}
	else {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Replay detected: Got msg_id=%" PRIu32 ", Have last_msg_id=%" PRIu32, cmd_msg.msg_id, last_msg_id);
		#endif
	}

	xSemaphoreGive(xPcpMutex);
}

bool lora_pcp_send_receipt(void)
{
	xSemaphoreTake(xPcpMutex, portMAX_DELAY);

	if (!valid_data_rec) {
		xSemaphoreGive(xPcpMutex);
		return false; // No TX started - caller must re-arm RX
	}

	// Build PCP ACK
	lora_pcp_ack_msg_t ack = {
		.type = LORA_PCP_ACK,
		.msg_id = last_msg_id,
	};

	// Reset valid marker
	valid_data_rec = false;

	#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "SENDING ACK msg_id=%" PRIu32, ack.msg_id);
	#endif

	// Hold mutex during encrypt to protect pcp_key_id from concurrent set_key
	bool ok = lora_pcp_encrypt_and_transmit((uint8_t *)&ack, sizeof(ack));

	xSemaphoreGive(xPcpMutex);
	return ok;
}

bool lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len)
{
	if (plaintext_len > LORA_PCP_CIPHERTEXT_LENGTH) {
		ESP_LOGE(TAG, "LoRa plaintext too long (%u bytes, max %u)",
			(unsigned)plaintext_len,
			(unsigned)LORA_PCP_CIPHERTEXT_LENGTH);
		return false;
	}

	if (pcp_key_id == 0) {
		ESP_LOGE(TAG, "No key set, cannot transmit");
		return false;
	}

	uint8_t nonce[LORA_PCP_NONCE_LENGTH];
	esp_fill_random(nonce, sizeof(nonce));

	// PSA outputs ciphertext || tag concatenated
	uint8_t ct_and_tag[LORA_PCP_CIPHERTEXT_LENGTH + LORA_PCP_MIC_LENGTH];
	size_t ct_and_tag_len = 0;

	psa_status_t status = psa_aead_encrypt(
			pcp_key_id, PCP_CCM_ALG,
			nonce, LORA_PCP_NONCE_LENGTH,
			NULL, 0,
			plaintext, plaintext_len,
			ct_and_tag, sizeof(ct_and_tag),
			&ct_and_tag_len);

	if (status != PSA_SUCCESS) {
		ESP_LOGE(TAG, "CCM encrypt failed: %d", (int)status);
		return false;
	}

	// Assemble wire message: [nonce | ciphertext | MIC]
	uint8_t message[LORA_PCP_PAYLOAD_LENGTH];
	size_t msg_len = LORA_PCP_NONCE_LENGTH + ct_and_tag_len;

	memcpy(message, nonce, LORA_PCP_NONCE_LENGTH);
	memcpy(message + LORA_PCP_NONCE_LENGTH, ct_and_tag, ct_and_tag_len);

	return lora_radio_tx(message, msg_len);
}
