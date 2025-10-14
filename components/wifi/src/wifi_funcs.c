#include "polyplug_macros.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "nvs.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "gpio_task.h"
#include "gpio_funcs.h"
#include "espnow_funcs.h"
#include "ota_update.h"

#define TAG "WIFI_FUNCS"

#define MQTT_NS "ma_ns"
#define MQTT_NS_KEY "ma_ns_ke"

#define WIFI_CONNECTED_BIT (1 << 0)
#define WIFI_DISCONNECTED_BIT (1 << 1)

/* Helpers to pull type/subtype from the 802.11 frame control */
#define FC_TYPE(fc)	(((fc) & 0x0C) >> 2)
#define FC_SUBTYPE(fc) (((fc) & 0xF0) >> 4)
#define TYPE_MGMT 0x00
#define SUBTYPE_BEACON 0x08

extern bool connected_to_network;
extern bool using_espnow;

static esp_mqtt_client_handle_t mqtt_client;

static EventGroupHandle_t wifi_event_group;

static char mqtt_topic_key_str[33];
static char topic_cmd[64];
static char topic_ack[64];

static const struct { const char *iana; const char *posix; } TZ_MAP[] = {
	// Majors
	{"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
	{"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
	{"America/Phoenix", "MST7"}, // No DST
	{"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
	{"America/Anchorage", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
	{"America/Adak", "HAST10HADT,M3.2.0/2,M11.1.0/2"}, // Aleutian (has DST)
	{"Pacific/Honolulu", "HST10"}, // No DST

	// Territories
	{"America/Puerto_Rico", "AST4"}, // No DST
	{"America/St_Thomas", "AST4"}, // USVI, no DST
	{"Pacific/Guam", "ChST-10"}, // UTC+10, no DST
	{"Pacific/Saipan", "ChST-10"}, // Same
	{"Pacific/Pago_Pago", "SST11"}, // UTC-11, no DST

	// Useful aliases (map to their major rules)
	{"America/Detroit", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Kentucky/Louisville", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Kentucky/Monticello", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Indianapolis", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Marengo", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Vevay", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Vincennes", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Winamac", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Petersburg", "EST5EDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Knox", "CST6CDT,M3.2.0/2,M11.1.0/2"},
	{"America/Indiana/Tell_City", "CST6CDT,M3.2.0/2,M11.1.0/2"},
	{"America/North_Dakota/Center", "CST6CDT,M3.2.0/2,M11.1.0/2"},
	{"America/North_Dakota/New_Salem", "CST6CDT,M3.2.0/2,M11.1.0/2"},
	{"America/North_Dakota/Beulah", "CST6CDT,M3.2.0/2,M11.1.0/2"},
	{"America/Boise", "MST7MDT,M3.2.0/2,M11.1.0/2"},
};

// Find a POSIX rule for a given IANA ID (exact match); returns NULL if unmapped
static const char* iana_to_posix(const char *iana)
{
	// Iterate the table
	for (size_t i = 0; i < sizeof(TZ_MAP) / sizeof(TZ_MAP[0]); ++i) {
		// Compare IANA strings
		if (strcmp(iana, TZ_MAP[i].iana) == 0) {
			// Return mapped POSIX rule
			return TZ_MAP[i].posix;
		}
	}
	// Not found in our compact table
	return NULL;
}

// Apply a fixed-offset POSIX TZ built from API offsets (correct "now", no future DST rules)
static void tz_apply_fixed_posix_from_offsets(int raw_offset_s, int dst_offset_s, bool dst_now)
{
	// Combine base UTC offset and DST add-on (seconds east of UTC, negative for the Americas)
	int total = raw_offset_s + (dst_now ? dst_offset_s : 0);

	// POSIX sign is inverted relative to UTC (UTC-4 → "UTC+4")
	int sec = -total;

	// Capture sign and make value positive for formatting
	int sign = (sec < 0) ? -1 : 1;
	sec = (sec < 0) ? -sec : sec;

	// Split into hours and minutes
	int h = sec / 3600;
	int m = (sec % 3600) / 60;

	// Format buffer
	char tzbuf[32];

	// Render "UTC±H" or "UTC±H:MM"
	if (m == 0) {
		// Whole-hour offset
		snprintf(tzbuf, sizeof(tzbuf), "UTC%+d", sign * h);
	}
	else {
		// Sub-hour offset (e.g., :30, :45)
		snprintf(tzbuf, sizeof(tzbuf), "UTC%+d:%02d", sign * h, m);
	}

	// Set the TZ environment variable
	setenv("TZ", tzbuf, 1);

	// Apply the TZ immediately
	tzset();

	// Log the applied fixed-offset TZ
	#ifdef POLYPLUG_DEBUG
	ESP_LOGI("AUTO_TZ", "Applied fixed-offset TZ: %s", tzbuf);
	#endif
}

// Simple retrying HTTP GET (chunk-safe). Returns true with malloc'd body on HTTP 200
static bool http_get_body_retry(const char *url, char **out_body, size_t *out_len)
{
	// Attempt count
	const int attempts = 3;

	// Try multiple times with short backoff
	for (int i = 0; i < attempts; ++i) {
		// Configure HTTP client (HTTP, short timeout)
		esp_http_client_config_t cfg = {
			.url = url,
			.timeout_ms = 5000,
		};

		// Create client handle
		esp_http_client_handle_t cli = esp_http_client_init(&cfg);

		// If init failed, retry
		if (!cli) {
			continue;
		}

		// Some middleboxes dislike keep-alive; request connection close
		esp_http_client_set_header(cli, "Connection", "close");

		// Identify ourselves
		esp_http_client_set_header(cli, "User-Agent", "esp32");

		// Open the connection (sends GET)
		esp_err_t err = esp_http_client_open(cli, 0);

		// If open failed, cleanup and retry
		if (err != ESP_OK) {
			esp_http_client_cleanup(cli);
			vTaskDelay(pdMS_TO_TICKS(300));
			continue;
		}

		// Parse headers (content length may be unknown/chunked)
		(void)esp_http_client_fetch_headers(cli);

		// Initial buffer capacity and current length
		size_t cap = 1024;
		size_t len = 0;

		// Allocate body buffer
		char *body = (char*)malloc(cap);

		// If malloc failed, cleanup and abort
		if (!body) {
			esp_http_client_cleanup(cli);
			return false;
		}

		// Read until EOF or error
		while (1) {
			// Temporary read chunk
			char buf[512];

			// Read from socket
			int r = esp_http_client_read(cli, buf, sizeof(buf));

			// On read error, free and mark as failed
			if (r < 0) {
				free(body);
				body = NULL;
				break;
			}

			// r == 0 means EOF (server closed after sending body)
			if (r == 0) {
				break;
			}

			// Grow the buffer if needed (+1 for terminating NUL)
			if (len + (size_t)r + 1 > cap) {
				// Double the capacity to amortize reallocs
				size_t nc = (cap + (size_t)r + 1) * 2;

				// Attempt to grow the buffer
				char *nb = (char*)realloc(body, nc);

				// If realloc fails, free and mark as failed
				if (!nb) {
					free(body);
					body = NULL;
					break;
				}

				// Accept the grown buffer
				body = nb;
				cap = nc;
			}

			// Append chunk into the body buffer
			memcpy(body + len, buf, (size_t)r);

			// Advance total written
			len += (size_t)r;
		}

		// Capture HTTP status
		int status = esp_http_client_get_status_code(cli);

		// Cleanup the client object
		esp_http_client_cleanup(cli);

		// If we have a body and HTTP 200 OK, return success
		if (body && status == 200) {
			// NUL-terminate the body
			body[len] = '\0';

			// Return body pointer to caller
			*out_body = body;

			// Optionally output length
			if (out_len) {
				*out_len = len;
			}

			// Indicate success
			return true;
		}

		// If body was allocated but status was not OK, free it
		if (body) {
			free(body);
		}

		// Small backoff before retrying
		vTaskDelay(pdMS_TO_TICKS(300));
	}

	// All attempts failed
	return false;
}

// Trim ASCII whitespace in-place (both ends) on a mutable C string
static void strtrim_inplace(char *s)
{
	// Null guard
	if (!s) {
		return;
	}

	// Find first non-space
	char *start = s;
	while (*start && isspace((unsigned char)*start)) {
		++start;
	}

	// Move content to the front if needed
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}

	// Find new end
	char *end = s + strlen(s);

	// Walk back over trailing spaces
	while (end > s && isspace((unsigned char)*(end - 1))) {
		--end;
	}

	// Terminate after last non-space
	*end = '\0';
}

// Fetch IANA timezone over HTTP, map to POSIX or apply fixed-offset; returns ESP_OK on success
esp_err_t wifi_funcs_apply_timezone_auto(void)
{
	// Response body buffer
	char *body = NULL;

	// Default result is failure (caller may set TZ=UTC0 on failure)
	esp_err_t ret = ESP_FAIL;

	// Try #1: worldtimeapi.org (JSON: timezone + raw_offset/dst_offset/dst)
	if (http_get_body_retry("http://worldtimeapi.org/api/ip", &body, NULL)) {
		// Parse JSON
		cJSON *root = cJSON_Parse(body);

		// Free body buffer now that it's parsed
		free(body);
		body = NULL;

		// If JSON parsed
		if (root) {
			// Extract fields
			const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");
			const cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "raw_offset");
			const cJSON *dst_off = cJSON_GetObjectItemCaseSensitive(root, "dst_offset");
			const cJSON *dst_now = cJSON_GetObjectItemCaseSensitive(root, "dst");

			// Read IANA string if present
			const char *iana = (cJSON_IsString(tz) && tz->valuestring) ? tz->valuestring : NULL;

			// If IANA available
			if (iana) {
				// Try mapping to a full POSIX rule
				const char *posix = iana_to_posix(iana);

				// If mapping found, apply it
				if (posix) {
					// Set POSIX TZ
					setenv("TZ", posix, 1);

					// Apply immediately
					tzset();

					// Log success
					#ifdef POLYPLUG_DEBUG
					ESP_LOGI("AUTO_TZ", "Applied TZ: IANA='%s' -> POSIX='%s'", iana, posix);
					#endif

					// Mark success
					ret = ESP_OK;
				}
				// If unmapped but we have offsets, apply fixed-offset POSIX
				else if (cJSON_IsNumber(raw) && cJSON_IsNumber(dst_off) && cJSON_IsBool(dst_now)) {
					// Apply fixed-offset TZ that is correct "now"
					tz_apply_fixed_posix_from_offsets(raw->valueint, dst_off->valueint, cJSON_IsTrue(dst_now));

					// Mark success
					ret = ESP_OK;
				}
			}

			// Free JSON object
			cJSON_Delete(root);

			// If success, return immediately
			if (ret == ESP_OK) {
				return ret;
			}
		}
	}

	// Try #2: ip-api.com (JSON with "timezone" only; no offsets)
	if (http_get_body_retry("http://ip-api.com/json", &body, NULL)) {
		// Parse JSON
		cJSON *root = cJSON_Parse(body);

		// Free body buffer
		free(body);
		body = NULL;

		// If JSON parsed
		if (root) {
			// Extract "timezone" (IANA)
			const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");

			// Pull C string if present
			const char *iana = (cJSON_IsString(tz) && tz->valuestring) ? tz->valuestring : NULL;

			// If IANA present
			if (iana) {
				// Map to POSIX (table only; no offsets on this endpoint)
				const char *posix = iana_to_posix(iana);

				// If mapped, apply and succeed
				if (posix) {
					// Set POSIX TZ
					setenv("TZ", posix, 1);

					// Apply immediately
					tzset();

					// Log success (ip-api path)
					#ifdef POLYPLUG_DEBUG
					ESP_LOGI("AUTO_TZ", "Applied TZ: IANA='%s' -> POSIX='%s' (ip-api)", iana, posix);
					#endif

					// Mark success
					ret = ESP_OK;
				}
			}

			// Free JSON object
			cJSON_Delete(root);

			// If success, return immediately
			if (ret == ESP_OK) {
				return ret;
			}
		}
	}

	// Try #3: ipapi.co/timezone (plain text IANA string)
	if (http_get_body_retry("http://ipapi.co/timezone", &body, NULL)) {
		// Trim whitespace/newlines
		strtrim_inplace(body);

		// If non-empty string
		if (body[0]) {
			// Map to POSIX
			const char *posix = iana_to_posix(body);

			// If mapped, apply and succeed
			if (posix) {
				// Set POSIX TZ
				setenv("TZ", posix, 1);

				// Apply immediately
				tzset();

				// Log success (ipapi path)
				#ifdef POLYPLUG_DEBUG
				ESP_LOGI("AUTO_TZ", "Applied TZ: IANA='%s' -> POSIX='%s' (ipapi)", body, posix);
				#endif

				// Mark success
				ret = ESP_OK;
			}
		}

		// Free body buffer
		free(body);
		body = NULL;

		// If success, return immediately
		if (ret == ESP_OK) {
			return ret;
		}
	}

	// All providers failed or we couldn't map; let caller fall back to UTC0
	return ESP_FAIL;
}


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
		vTaskDelay(pdMS_TO_TICKS(1));
		time(&now);
		localtime_r(&now, &timeinfo);
	}
	
	// Get local time zone over http
	if (wifi_funcs_apply_timezone_auto() != ESP_OK) {
		// Fallback
		setenv("TZ", "UTC0", 1);
		tzset();
		
		ESP_LOGE(TAG, "wifi_funcs_apply_timezone_auto FAILED: Falling back to UTC0");
	}
	
	char strftime_buf[64];

	// Get the epoch time
	time(&now);
	
	// Convert to a local broken-out form
	localtime_r(&now, &timeinfo);
	// 'time()' returns seconds since Jan 1 1970 UTC,
	// 'localtime_r()' applies your TZ rules into a 'struct tm'.

	// Render it as 'YYYY-MM-DD HH:MM:SS' into our buffer
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
		
		connected_to_network = false;
		
		gpio_rgb_wifi_status(false); // Signal MQTT not connected (no Wi-Fi)
		
		xEventGroupSetBits(wifi_event_group, WIFI_DISCONNECTED_BIT);
	}
	// Connected event
	else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
		
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
		#endif
		
		connected_to_network = true;
				
		xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
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
			// Build the topic and ack strings
			snprintf(topic_cmd, sizeof(topic_cmd), "polycast5/%s/cmd", mqtt_topic_key_str);
			snprintf(topic_ack, sizeof(topic_ack), "polycast5/%s/ack", mqtt_topic_key_str);
			
			#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Connected to MQTT");
			#endif
			
			#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Subscribed to MQTT topic '%s'", topic_cmd);
			#endif
		
			esp_mqtt_client_subscribe(mqtt_client, topic_cmd, 0);
			
			gpio_rgb_wifi_status(true); // Signal MQTT connected
			break;
			
		case MQTT_EVENT_DISCONNECTED:
			#ifdef POLYPLUG_DEBUG
			ESP_LOGW(TAG, "Disconnected from MQTT");
			#endif
			
			gpio_rgb_wifi_status(false); // Signal MQTT not connected
			break;
			
		case MQTT_EVENT_DATA:
			ESP_LOGI(TAG, "MQTT DATA incoming on topic=%.*s", event->topic_len, event->topic);
			ESP_LOGI(TAG, "Payload=%.*s", event->data_len, event->data);
			
			if (event->topic_len == strlen(topic_cmd) && memcmp(event->topic, topic_cmd, event->topic_len) == 0) {
				#ifdef POLYPLUG_DEBUG
				ESP_LOGI(TAG, "Sent MQTT ACK on %s", topic_ack);
				#endif
				
				// Extract the payload
				char buf[4];
				size_t len = event->data_len < sizeof(buf) - 1 ? event->data_len : sizeof(buf) - 1;
				memcpy(buf, event->data, len);
				buf[len] = '\0';
		
				// Parse with sscanf
				int value;
				if (sscanf(buf, "%d", &value) == 1) {
					// Validate range
					if (value >= 0 && value <= 255) {
						#ifdef POLYPLUG_DEBUG
						ESP_LOGI(TAG, "Parsed payload as %d", value);
						#endif
						
						relay_t relay_tx;
						// Check for OTA update
						if (value == 255) {
							// Check for new firmware version and update if so
							ota_update_check_start("https://raw.githubusercontent.com/RoboticWorx/pc5-test/main/manifest.json");
						}
						// Relay ON cmd
						else if (value == 1) {
							relay_tx.index = -1;
							xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);
						}
						// Relay OFF cmd
						else if (value == 0) {
							relay_tx.index = -2;
							xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);
						}
						// GPIO toggle
						else {
							relay_tx.index = 4; // GPIO
				
							#ifdef POLYPLUG_DEBUG
							ESP_LOGI(TAG, "Parsed Wi-Fi GPIO command: %d", value);
							#endif
							
							// Save and send
							relay_tx.gpio_cmd = value;
							xQueueSend(xRelayToggleQueue, &relay_tx, portMAX_DELAY);
						}
					}
					else {
						#ifdef POLYPLUG_DEBUG
						ESP_LOGW(TAG, "Value out of range: %d", value);
						#endif
					}
				}
				else {
					#ifdef POLYPLUG_DEBUG
					ESP_LOGW(TAG, "Failed to parse integer from '%s'", buf);
					#endif
				}
				
				esp_mqtt_client_publish(mqtt_client, topic_ack, "PolyCast5MQTTRxSuccess", 0, 0, 0);
			}
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

void wifi_funcs_mqtt_client_destroy(void)
{
	if (mqtt_client) {
		esp_mqtt_client_stop(mqtt_client);
		esp_mqtt_client_destroy(mqtt_client);
		mqtt_client = NULL;
	}
}

void wifi_funcs_mqtt_client_stop(void)
{
	esp_mqtt_client_stop(mqtt_client);
}

void wifi_funcs_mqtt_client_start(void)
{
	esp_mqtt_client_start(mqtt_client);
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
	// Make sure there is something to connect to
	wifi_config_t cfg;
	ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &cfg));
	if (cfg.sta.ssid[0] == '\0') {
		return ESP_OK;
	}
	
	esp_err_t err = ESP_OK;
	
	if (!connected_to_network && !using_espnow) {
		xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
		
		err = esp_wifi_connect();
		
		// Check connetion
		if (wait_for_connection(pdMS_TO_TICKS(15000))) {
			#ifdef POLYPLUG_DEBUG
			ESP_LOGI(TAG, "Wi-Fi connected and got IP!");
			#endif
			
			wifi_funcs_get_current_date_time();
	
			esp_mqtt_client_start(mqtt_client); // Start MQTT client
			connected_to_network = true;
		}
		else {
			ESP_LOGE(TAG, "Failed to connect");
			connected_to_network = false;
		}
	}
	return err;
}

esp_err_t wifi_funcs_set_config(const char *ssid, const uint8_t* bssid, const char *password)
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
	//	 bssid[0], bssid[1], bssid[2],
	//	 bssid[3], bssid[4], bssid[5]);
	ESP_LOGI(TAG, "Setting Wi-Fi config password='%s'", password);
	#endif
	
	// Set config
	esp_err_t err = esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
	if (err != ESP_OK) {
		return err;
	}
	
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

esp_err_t wifi_funcs_wifi_disconnect(void)
{	
	// Called only by ESP-NOW before init-ing ESP-NOW
	using_espnow = true;
	
	// Disconnect
	esp_mqtt_client_stop(mqtt_client); // Stop possible client
	esp_err_t err = esp_wifi_disconnect(); // Disconnect if connected
	
	return err;
}

void wifi_funcs_mac_nvs_save(const char *key)
{
	nvs_handle_t handle;
	esp_err_t err;

	// Open NVS namespace in RW mode
	err = nvs_open(MQTT_NS, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
		return;
	}

	// Write or overwrite the string under key
	err = nvs_set_str(handle, MQTT_NS_KEY, key);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_str failed (%s)", esp_err_to_name(err));
		nvs_close(handle);
		return;
	}

	// Commit to flash
	err = nvs_commit(handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_commit failed (%s)", esp_err_to_name(err));
	}
	else {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Key saved to NVS: %s", key);
		#endif
		
		// Copy into global for use
		strlcpy(mqtt_topic_key_str, key, sizeof(mqtt_topic_key_str));
	}

	// Close NVS
	nvs_close(handle);
}

void wifi_funcs_mac_nvs_load(void)
{
	nvs_handle_t handle;
	
	// Open NVS
	nvs_open(MQTT_NS, NVS_READONLY, &handle);
	
	// Retreive string
	size_t len = sizeof(mqtt_topic_key_str);
	if (nvs_get_str(handle, MQTT_NS_KEY, mqtt_topic_key_str, &len) == ESP_OK) {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGI(TAG, "Loaded key: %s", mqtt_topic_key_str);
		#endif
	}
	else {
		#ifdef POLYPLUG_DEBUG
		ESP_LOGW(TAG, "Failed to load key");
		#endif
	}
	
	// Close NVS
	nvs_close(handle);
}
