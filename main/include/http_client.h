#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// HTTP download result structure
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t buffer_allocated;
} http_download_t;

/**
 * @brief Download XML feed from NHC website
 * 
 * @param url The URL to download from
 * @param result Pointer to http_download_t structure to store result
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t http_download_xml_feed(const char* url, http_download_t* result);

/**
 * @brief Download a single image using the conversion API
 * 
 * @param image_index Index of the image in the global image array (0-9)
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t http_download_image(int image_index);

/**
 * @brief Download all configured images
 * 
 * @return ESP_OK if at least one image downloaded successfully, ESP_FAIL otherwise
 */
esp_err_t http_download_all_images(void);

/**
 * @brief Update image URLs by downloading and parsing NHC XML feed
 * 
 * Downloads the NHC XML feed, parses it for hurricane cone images,
 * and updates the global image_urls array with Atlantic outlook + cone images.
 * 
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t http_update_image_urls_from_xml(void);

/**
 * @brief Clean up dynamically allocated image URLs
 * 
 * Frees all dynamically allocated image URLs and names in the global arrays.
 */
void http_cleanup_image_urls(void);

/**
 * @brief Free an http_download_t result structure
 * 
 * @param download Pointer to the structure to free
 */
void http_download_free(http_download_t* download);

#ifdef __cplusplus
}
#endif
