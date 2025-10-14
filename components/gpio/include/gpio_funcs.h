#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#include "freertos/FreeRTOS.h"

#define RELAY_PIN 26
#define PAIR_BTN1_PIN 10
#define PAIR_BTN2_PIN 9

#define RGB_RED_PIN 12
#define RGB_GREEN_PIN 11
#define RGB_BLUE_PIN 25

// Pin numbering - Left to right from the front on PolyPlug HWv3:
// GND -> GND -> ADC_AC -> IO4 -> IO27 -> IO23 -> IO24 -> 3V3 -> 3V3 -> 5V
#define GPIO_BIT_1_PIN 24
#define GPIO_BIT_2_PIN 23
#define GPIO_BIT_3_PIN 27
#define GPIO_BIT_4_PIN 4

#define LOOP_LEN 4
#define PLAN_DAYS_LEN 8 // 7 days + NULL
#define PLAN_TIME_LEN 7 // "HHMMSS" + NULL

typedef struct {
	int index;
    char loop_on[LOOP_LEN];
    char loop_off[LOOP_LEN];
    char plan_days[PLAN_DAYS_LEN];
    char plan_on[PLAN_TIME_LEN];
    char plan_off[PLAN_TIME_LEN];
    int away_min;
    int away_max;
    int gpio_cmd;
} relay_t;

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

/**
 * @brief Turn on a specific or multiple RGB LEDs
 *
 * @param [in] r_on 0 turns RED off
 * @param [in] g_on 0 turns GREEN off
 * @param [in] b_on 0 turns BLUE off
 */
void gpio_rgb_set(int r_on, int g_on, int b_on);

/**
 * @brief Cycles through RGB LEDs: non-blocking
 *
 * @param [in] period_ms Period to cycle at in milliseconds
 */
void gpio_rgb_cycle_tick(uint32_t period_ms);

/**
 * @brief Writes a given number high in binary via the GPIOs
 *
 * @param [in] pulse_ms Period to hold the pins high in milliseconds (non-blocking)
 */
void gpio_pulse_4bit_bus(uint8_t value, uint32_t pulse_ms);

#endif // GPIO_FUNCS_H