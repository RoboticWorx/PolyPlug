#ifndef LORA_FUNCS_H
#define LORA_FUNCS_H

#include "aes.h"
#include "sx126x.h"
#include "sx126x_hal.h"

#define IV_LENGTH 16
#define INSTR_MAX_LEN 32

// Binary wire protocol
#define LORA_MSG_MAGIC   0x5043 // "PC" - brute-force guard
#define LORA_MSG_COMMAND 0x01
#define LORA_MSG_ACK     0x02

// Command message (40 bytes, padded to 48 for AES-CBC)
typedef struct __attribute__((packed)) {
	uint16_t magic;
	uint8_t  type;
	uint32_t msg_id;
	uint8_t  index;
	char     instr[INSTR_MAX_LEN];
} lora_cmd_msg_t;

// ACK message (7 bytes, padded to 16 for AES-CBC)
typedef struct __attribute__((packed)) {
	uint16_t magic;
	uint8_t  type;
	uint32_t msg_id;
} lora_ack_msg_t;

// Ciphertext sizes per message type (next multiple of 16)
#define CMD_CIPHERTEXT_LEN 48
#define ACK_CIPHERTEXT_LEN 16

// Max ciphertext size (command message)
#define CYPHERTEXT_LENGTH (CMD_CIPHERTEXT_LEN)
#define PAYLOAD_LENGTH    (CYPHERTEXT_LENGTH + IV_LENGTH)


/** 
 * @brief Sets the encryption key for LoRa communication
 *
 * @param [in] key The encryption key to set
 */
void lora_set_key(const uint8_t *key);

/** 
 * @brief Sets SX1262 radio in receive mode
 */
void lora_set_rx_mode(void);

/** 
 * @brief Process the received LoRa message (decrypt, etc.)
 *
 * @param [in] message The message received
 * @param [in] message_len Length of the message received
 */
void lora_process_received_message(uint8_t *message, size_t message_len);

/** 
 * @brief Sends a receipt to confirm with the sender if data was valid
 */
void lora_send_receipt();

/** 
 * @brief Transmit data over LoRa
 *
 * @param [in] tx_data The data to transmit
 * @param [in] data_len Length of the data to transmit
 */
void lora_tx(uint8_t tx_data[], uint8_t data_len);

/** 
 * @brief Encrypts plaintext and calls lora_tx() to transmit
 *
 * @param [in] plaintext The data to encrypt and transmit
 * @param [in] plaintext_len Length of the data to encrypt and transmit
 */
void lora_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len);

#endif // LORA_FUNCS_H