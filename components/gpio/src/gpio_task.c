#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_log.h"

#include "gpio_funcs.h"
#include "gpio_task.h"

static const char *TAG = "GPIO_TASK";

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
		// If received queue data 
		if (xQueueReceive(xRelayToggleQueue, &relay_rx, 1) == pdPASS) {
			ESP_LOGI(TAG, "RECEIVED: on=%s, off=%s", relay_rx.loop_on, relay_rx.loop_off);

            // Turn received time on/off into ticks
            TickType_t on_ticks = gpio_lookup_time_ticks(relay_rx.loop_on);
            TickType_t off_ticks = gpio_lookup_time_ticks(relay_rx.loop_off);

            // Loop until a new pattern arrives
            while(1) {
				
                // Check if a new command has come in _without_ blocking
                if (xQueueReceive(xRelayToggleQueue, &relay_rx, 0) == pdPASS) {
                    // Break out to outer loop
                    ESP_LOGI(TAG, "Pattern updated: on=%s off=%s", relay_rx.loop_on, relay_rx.loop_off);
                    break;
                }
                
                // Otherwise drive the pins at their given periods
                gpio_set_level(RELAY_PIN, 1);
                vTaskDelay(on_ticks);

                gpio_set_level(RELAY_PIN, 0);
                vTaskDelay(off_ticks);
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