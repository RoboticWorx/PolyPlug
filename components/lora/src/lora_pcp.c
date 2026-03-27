#include "polyplug_macros.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "portmacro.h"
#include "lora_pcp.h"
#include "lora_radio.h"
#include "lora_task.h"
#include "espnow_task.h"
#include "gpio_funcs.h"
#include "gpio_task.h"

static const char *TAG = "LORA_PCP";

static relay_t relay_tx;

static uint8_t encryption_key[16] = {0};

static bool valid_data_rec = false;
static uint32_t last_rx_id = 0;

void lora_pcp_set_key(const uint8_t *key) {
	memcpy(encryption_key, key, ENC_KEY_LEN);
}

static void generate_random_iv(uint8_t *iv, size_t length) {
	for (size_t i = 0; i < length; i++) {
		iv[i] = (uint8_t)(esp_random() % (255 + 1)); // Generate number 0 - 255
	}
}

void lora_pcp_process_received_message(uint8_t *message, size_t message_len)
{
	// Minimum: 16 bytes IV + 16 bytes ciphertext (one AES block)
	if (message_len < LORA_PCP_IV_LENGTH + LORA_PCP_ACK_CIPHERTEXT_LEN) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Received message too short!");
		#endif

		return;
	}

	size_t ct_len = message_len - LORA_PCP_IV_LENGTH;

	// Ciphertext must be a multiple of 16 (AES block size) and within max
	if ((ct_len % 16) != 0 || ct_len > LORA_PCP_CIPHERTEXT_LENGTH) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Invalid ciphertext length: %u bytes\n", (unsigned)ct_len);
		#endif

		return;
	}

	uint8_t iv[LORA_PCP_IV_LENGTH]; // To hold IV
	memcpy(iv, message, LORA_PCP_IV_LENGTH); // Extract the IV (first 16 bytes)

	uint8_t ciphertext[LORA_PCP_CIPHERTEXT_LENGTH] = {0}; // To hold cyphertext
	memcpy(ciphertext, LORA_PCP_IV_LENGTH + message, ct_len); // Extract the ciphertext

	// Initialize the AES context with the key and received IV
	struct AES_ctx ctx;
	AES_init_ctx_iv(&ctx, encryption_key, iv);

	// Decrypt ciphertext
	AES_CBC_decrypt_buffer(&ctx, ciphertext, ct_len);

	#ifdef POLYPLUG_DEBUG
	ESP_LOG_BUFFER_HEX("LORA_PCP: Decrypted", ciphertext, ct_len);
	#endif

	// Validate magic bytes
	uint16_t magic;
	memcpy(&magic, ciphertext, sizeof(magic));
	if (magic != LORA_PCP_MAGIC) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Bad magic: 0x%04X", magic);
		#endif
		return;
	}

	uint8_t msg_type = ciphertext[2];

	if (msg_type != LORA_PCP_COMMAND || ct_len != LORA_PCP_CMD_CIPHERTEXT_LEN) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Bad type (0x%02X) or length (%u)", msg_type, (unsigned)ct_len);
		#endif
		return;
	}

	// Parse binary command
	lora_pcp_cmd_msg_t cmd_msg;
	memcpy(&cmd_msg, ciphertext, sizeof(cmd_msg));
	cmd_msg.instr[sizeof(cmd_msg.instr) - 1] = '\0'; // Ensure null termination

	#ifdef POLYPLUG_DEBUG
	ESP_LOGI(TAG, "Parsed msg_id=%" PRIu32 ", cmd=%d, instr=\"%s\"", cmd_msg.msg_id, cmd_msg.index, cmd_msg.instr);
	#endif

	// Check for unique ID
	if (cmd_msg.msg_id != last_rx_id) {
		last_rx_id = cmd_msg.msg_id;

		relay_tx.index = cmd_msg.index;

		// Simple toggle
		if (cmd_msg.index == 0) {
			xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);

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

				xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);

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
				xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);

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
				xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);

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
				xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);

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
	}
	else {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Duplicate msg, re-ACK only");
		#endif

		// Still send receipt, but don't re-execute.
		// This is for case when cmd was received but receipt was lost.
		// This prevents the sender from thinking it wasn't received and re-sending to un-do what the first send did.

		// Send receipt
		valid_data_rec = true;
	}
}

void lora_pcp_send_receipt(void)
{
	if (valid_data_rec) {
		// Build binary ACK
		lora_pcp_ack_msg_t ack = {
			.magic = LORA_PCP_MAGIC,
			.type = LORA_PCP_ACK,
			.msg_id = last_rx_id,
		};

		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "SENDING ACK msg_id=%" PRIu32, last_rx_id);
		#endif

		// Encrypt and send over
		lora_pcp_encrypt_and_transmit((uint8_t *)&ack, sizeof(ack));

		// Reset valid marker
		valid_data_rec = false;
	}
	else {
		lora_radio_set_rx_mode(LORA_PCP_PAYLOAD_LENGTH); // Reset RX
	}
}

void lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len)
{
	// Round up to next AES block size (multiple of 16)
	size_t padded_len = ((plaintext_len + 15) / 16) * 16;

	// Check length
	if (padded_len > LORA_PCP_CIPHERTEXT_LENGTH) {
		ESP_LOGE(TAG, "LoRa plaintext too long (%u bytes, padded %u), max is %u",
			(unsigned)plaintext_len,
			(unsigned)padded_len,
			(unsigned)LORA_PCP_CIPHERTEXT_LENGTH);
		return;
	}

	uint8_t buffer[LORA_PCP_CIPHERTEXT_LENGTH] = {0}; // Zero-padded for AES block alignment
	memcpy(buffer, plaintext, plaintext_len); // Copy only the actual data

	uint8_t iv[LORA_PCP_IV_LENGTH]; // To hold IV
	generate_random_iv(iv, sizeof(iv)); // Generate random IV into iv[16]

	struct AES_ctx ctx;
	AES_init_ctx_iv(&ctx, encryption_key, iv); // Initialize AES context with key and IV

	AES_CBC_encrypt_buffer(&ctx, buffer, padded_len); // Encrypt padded data

	uint8_t message[LORA_PCP_IV_LENGTH + LORA_PCP_CIPHERTEXT_LENGTH]; // Buffer to send
	memcpy(message, iv, LORA_PCP_IV_LENGTH); // First 16 bytes is IV
	memcpy(LORA_PCP_IV_LENGTH + message, buffer, padded_len); // Next is the cyphertext

	lora_radio_tx(message, LORA_PCP_IV_LENGTH + padded_len); // Send the data
}
