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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LORA_PCP_NONCE_LENGTH  13
#define LORA_PCP_MIC_LENGTH    4
#define LORA_PCP_ENC_KEY_LEN   16
#define LORA_PCP_INSTR_MAX_LEN 32
#define LORA_PCP_COMMAND 0x01
#define LORA_PCP_ACK     0x02

// User-selectable LoRa spreading factor range (must match the PolyCast5 remote)
// The numeric SF value equals the SX126X_LORA_SFx enum value (e.g. SF7 == 0x07)
#define LORA_PCP_SF_MIN     7  // SX126X_LORA_SF7
#define LORA_PCP_SF_MAX     12 // SX126X_LORA_SF12
#define LORA_PCP_SF_DEFAULT 7  // SX126X_LORA_SF7 (matches the remote's default)

// LoRa region: the RF band synced from the PolyCast5 remote
// The PCP carrier and SX1262 image-calibration band both follow this
typedef enum {
	LORA_REGION_US = 0, // US 902-928 MHz ISM band
	LORA_REGION_EU = 1, // EU 863-870 MHz ISM band (ETSI EN 300 220)
	LORA_REGION_COUNT   // Not a region; count for range clamping
} lora_region_t;

#define LORA_REGION_DEFAULT LORA_REGION_US // Preserves the original hardcoded 915 MHz behavior

// PCP carrier frequency per region (must match the remote)
#define LORA_PCP_FREQ_US_HZ 915000000UL // US 915 MHz band center
// EU: the 869.4-869.65 MHz high-power sub-band (ETSI allows 500 mW / 27 dBm, 10% duty), so PCP's 22 dBm TX stays legal
// BW125 at 869.5 spans 869.4375-869.5625, inside the sub-band
#define LORA_PCP_FREQ_EU_HZ 869500000UL

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
 * @brief Load the persisted LoRa spreading factor from NVS
 *
 * @returns The stored SF as a plain numeric value (LORA_PCP_SF_MIN..LORA_PCP_SF_MAX).
 *          Returns LORA_PCP_SF_DEFAULT when no value is stored or the stored value
 *          is out of range, so callers always receive a radio-safe SF.
 */
uint8_t lora_pcp_load_sf_nvs(void);

/**
 * @brief Persist the LoRa spreading factor to NVS (received from the remote during sync)
 *
 * @param [in] sf Spreading factor as a plain numeric value; clamped to
 *                LORA_PCP_SF_MIN..LORA_PCP_SF_MAX before it is stored.
 *
 * @returns ESP_OK on success, otherwise the failing NVS error code.
 */
esp_err_t lora_pcp_save_sf_nvs(uint8_t sf);

/**
 * @brief Load the persisted LoRa region from NVS
 *
 * @returns The stored region (LORA_REGION_US or LORA_REGION_EU). Returns
 *          LORA_REGION_DEFAULT when no value is stored or the stored value is
 *          out of range, so callers always receive a valid region.
 */
lora_region_t lora_pcp_load_region_nvs(void);

/**
 * @brief Persist the LoRa region to NVS (received from the remote during sync)
 *
 * @param [in] region Region to store; clamped to a valid lora_region_t before
 *                    it is written.
 *
 * @returns ESP_OK on success, otherwise the failing NVS error code.
 */
esp_err_t lora_pcp_save_region_nvs(lora_region_t region);

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
 *
 * @return True if ACK TX was started (caller should wait for TX_DONE to re-arm RX),
 *         false if no TX was started (caller must re-arm RX)
 */
bool lora_pcp_send_receipt(void);

/**
 * @brief Encrypts plaintext with AES-128-CCM and transmits over LoRa
 *
 * @param [in] plaintext The data to encrypt and transmit
 * @param [in] plaintext_len Length of the data to encrypt and transmit
 *
 * @return True on TX success, false on failure (caller must re-arm RX)
 */
bool lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len);

#endif // LORA_PCP_H
