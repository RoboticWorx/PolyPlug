//    Behold, PolyCast5's very own LoRa protocol: the Poly Cipher Protocol (PCP)!
// ┌──────────────────────────────────────────────────────────────────────────────┐
// │         Poly Cipher Protocol (PCP) — AES-128-CBC, Explicit Header            │
// ├──────────────────────────────────────────────────────────────────────────────┤
// │                      Command Packet (64 bytes on air)                        │
// ├────────────────┬─────────────────────────────────────────────────────────────┤
// │   IV (16B)     │               AES-CBC Ciphertext (48B)                      │
// │   Random IV    ├────────┬──────┬──────────┬───────┬────────────┬─────────────┤
// │                │ Magic  │ Type │  Msg ID  │ Index │   Instr    │  Zero Pad   │
// │                │   2B   │  1B  │    4B    │  1B   │    32B     │     8B      │
// │                │ 0x5043 │ 0x01 │  uint32  │ uint8 │  char[32]  │  (AES pad)  │
// ├────────────────┴────────┴──────┴──────────┴───────┴────────────┴─────────────┤
// │                        ACK Packet (32 bytes on air)                          │
// ├────────────────┬─────────────────────────────────────────────────────────────┤
// │   IV (16B)     │               AES-CBC Ciphertext (16B)                      │
// │   Random IV    ├────────┬──────┬──────────┬──────────────────────────────────┤
// │                │ Magic  │ Type │  Msg ID  │           Zero Pad               │
// │                │   2B   │  1B  │    4B    │              9B                  │
// │                │ 0x5043 │ 0x02 │  uint32  │           (AES pad)              │
// └────────────────┴────────┴──────┴──────────┴──────────────────────────────────┘

#ifndef LORA_PCP_H
#define LORA_PCP_H

#include <stddef.h>
#include <stdint.h>

#include "aes.h"

#define LORA_PCP_IV_LENGTH 16
#define LORA_PCP_INSTR_MAX_LEN 32
#define LORA_PCP_ENC_KEY_LEN 16

// Binary wire protocol
#define LORA_PCP_MAGIC   0x5043 // "PC" - brute-force guard
#define LORA_PCP_COMMAND 0x01
#define LORA_PCP_ACK     0x02

// Command message (40 bytes, padded to 48 for AES-CBC)
typedef struct __attribute__((packed)) {
	uint16_t magic;
	uint8_t  type;
	uint32_t msg_id;
	uint8_t  index;
	char     instr[LORA_PCP_INSTR_MAX_LEN];
} lora_pcp_cmd_msg_t;

// ACK message (7 bytes, padded to 16 for AES-CBC)
typedef struct __attribute__((packed)) {
	uint16_t magic;
	uint8_t  type;
	uint32_t msg_id;
} lora_pcp_ack_msg_t;

// Ciphertext sizes per message type (next multiple of 16)
#define LORA_PCP_CMD_CIPHERTEXT_LEN 48
#define LORA_PCP_ACK_CIPHERTEXT_LEN 16

// Max ciphertext size (command message)
#define LORA_PCP_CIPHERTEXT_LENGTH (LORA_PCP_CMD_CIPHERTEXT_LEN)
#define LORA_PCP_PAYLOAD_LENGTH    (LORA_PCP_CIPHERTEXT_LENGTH + LORA_PCP_IV_LENGTH)

/**
 * @brief Loads persisted replay counter from NVS and creates a mutex
 * 		  for synchronizing access to replay counter and valid data marker.
 */
void lora_pcp_init(void);

/**
 * @brief Sets the encryption key for PCP and resets the replay counter
 *
 * @param [in] key The encryption key to set
 */
void lora_pcp_set_key(const uint8_t *key);

/**
 * @brief Process a received PCP message (decrypt, validate, dispatch)
 *
 * @param [in] message The message received
 * @param [in] message_len Length of the message received
 */
void lora_pcp_process_received_message(uint8_t *message, size_t message_len);

/**
 * @brief Sends an ACK receipt if last received message was valid
 */
void lora_pcp_send_receipt(void);

/**
 * @brief Encrypts plaintext and transmits over LoRa
 *
 * @param [in] plaintext The data to encrypt and transmit
 * @param [in] plaintext_len Length of the data to encrypt and transmit
 */
void lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len);

#endif // LORA_PCP_H
