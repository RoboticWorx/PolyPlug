#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_log.h"

#include "gpio_funcs.h"
#include "gpio_task.h"

static const char *TAG = "GPIO_TASK";

static bool relay_level = true;

relay_t relay_rx;

QueueHandle_t xRelayToggleQueue;

static void gpio_task(void *arg)
{
	// Create queue
	xRelayToggleQueue = xQueueCreate(1, sizeof(relay_t));
	if (xRelayToggleQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xRelayToggleQueue");
	}
		
	while (1) 
	{
		// If received queue data (loop command)
		if (xQueueReceive(xRelayToggleQueue, &relay_rx, 1) == pdPASS) {
			if (relay_rx.index == 0) {
				gpio_set_level(RELAY_PIN, relay_level);
    			relay_level = !relay_level;
			}
			else if (relay_rx.index == 1) {
				ESP_LOGI(TAG, "RECEIVED: on=%s, off=%s", relay_rx.loop_on, relay_rx.loop_off);

	            // Turn received time on/off into ticks
	            TickType_t on_ticks = gpio_lookup_time_ticks(relay_rx.loop_on);
	            TickType_t off_ticks = gpio_lookup_time_ticks(relay_rx.loop_off);
	
	            // Loop until new data arrives
	            while(1) {
					
	                // Start ON loop for duration until new data received
	                gpio_set_level(RELAY_PIN, 1);
	                relay_level = false; // False to toggle from true if toggle cmd sent
	                if (xQueueReceive(xRelayToggleQueue, &relay_rx, on_ticks) == pdPASS) {
	                    ESP_LOGI(TAG, "Pattern updated during ON: index=%d on=%s off=%s",
							relay_rx.index, relay_rx.loop_on, relay_rx.loop_off);
							
						// Re-send data to unlock queue at the top
						xQueueSend(xRelayToggleQueue, &relay_rx, 1);
						
	                    break;
	                }
	                
	                // Start OFF loop for duration until new data received
	                gpio_set_level(RELAY_PIN, 0);
	                relay_level = true; // True to toggle from false if toggle cmd sent
	                if (xQueueReceive(xRelayToggleQueue, &relay_rx, off_ticks) == pdPASS) {
						ESP_LOGI(TAG, "Pattern updated during OFF: index=%d on=%s off=%s",
							relay_rx.index, relay_rx.loop_on, relay_rx.loop_off);
						
						// Re-send data to unlock queue at the top
						xQueueSend(xRelayToggleQueue, &relay_rx, 1);
						
	                    break;
	                }
	            }
			}
		}
			
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void gpio_task_create(void)
{
	xTaskCreate(gpio_task, "gpio_task", 4096, NULL, tskIDLE_PRIORITY + 1,
				NULL);
}