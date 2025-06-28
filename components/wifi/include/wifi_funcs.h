#ifndef WIFI_FUNCS_H
#define WIFI_FUNCS_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
    char mac[13];
} wifi_mqtt_t;

/**
 * @brief Connect to a given Wi-Fi network
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_connect(void);

/**
 * @brief Configure and start the radio to join a given network
 *
 * @param [in] ssid Network SSID
 * @param [in] bssid Network BSSID
 * @param [in] password Network password
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_radio_start(const char *ssid, const uint8_t* bssid, const char *password);

void wifi_funcs_wifi_event_init(void);
void wifi_funcs_mqtt_client_init(void);

esp_err_t wifi_funcs_radio_stop(void);
void wifi_funcs_get_current_date_time(void);
wifi_mqtt_t wifi_funcs_get_prev(void);


#endif // WIFI_FUNCS_H