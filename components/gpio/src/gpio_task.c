#include "freertos/projdefs.h"
#include "polyplug_macros.h"

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
	configASSERT(xRelayToggleQueue);
	
	while (1) 
	{
		// Toggle relay with button press
		if (gpio_get_level(PAIR_BTN2_PIN) == 0) {
			gpio_relay_toggle(relay_level);
				
    		relay_level = !relay_level;
    		
			vTaskDelay(pdMS_TO_TICKS(500)); // Wait for debounce
		}
		
		// If received relay queue data gpio_relay_toggle(false);
		if (xQueueReceive(xRelayToggleQueue, &relay_rx, 1) == pdPASS) {
			if (relay_rx.index == -2) { // Relay OFF
				gpio_relay_toggle(false);
				relay_level = true; // True to toggle from false if toggle cmd sent
			}
			else if (relay_rx.index == -1) { // Relay ON
				gpio_relay_toggle(true);
				relay_level = false; // False to toggle from true if toggle cmd sent
			}
			else if (relay_rx.index == 0) { // LoRa command toggle
				gpio_relay_toggle(relay_level);
				
    			relay_level = !relay_level;
			}
			else if (relay_rx.index == 1) { // LoRa command loop
				#ifdef POLYPLUG_DEBUG
					ESP_LOGI(TAG, "RECEIVED: on=%s, off=%s", relay_rx.loop_on, relay_rx.loop_off);
				#endif

	            // Turn received time on/off into ticks
	            TickType_t on_ticks = gpio_lookup_time_ticks(relay_rx.loop_on);
	            TickType_t off_ticks = gpio_lookup_time_ticks(relay_rx.loop_off);
	            
	            #ifdef POLYPLUG_DEBUG
					ESP_LOGI(TAG, "TICKS: on=%" PRIu32 ", off=%" PRIu32, on_ticks, off_ticks);
					ESP_LOGI(TAG, "MIN: on=%" PRIu32 ", off=%" PRIu32, (pdTICKS_TO_MS(on_ticks) / (1000 * 60)), (pdTICKS_TO_MS(off_ticks) / (1000 * 60)));
				#endif
	
	            // Loop until new data arrives
	            while(1) {
	                // Start ON loop for duration until new data received
	                gpio_relay_toggle(true);
	                relay_level = false; // False to toggle from true if toggle cmd sent
	                if (xQueueReceive(xRelayToggleQueue, &relay_rx, on_ticks) == pdPASS) {
						#ifdef POLYPLUG_DEBUG
		                    ESP_LOGI(TAG, "Pattern updated during ON: index=%d on=%s off=%s",
								relay_rx.index, relay_rx.loop_on, relay_rx.loop_off);
						#endif
							
						// Re-send data to unlock queue at the top
						xQueueSend(xRelayToggleQueue, &relay_rx, 1);
						
	                    break;
	                }
	                
	                // Start OFF loop for duration until new data received
	                gpio_relay_toggle(false);
	                relay_level = true; // True to toggle from false if toggle cmd sent
	                if (xQueueReceive(xRelayToggleQueue, &relay_rx, off_ticks) == pdPASS) {
						#ifdef POLYPLUG_DEBUG
							ESP_LOGI(TAG, "Pattern updated during OFF: index=%d on=%s off=%s",
								relay_rx.index, relay_rx.loop_on, relay_rx.loop_off);
						#endif
						
						// Re-send data to unlock queue at the top
						xQueueSend(xRelayToggleQueue, &relay_rx, 1);
						
	                    break;
	                }
	            }
			}
			// Away mode
			else if (relay_rx.index == 3) {
			    int min_m = relay_rx.away_min;
			    int max_m = relay_rx.away_max;
				
			    while (1) {			
					// Generate a random ON duration in range			
			        TickType_t delay_ticks = gpio_get_random_ticks_from_range(min_m, max_m);
			        
			        // For logging
			        int total_s = (uint32_t)(delay_ticks * portTICK_PERIOD_MS / 1000);
			        int m = total_s / 60;
			        int s = total_s % 60;
			        
			        #ifdef POLYPLUG_DEBUG
				        ESP_LOGI(TAG, "Away ON for %d min %d sec", m, s);
			        #endif
			        
			        gpio_relay_toggle(true);
			        relay_level = false; // False to toggle from true if toggle cmd sent
			        
			        // Wait in ON state unless a new command arrives
			        if (xQueueReceive(xRelayToggleQueue, &relay_rx, delay_ticks) == pdPASS) {
						#ifdef POLYPLUG_DEBUG
				            ESP_LOGI(TAG, "Away mode updated during ON");
			            #endif
			            
			            // Re-send data to unlock queue at the top
			            xQueueSend(xRelayToggleQueue, &relay_rx, 0);
			            
			            break;
			        }
			
			        // Generate a random OFF duration in range
			        delay_ticks = gpio_get_random_ticks_from_range(min_m, max_m);
			        
			        // For logging
			        total_s = (uint32_t)(delay_ticks * portTICK_PERIOD_MS / 1000);
			        m = total_s / 60;
			        s = total_s % 60;
			        
			        #ifdef POLYPLUG_DEBUG
				        ESP_LOGI(TAG, "Away OFF for %d min %d sec", m, s);
			        #endif
			        
			        gpio_relay_toggle(false);
			        relay_level = true; // True to toggle from false if toggle cmd sent
			        
			        // Wait in OFF state unless a new command arrives
			        if (xQueueReceive(xRelayToggleQueue, &relay_rx, delay_ticks) == pdPASS) {
						#ifdef POLYPLUG_DEBUG
				            ESP_LOGI(TAG, "Away mode updated during OFF");
			            #endif
			            
			            // Re-send data to unlock queue at the top
			            xQueueSend(xRelayToggleQueue, &relay_rx, 0);
			            
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
	if (xTaskCreate(gpio_task, "gpio_task", 1024 * 2, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to create gpio_task");
	}
}