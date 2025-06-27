#include "polyplug_macros.h"

#include <string.h>

#include "esp_log.h"

#include "sx126x_hal.h"
#include "lora_funcs.h"
#include "lora_task.h"
#include "espnow_task.h"
#include "gpio_funcs.h"
#include "gpio_task.h"

static const char *TAG = "LORA_FUNCS";

static relay_t relay_tx;

static uint8_t encryption_key[16] = {0};

static bool valid_data_rec = false;
static uint32_t last_rx_id = 0;

void lora_set_key(const uint8_t *key) {
    memcpy(encryption_key, key, ENC_KEY_LEN);
}

static void generate_random_iv(uint8_t *iv, size_t length) {
	for (size_t i = 0; i < length; i++) {
		iv[i] = (uint8_t)(rand() % (255 + 1)); // Generate number 0 - 255
	}
}

void lora_set_rx_mode(void) // Call once to set RX mode and receive on EXTI8
{
	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1)); // Poll for SX1262 to be ready
	}

	// Enter continuous RX mode
	sx126x_status_t status = sx126x_set_rx(NULL, SX126X_RX_SINGLE_MODE);

	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to enter continuous RX mode\n");
		return;
	}
}

void lora_tx(uint8_t tx_data[], uint8_t data_len) {

	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1)); // Poll for SX1262 to be ready
	}

	sx126x_status_t status = sx126x_write_buffer(NULL, 0, tx_data, data_len);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to write to buffer\n");
	}

	// Start transmission
	status = sx126x_set_tx(NULL, SX126X_MAX_TIMEOUT_IN_MS);

	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to start transmission\n");
	}
}

void lora_process_received_message(uint8_t *message, size_t message_len) {
	// Verify that the message length is at least 16 bytes (for IV) + 16 bytes
	// (minimum ciphertext)
	if (message_len < 32) {
		#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Received message too short!");
		#endif
		
		return;
	}

	// The expected message length is 80 bytes (16 IV + 64 cyphertext)
	if (message_len != CYPHERTEXT_LENGTH + 16) {
		#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Unexpected message length: %u bytes\n", (unsigned)message_len);
		#endif
		
		return;
	}

	uint8_t iv[IV_LENGTH];			// To hold IV
	memcpy(iv, message, IV_LENGTH); // Extract the IV (first 16 bytes)

	uint8_t ciphertext[CYPHERTEXT_LENGTH]; // To hold cyphertext
	memcpy(ciphertext, message + IV_LENGTH,
		   CYPHERTEXT_LENGTH); // Extract the ciphertext (remaining 64 bytes)

	// Initialize the AES context with the key and received IV.
	struct AES_ctx ctx;
	AES_init_ctx_iv(&ctx, encryption_key, iv);

	// Decrypt "ciphertext"
	AES_CBC_decrypt_buffer(&ctx, ciphertext, sizeof(ciphertext));

	ciphertext[sizeof(ciphertext) - 1] = '\0'; // Ensure null termination

	// "cyphertext" is now decrypted - print
	#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Decrypted text: %s\n", ciphertext);
	#endif
		
	// Processing logic:
	uint32_t rx_id;
	int cmd;
	char instr_buf[64];
	
	// Try to parse ID, cmd, and instr (up to 63 chars) in one go:
	int got = sscanf((char*)ciphertext, "PolyCast_Command_Value:%" SCNu32 ":%d:%63[^\n]", &rx_id, &cmd, instr_buf);
	
	if (got == 3) {
		#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Parsed rx_id=%" PRIu32 ", cmd=%d, instr=\"%s\"", rx_id, cmd, instr_buf);
		#endif
	    
	    // Check for unique ID
	    if (rx_id != last_rx_id) {
	        last_rx_id = rx_id;
	        
	        relay_tx.index = cmd;
	        
	        // Simple toggle
	        if (cmd == 0) {
				xQueueSend(xRelayToggleQueue, &relay_tx, 1);
				
				// Send receipt
				valid_data_rec = true;
	        }
	        // Loop with specific times
	        else if (cmd == 1) {
				char on_arg[LOOP_LEN], off_arg[LOOP_LEN];
				
		        if (sscanf(instr_buf, "on %3s off %3s", on_arg, off_arg) == 2) {
					memset(relay_tx.loop_on,  0, LOOP_LEN);
					memset(relay_tx.loop_off, 0, LOOP_LEN);
					strncpy(relay_tx.loop_on,  on_arg,  LOOP_LEN - 1);
					strncpy(relay_tx.loop_off, off_arg, LOOP_LEN - 1);

		            xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);
		            
		            // Send receipt
		            valid_data_rec = true;
		        }
		        else {
		            ESP_LOGE(TAG, "Bad loop args: \"%s\"", instr_buf);
		        }
	        }
	        // Away mode
	        else if (cmd == 3) {
				int away_min, away_max;
				
		        if (sscanf(instr_buf, "away %d-%dm", &away_min, &away_max) == 2) {
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
		            ESP_LOGE(TAG, "Bad away args: \"%s\"", instr_buf);
		        }
			}
	        else {
				#ifdef POLYPLUG_DEBUG
					ESP_LOGW(TAG, "Unknown command %d", cmd);
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
	else {
		#ifdef POLYPLUG_DEBUG
			ESP_LOGE(TAG, "Failed to parse incoming \"%s\"", ciphertext);
		#endif
	}
}

void lora_send_receipt()
{
	if (valid_data_rec) {
		// Hold data to send
		char payload[CYPHERTEXT_LENGTH] = {0};
		
		// Format command into string
		snprintf(payload, sizeof(payload), "PolyCast_Command_Value_Received:%" PRIu32, last_rx_id);
	
		#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "SENDING: %s", payload);
		#endif
	
		// Encrypt and send over
		lora_encrypt_and_transmit((uint8_t *)payload);
		
		// Reset valid marker
		valid_data_rec = false;
	}
	else {
		lora_set_rx_mode(); // Reset RX
	}
}

void lora_encrypt_and_transmit(uint8_t plaintext[])
{
	uint8_t buffer[CYPHERTEXT_LENGTH]; // Padded to 64 bytes (must be multiple
									   // of 16)
	memcpy(buffer, plaintext, sizeof(buffer)); // Copy the 64 bytes into buffer

	uint8_t iv[IV_LENGTH];				// To hold IV
	generate_random_iv(iv, sizeof(iv)); // Generate random IV into iv[16]

	/*ESP_LOGI(TAG, "Generated IV: ");
	for (int i = 0; i < 16; i++) {
		ESP_LOGI(TAG, "%02X ", iv[i]);
	}
	ESP_LOGI(TAG, "\n");*/

	struct AES_ctx ctx;
	AES_init_ctx_iv(&ctx, encryption_key, iv); // Initialize AES context with key and IV

	AES_CBC_encrypt_buffer(&ctx, buffer, sizeof(buffer)); // Encrypt buffer

	uint8_t message[IV_LENGTH + CYPHERTEXT_LENGTH]; // New buffer to send
	memcpy(message, iv, IV_LENGTH); // First 16 bytes are IV
	memcpy(message + IV_LENGTH, buffer, CYPHERTEXT_LENGTH); // Next are the cyphertext

	/*ESP_LOGI(TAG, "Message to send (hex): ");
	for (int i = 0; i < (int)sizeof(message); i++)
	{
		ESP_LOGI(TAG, "%02X ", message[i]);
	}
	ESP_LOGI(TAG, "\n");*/

	lora_tx(message, sizeof(message)); // Send the data
}

