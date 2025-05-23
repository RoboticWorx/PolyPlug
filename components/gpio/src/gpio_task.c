#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "gpio_funcs.h"
#include "gpio_task.h"

static const char *TAG = "GPIO_TASK";

//SemaphoreHandle_t xSPIBusMutex;

static void gpio_task(void *arg)
{
	//xSPIBusMutex = xSemaphoreCreateMutex();
	//configASSERT(xSPIBusMutex); // Ensure success
	
	//xUpButtonSemaphore = xSemaphoreCreateBinary();
	
	while (1) 
	{
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void gpio_task_create(void)
{
	xTaskCreate(gpio_task, "gpio_task", 4096, NULL, tskIDLE_PRIORITY + 1,
				NULL);
}