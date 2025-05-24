#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define WIFI_CHANNEL 1

#define ENC_KEY_LEN 16

extern QueueHandle_t xReceivedEncKeyQueue;

/**
 * @brief Create the ESP-NOW task
 */
void espnow_task_create(void);


#endif // ESPNOW_TASK_H