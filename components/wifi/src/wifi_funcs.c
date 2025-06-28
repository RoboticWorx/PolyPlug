#include "polyplug_macros.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "esp_sntp.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "gpio_task.h"
#include "gpio_funcs.h"
#include "espnow_funcs.h"

#define TAG "WIFI_FUNCS"

#define WIFI_CONNECTED_BIT (1 << 0)
#define WIFI_DISCONNECTED_BIT (1 << 1)

/* Helpers to pull type/subtype from the 802.11 frame control */
#define FC_TYPE(fc)    (((fc) & 0x0C) >> 2)
#define FC_SUBTYPE(fc) (((fc) & 0xF0) >> 4)
#define TYPE_MGMT 0x00
#define SUBTYPE_BEACON 0x08


static esp_mqtt_client_handle_t mqtt_client;

static uint8_t target_bssid[6] = { 0x60, 0x55, 0xF9, 0xFC, 0xDE, 0xA8 };
static EventGroupHandle_t wifi_event_group;

bool wifi_connected = false;

void wifi_funcs_get_current_date_time(void)
{
	static bool initialized = false;
	
	if (!initialized) {
		// Tell SNTP client to poll for time
	    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
	
	    // Point STNP client at a given server
	    esp_sntp_setservername(0, "pool.ntp.org"); // or time.nist.gov
	
		// Init and start SNTP service
	    esp_sntp_init();
	    
	    initialized = true;
	}
	    
    time_t now = 0;
    struct tm timeinfo = {0};

    // Wait until the SNTP task clock has gone past 2025
    while (timeinfo.tm_year < (2025 - 1900)) {
        vTaskDelay(pdMS_TO_TICKS(100));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    char strftime_buf[64];
    
    // Configure the timezone environment
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
	tzset();
    // • Standard time = UTC–5 (“EST”)
    // • Daylight time = UTC–4 (“EDT”)
    // • DST starts 2nd Sunday in March at 2 AM
    // • DST ends 1st Sunday in November at 2 AM
    // `tzset()` makes the library re-read that TZ rule now.

	// Get the epoch time
    time(&now);
    
    // Convert to a local broken-out form
    localtime_r(&now, &timeinfo);
    // `time()` returns seconds since Jan 1 1970 UTC,
    // `localtime_r()` applies your TZ rules into a `struct tm`.

    // Render it as “YYYY-MM-DD HH:MM:SS” into our buffer
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

	#ifdef POLYPLUG_DEBUG
    	ESP_LOGI(TAG, "Current date/time 24h: %s", strftime_buf);
    #endif
    
    // 12-hour format with AM/PM:
	strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %I:%M:%S %p", &timeinfo);
	
	#ifdef POLYPLUG_DEBUG
    	ESP_LOGI(TAG, "Current date/time 12h: %s", strftime_buf);
    #endif
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
	// Disconnection event
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)data;
        
        #ifdef POLYPLUG_DEBUG
        	ESP_LOGW(TAG, "Disconnected, reason=%d", d->reason);
        #endif
        
        wifi_funcs_radio_stop();
        
        gpio_rgb_wifi_status(false);

        xEventGroupSetBits(wifi_event_group, WIFI_DISCONNECTED_BIT);
    }
    // Connected event
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        
        #ifdef POLYPLUG_DEBUG
        	ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        #endif
        
        gpio_rgb_wifi_status(true);
        
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        		
		wifi_connected = true;
    }
}

void wifi_funcs_wifi_event_init(void)
{
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
    			 NULL, NULL));
    			 
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
    			 NULL, NULL));
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
        	#ifdef POLYPLUG_DEBUG
            	ESP_LOGI(TAG, "Connected to MQTT");
            #endif
            break;
        case MQTT_EVENT_DISCONNECTED:
        	#ifdef POLYPLUG_DEBUG
            	ESP_LOGI(TAG, "Disconnected from MQTT");
            #endif
            break;
        default:
            break;
    }
}

void wifi_funcs_mqtt_client_init(void)
{
	esp_mqtt_client_config_t cfg = {
	    .broker = {
	        .address = {
	            .uri = "mqtt://test.mosquitto.org"
	        }
	    },
	    .session = {
	        .keepalive = 60
	    }
	};
    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
}

static bool wait_for_connection(TickType_t timeout)
{
    // Wait for either bit
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT, pdTRUE,
    			pdFALSE, timeout);

	// Got IP
    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }
    
    // Disconnected
    if (bits & WIFI_DISCONNECTED_BIT) {
        return false;
    }
    
    // Timed out
    return false;
}

esp_err_t wifi_funcs_connect(void)
{
	xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
	
    esp_err_t err = esp_wifi_connect();
    
    // Check connetion
    if (wait_for_connection(pdMS_TO_TICKS(15000))) {
		#ifdef POLYPLUG_DEBUG
    		ESP_LOGI(TAG, "Wi-Fi connected and got IP!");
    	#endif
    	esp_mqtt_client_start(mqtt_client); // Start mqtt client
	}
	else {
	    ESP_LOGE(TAG, "Failed to connect");
	    // Notify LCD
	    wifi_funcs_radio_stop();
	}
	
	return err;
}

esp_err_t wifi_funcs_radio_start(const char *ssid, const uint8_t* bssid, const char *password)
{
	wifi_config_t cfg = {0};
    
    // Copy in SSID and password
    strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password));
    
    // Copy BSSID
    //cfg.sta.bssid_set = true;
	//memcpy(cfg.sta.bssid, bssid, sizeof(cfg.sta.bssid));
	
	cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // Weakest auth mode to accept in the fast scan mode 

	#ifdef POLYPLUG_DEBUG
    	ESP_LOGI(TAG, "Setting Wi-Fi config SSID='%s'", ssid);
    	//ESP_LOGI(TAG, "Setting Wi-Fi config BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
		//     bssid[0], bssid[1], bssid[2],
		//     bssid[3], bssid[4], bssid[5]);
    	ESP_LOGI(TAG, "Setting Wi-Fi config password='%s'", password);
    #endif
    
    // Set mode
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
		return err;
	}
	
	// Set config
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
    if (err != ESP_OK) {
		return err;
	}
	
	// Start the driver
    err = esp_wifi_start();
    
    return err;
}

esp_err_t wifi_funcs_radio_stop(void)
{
	esp_mqtt_client_stop(mqtt_client); // Stop possible client
	esp_wifi_disconnect(); // Disconnect if connected
	
	// Stop Wi-Fi
	esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
		ESP_LOGE(TAG, "wifi_funcs_radio_stop: %d", err);
	}
    
    wifi_connected = false;
    
    return err;
}

wifi_mqtt_t wifi_funcs_get_prev(void)
{
	wifi_config_t current;
	ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &current));
	
	wifi_mqtt_t prev;
	strlcpy(prev.ssid, (char*)current.sta.ssid, sizeof(current.sta.ssid));
	strlcpy(prev.password, (char*)current.sta.password, sizeof(current.sta.password));
	
	return prev;
}
