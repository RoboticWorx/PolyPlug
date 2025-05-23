#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#define RELAY_PIN 26

/**
 * @brief Initialize gpio pins
 *
 * @return ESP_OK on success
 */
void gpio_init(void);

void gpio_spi_init(void);

#endif // GPIO_FUNCS_H