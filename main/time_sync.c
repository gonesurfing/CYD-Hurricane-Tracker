#include "time_sync.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char TAG[] = "time_sync";

// Generic HTTP response structure for time synchronization downloads
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t buffer_allocated;
} http_response_t;

static esp_err_t worldtime_http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t*)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "WorldTimeAPI HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "WorldTimeAPI HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "WorldTimeAPI HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "WorldTimeAPI HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            // Pre-allocate buffer based on content length
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                size_t content_length = atoi(evt->header_value);
                if (content_length > 0 && response->buffer == NULL) {
                    response->buffer = malloc(content_length + 1); // +1 for null terminator
                    response->buffer_allocated = content_length + 1;
                    if (response->buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory for WorldTimeAPI response");
                        return ESP_FAIL;
                    }
                    response->buffer_size = 0;
                }
            }
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "WorldTimeAPI HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (evt->data_len > 0 && response->buffer != NULL) {
                // Ensure we don't overflow the buffer
                if (response->buffer_size + evt->data_len < response->buffer_allocated) {
                    memcpy(response->buffer + response->buffer_size, evt->data, evt->data_len);
                    response->buffer_size += evt->data_len;
                    response->buffer[response->buffer_size] = '\0'; // Null terminate
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "WorldTimeAPI HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WorldTimeAPI HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "WorldTimeAPI HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

// SNTP (Simple Network Time Protocol) functions for time synchronization
static void sntp_time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized with SNTP server");
}

// WorldTimeAPI time synchronization function
esp_err_t initialize_worldtime_api(void)
{
    ESP_LOGI(TAG, "Initializing time synchronization via WorldTimeAPI");
    
    // Set timezone to UTC (same as SNTP version)
    setenv("TZ", TIMEZONE_CONFIG, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", TIMEZONE_CONFIG);
    
    http_response_t response = {0};
    esp_err_t err = ESP_FAIL;
    int retry = 0;
    const int retry_count = 10;
    
    while (retry < retry_count) {
        ESP_LOGI(TAG, "Attempting to get time from WorldTimeAPI... (%d/%d)", retry + 1, retry_count);
        
        // Reset response buffer for each retry
        if (response.buffer != NULL) {
            free(response.buffer);
            response.buffer = NULL;
        }
        response.buffer_size = 0;
        response.buffer_allocated = 0;
        
        // Configure HTTP client
        esp_http_client_config_t config = {
            .url = "http://worldtimeapi.org/api/timezone/America/New_York",
            .event_handler = worldtime_http_event_handler,
            .user_data = &response,
            .timeout_ms = 10000,
        };
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client for WorldTimeAPI");
            retry++;
            continue;
        }
        
        // Perform HTTP GET request
        err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        
        if (err == ESP_OK && status_code == 200 && response.buffer != NULL && response.buffer_size > 0) {
            ESP_LOGI(TAG, "WorldTimeAPI response received: %d bytes", response.buffer_size);
            ESP_LOGD(TAG, "WorldTimeAPI JSON response: %s", response.buffer);
            
            // Parse JSON response
            cJSON *json = cJSON_Parse(response.buffer);
            if (json == NULL) {
                ESP_LOGE(TAG, "Failed to parse WorldTimeAPI JSON response");
                retry++;
                continue;
            }
            
            // Extract unixtime field
            cJSON *unixtime_json = cJSON_GetObjectItem(json, "unixtime");
            if (unixtime_json == NULL || !cJSON_IsNumber(unixtime_json)) {
                ESP_LOGE(TAG, "Failed to find 'unixtime' field in WorldTimeAPI response");
                cJSON_Delete(json);
                retry++;
                continue;
            }
            
            // Get the unix timestamp
            time_t unix_time = (time_t)cJSON_GetNumberValue(unixtime_json);
            ESP_LOGI(TAG, "Extracted unix timestamp: %ld", unix_time);
            
            // Set system time
            struct timeval tv = {
                .tv_sec = unix_time,
                .tv_usec = 0
            };
            
            if (settimeofday(&tv, NULL) == 0) {
                ESP_LOGI(TAG, "System time set successfully via WorldTimeAPI");
                
                // Verify the time was set correctly
                time_t now;
                struct tm timeinfo;
                time(&now);
                localtime_r(&now, &timeinfo);
                
                char strftime_buf[64];
                strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
                ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
                
                err = ESP_OK;
                cJSON_Delete(json);
                break;
            } else {
                ESP_LOGE(TAG, "Failed to set system time");
                cJSON_Delete(json);
                retry++;
                continue;
            }
        } else {
            ESP_LOGE(TAG, "WorldTimeAPI request failed: err=%s, status=%d", esp_err_to_name(err), status_code);
            retry++;
        }
        
        // Wait before retry
        if (retry < retry_count) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    
    // Clean up response buffer
    if (response.buffer != NULL) {
        free(response.buffer);
    }
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to synchronize time with WorldTimeAPI after %d attempts", retry_count);
    }
    
    return err;
}

esp_err_t initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    
    // Set timezone using the configurable setting
    setenv("TZ", TIMEZONE_CONFIG, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", TIMEZONE_CONFIG);
    
    // Initialize SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");  // Backup server
    esp_sntp_set_time_sync_notification_cb(sntp_time_sync_notification_cb);
    esp_sntp_init();
    
    // Wait for time to be set (with timeout)
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    if (retry >= retry_count) {
        ESP_LOGW(TAG, "Failed to synchronize time with SNTP server");
        return ESP_FAIL;
    }
    
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
    
    return ESP_OK;
}
