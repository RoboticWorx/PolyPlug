#ifndef WIFI_FUNCS_H
#define WIFI_FUNCS_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
    char key[33];
} wifi_mqtt_t;

/**
 * @brief Connect to a given Wi-Fi network
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_connect(void);

/**
 * @brief Disconnects from a given Wi-Fi network: to be called before esp_now_init
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_wifi_disconnect(void);

/**
 * @brief Configure the radio to join a given network
 *
 * @param [in] ssid Network SSID
 * @param [in] bssid Network BSSID
 * @param [in] password Network password
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_set_config(const char *ssid, const uint8_t* bssid, const char *password);

/**
 * @brief Gets the previously known network info from NVS
 *
 * @return Last network via wifi_mqtt_t
 */
wifi_mqtt_t wifi_funcs_get_prev(void);

/**
 * @brief Initializes Wi-Fi event handler
 */
void wifi_funcs_wifi_event_init(void);

/**
 * @brief Initializes MQTT client
 */
void wifi_funcs_mqtt_client_init(void);

/**
 * @brief Fetches current date and time via Wi-Fi
 */
void wifi_funcs_get_current_date_time(void);

/**
 * @brief Saves a given MAC address to NVS flash
 *
 * @param [in] mac The MAC address being saved
 */
void wifi_funcs_mac_nvs_save(const char *mac);

/**
 * @brief Loads a given MAC address from NVS flash: returns to global
 */
void wifi_funcs_mac_nvs_load(void);

#endif // WIFI_FUNCS_H