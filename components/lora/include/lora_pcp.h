// Behold, PolyCast5's very own LoRa protocol: the Poly Cipher Protocol (PCP)!
// ┌────────────────────────────────────────────────────────────────────────┐
// │       Poly Cipher Protocol (PCP) — AES-128-CCM, Explicit Header        │
// ├────────────────────────────────────────────────────────────────────────┤
// │                     Command Packet (55 bytes on air)                   │
// ├─────────────┬─────────────────────────────────────────────┬────────────┤
// │ Nonce (13B) │           AES-CCM Ciphertext (38B)          │  MIC (4B)  │
// │  Random     ├──────┬──────────┬───────┬───────────────────┤  Auth Tag  │
// │             │ Type │  Msg ID  │ Index │       Instr       │            │
// │             │  1B  │    4B    │  1B   │        32B        │            │
// │             │ 0x01 │  uint32  │ uint8 │      char[32]     │            │
// ├─────────────┴──────┴──────────┴───────┴───────────────────┴────────────┤
// │                      ACK Packet (22 bytes on air)                      │
// ├─────────────┬─────────────────────────────────────────────┬────────────┤
// │ Nonce (13B) │           AES-CCM Ciphertext (5B)           │  MIC (4B)  │
// │  Random     ├──────┬──────────────────────────────────────┤  Auth Tag  │
// │             │ Type │                Msg ID                │            │
// │             │  1B  │                  4B                  │            │
// │             │ 0x02 │                uint32                │            │
// └─────────────┴──────┴──────────────────────────────────────┴────────────┘

#ifndef LORA_PCP_H
#define LORA_PCP_H

#include <stddef.h>
#include <stdint.h>

#define LORA_PCP_NONCE_LENGTH  13
#define LORA_PCP_MIC_LENGTH    4
#define LORA_PCP_ENC_KEY_LEN   16
#define LORA_PCP_INSTR_MAX_LEN 32
#define LORA_PCP_COMMAND 0x01
#define LORA_PCP_ACK     0x02

// Command message (38 bytes plaintext, 38 bytes ciphertext - no padding with CCM)
typedef struct __attribute__((packed)) {
	uint8_t  type;
	uint32_t msg_id;
	uint8_t  index;
	char     instr[LORA_PCP_INSTR_MAX_LEN];
} lora_pcp_cmd_msg_t;

// ACK message (5 bytes plaintext, 5 bytes ciphertext — no padding with CCM)
typedef struct __attribute__((packed)) {
	uint8_t  type;
	uint32_t msg_id;
} lora_pcp_ack_msg_t;

// Ciphertext sizes per message type (plaintext size, no block padding)
#define LORA_PCP_CMD_CIPHERTEXT_LEN (sizeof(lora_pcp_cmd_msg_t)) // 38
#define LORA_PCP_ACK_CIPHERTEXT_LEN (sizeof(lora_pcp_ack_msg_t)) // 5

// Max ciphertext size (command message)
#define LORA_PCP_CIPHERTEXT_LENGTH (LORA_PCP_CMD_CIPHERTEXT_LEN)

// Max on-air payload: nonce + largest ciphertext + MIC
#define LORA_PCP_PAYLOAD_LENGTH (LORA_PCP_NONCE_LENGTH + LORA_PCP_CIPHERTEXT_LENGTH + LORA_PCP_MIC_LENGTH)

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
 * @brief Process a received PCP message (CCM auth-decrypt, validate, dispatch)
 *
 * @param [in] message The raw message received (Nonce + ciphertext + MIC)
 * @param [in] message_len Length of the message received
 */
void lora_pcp_process_received_message(uint8_t *message, size_t message_len);

/**
 * @brief Sends an ACK receipt if last received message was valid
 */
void lora_pcp_send_receipt(void);

/**
 * @brief Encrypts plaintext with AES-128-CCM and transmits over LoRa
 *
 * @param [in] plaintext The data to encrypt and transmit
 * @param [in] plaintext_len Length of the data to encrypt and transmit
 */
void lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len);

#endif // LORA_PCP_H
