#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define WIFI_CHANNEL 1

#define ESPNOW_MAGIC "PC5: " // Magic prefix on the LoRa key sync frame (must match the PolyCast5 remote)
#define ESPNOW_MAGIC_LEN (sizeof(ESPNOW_MAGIC) - 1) // Length excluding the NUL terminator

#define MQTT_MAX_LEN 134

extern QueueHandle_t xEspReceivedEncKeyQueue;

/**
 * @brief Create the ESP-NOW task
 */
void espnow_task_create(void);


#endif // ESPNOW_TASK_H