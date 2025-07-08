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
 */
void gpio_init(void);

/**
 * @brief Initialize SPI
 */
void gpio_spi_init(void);

/**
 * @brief Initialize gpio pins
 *
 * @param [in] s Time to look up
 *
 * @return Passed time in ticks
 */
TickType_t gpio_lookup_time_ticks(const char *s);

/**
 * @brief Initialize gpio pins
 *
 * @param [in] s Time to look up
 *
 * @return Passed time in ticks
 */
TickType_t gpio_get_random_ticks_from_range(int min_m, int max_m);

/**
 * @brief Toggle AC relay
 *
 * @param [in] on State to put relay: on or off
 */
void gpio_relay_toggle(bool on);

/**
 * @brief Read the voltage at the AC ADC pin
 *
 * @return The measured voltage
 */
float gpio_get_ac_voltage(void);

/**
 * @brief Set RGB LED based on RX status
 *
 * @param [in] ready If ready to receive or not
 */
void gpio_rgb_ready_to_rx(bool ready);

/**
 * @brief Set RGB LED based on Wi-Fi status
 *
 * @param [in] connected If connected to Wi-Fi
 */
void gpio_rgb_wifi_status(bool connected);

#endif // GPIO_FUNCS_H