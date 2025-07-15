#include "polyplug_macros.h"

#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "gpio_funcs.h"
#include "gpio_task.h"

#define MAX_PLANS 10 // Concurrent 'plan' times allowed to run in parallel

static TaskHandle_t plan_tasks[MAX_PLANS];
static size_t plan_count = 0;

static const char *TAG = "GPIO_TASK";

static bool relay_level = true;

relay_t relay_rx;

QueueHandle_t xRelayToggleQueue;

/* Plan mode helpers */
// Parse “1...7” day format into bitmask
static int parse_weekdays(const char *days) {
    int mask = 0; // Start 0
    
    // Loop over each character in the days string until you hit the terminator
    for (; *days; days++) {
        int digit = *days - '0'; // Convert ASCII digit to integer
        
        // Only accept digits 1 through 7 (valid days)
        if (digit >= 1 && digit <= 7) {
            int w = (digit == 7 ? 0 : digit); // If digit==7 (Sunday), map to w=0. Otherwise, Monday(1)->1, Tuesday(2)->2
            
            // Set the bit in the mask
            mask |= (1 << w);
        }
    }
    return mask;
}
// Parse “HHMMSS” format into the number of seconds since midnight
static int parse_hhmmss(const char *s) {
    int int_str = atoi(s); // Parse the string into an integer (turn directly)
    
    int hour = int_str / 10000; // Isolate hours
    int min = (int_str / 100) % 100; // Isolate minutes
    int sec = int_str % 100; // Isolate seconds
    
    return hour * 3600 + min * 60 + sec; // Seconds since midnight
}
// Find the next epoch >= now that matches days_mask and sec_of_day
static time_t next_event_epoch(int days_mask, int sec_of_day) {
    time_t now = time(NULL); // Read the current epoch (seconds since 1970)
    
    // Break that epoch into a calendar struct tm
    struct tm tmn;
    localtime_r(&now, &tmn);
    
    // Compute the epoch at today's midnight
    time_t today0 = now - (tmn.tm_hour * 3600 + tmn.tm_min * 60 + tmn.tm_sec);

	// Try each offset from d=0 (today) to d=6
    for (int day = 0; day < 7; day++) {
		// Compute weekday
        int weekday = (tmn.tm_wday + day) % 7;
        
        // If that weekday is enabled in your days_mask, build a candidate epoch
        if (days_mask & (1 << weekday)) {
            time_t candidate = today0 + day * 86400 + sec_of_day;
            
            // If cand > now, that's the very next matching event so return it.
            if (candidate > now) {
				return candidate;
			}
        }
    }
    
    // Fallback: first matching day next week
    for (int day = 1; day <= 7; day++) {
		// Compute weekday
        int weekday = (tmn.tm_wday + day) % 7;
        
        // If that weekday is enabled in your days_mask, build a candidate epoch
        if (days_mask & (1 << weekday)) {
            return today0 + day * 86400 + sec_of_day;
        }
    }
    return now;
}

/* Plan mode task */
static void plan_mode_task(void *arg) {
	// Grab heap-allocated structure
    relay_t *plan = arg;
    
    // Precompute masks and seconds
    int days_mask = parse_weekdays(plan->plan_days);
    int on_sec = parse_hhmmss(plan->plan_on);
    int off_sec = parse_hhmmss(plan->plan_off);
    
    free(plan); // No longer needed

    while (1) {
		// Read current epoch again
        time_t now = time(NULL);
        
        // Compute the absolute epoch of the next ON
        time_t on_time = next_event_epoch(days_mask, on_sec);
        
        // Compute the next OFF
        time_t off_time = next_event_epoch(days_mask, off_sec);
        
        // If it would fall before the ON, bump it by one week.
        if (off_time <= on_time) {
			off_time += 7 * 86400;
		}

        // Wait until ON time
        vTaskDelay(pdMS_TO_TICKS((on_time - now) * 1000));
        gpio_relay_toggle(true);

        // then wait until OFF time
        vTaskDelay(pdMS_TO_TICKS((off_time - on_time) * 1000));
        gpio_relay_toggle(false);
    }
}

/* Main task */
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
			// Plan mode
			else if (relay_rx.index == 2) {	            
	            if (plan_count < MAX_PLANS) {
			        relay_t *plan = malloc(sizeof(*plan));
			        *plan = relay_rx;
			        if (xTaskCreate(plan_mode_task, "plan_mode", 1024 * 2,
	                		plan, tskIDLE_PRIORITY + 1, &plan_tasks[plan_count]) != pdPASS) {
						free(plan);
						ESP_LOGE(TAG, "Failed to create plan_mode_task");
					}
					else {
						plan_count++;
					}
			    }
			    else {
			    	ESP_LOGE(TAG, "Plan queue full (%u/%u)", plan_count, MAX_PLANS);
			    }
			    
			    /*
			    NOT YET IMPLEMENTED: For reference reset all code:
			    for (size_t i = 0; i < plan_count; ++i) {
			        vTaskDelete(plan_tasks[i]);
			        ESP_LOGI(TAG, "Deleted plan task %u", (unsigned)i);
			    }
			    plan_count = 0;
			    ESP_LOGI(TAG, "All plan tasks cleared");
			    */
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