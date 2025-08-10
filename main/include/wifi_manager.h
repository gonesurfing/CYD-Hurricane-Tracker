#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in station mode and connect to configured access point
 * 
 * This function initializes the WiFi subsystem, configures it in station mode,
 * and attempts to connect to the access point specified in menuconfig.
 * It will retry up to MAXIMUM_RETRY times before giving up.
 * 
 * @return ESP_OK on successful connection, ESP_FAIL on connection failure
 */
esp_err_t wifi_init_sta(void);

#ifdef __cplusplus
}
#endif
