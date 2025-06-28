#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xWifiReconnectSemaphore;

extern QueueHandle_t xWifiConnectQueue;

/**
 * @brief Create the Wi-Fi task
 */
void wifi_task_create(void);


#endif // WIFI_TASK_H