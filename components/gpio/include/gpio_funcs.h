#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#include "freertos/FreeRTOS.h"

#define RELAY_PIN 26
#define PAIR_BTN1_PIN 10
#define PAIR_BTN2_PIN 9

#define RGB_RED_PIN 12
#define RGB_GREEN_PIN 11
#define RGB_BLUE_PIN 25

/**
 * @brief Initialize gpio pins
 *
 * @return ESP_OK on success
 */
void gpio_init(void);

void gpio_spi_init(void);

TickType_t gpio_lookup_time_ticks(const char *s);

TickType_t gpio_get_random_ticks_from_range(int min_m, int max_m);

#endif // GPIO_FUNCS_H