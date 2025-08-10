#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize time synchronization using WorldTimeAPI
 * 
 * This function attempts to get the current time from WorldTimeAPI and set the system time.
 * It will retry up to 3 times on failure.
 * 
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t initialize_worldtime_api(void);

/**
 * @brief Initialize time synchronization using SNTP
 * 
 * This function initializes SNTP client and synchronizes system time with NTP servers.
 * It will wait up to 20 seconds for time synchronization to complete.
 * 
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t initialize_sntp(void);

#ifdef __cplusplus
}
#endif
