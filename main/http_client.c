#include "http_client.h"
#include "xml_parse.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>

static const char TAG[] = "http_client";

static const char* conversion_api_url= CONFIG_CONVERSION_API_URL;

// Forward declarations for external globals (defined in main.c)
extern char* image_urls[MAX_IMAGES];
extern char* image_names[MAX_IMAGES];
extern int active_image_count;

// Local static arrays initialized from macros
static const char* static_image_urls[STATIC_IMAGE_COUNT] = STATIC_IMAGE_URLS_INIT;
static const char* static_image_names[STATIC_IMAGE_COUNT] = STATIC_IMAGE_NAMES_INIT;


// Helper function to check if URL is one of the Atlantic outlook images that should be cropped
static bool is_outlook_image(const char* url) {
    if (url == NULL) {
        return false;
    }
    
    for (int i = 0; i < STATIC_IMAGE_COUNT; i++) {
        if (strcmp(url, static_image_urls[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Forward declarations for image management (implemented in main.c)
esp_err_t get_image_buffer_info(int image_index, char **buffer, size_t *buffer_size, size_t *buffer_allocated);
void set_image_buffer_info(int image_index, char *buffer, size_t buffer_size, size_t buffer_allocated, bool is_valid);
void reset_image_buffer(int image_index);

// Generic HTTP event handler that can be used for both XML and image downloads
static esp_err_t generic_http_event_handler(esp_http_client_event_t *evt)
{
    http_download_t *download = (http_download_t*)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "HTTP error occurred");
            break;
        case HTTP_EVENT_ON_HEADER:
            // Pre-allocate buffer based on content length
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                size_t content_length = atoi(evt->header_value);
                if (content_length > 0 && download->buffer == NULL) {
                    download->buffer = malloc(content_length + 1024); // +1024 for safety
                    download->buffer_allocated = content_length + 1024;
                    if (download->buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate %zu bytes for HTTP response", content_length + 1024);
                        return ESP_FAIL;
                    }
                    download->buffer_size = 0;
                }
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && download->buffer != NULL) {
                // Ensure we don't overflow the buffer
                if (download->buffer_size + evt->data_len < download->buffer_allocated) {
                    memcpy(download->buffer + download->buffer_size, evt->data, evt->data_len);
                    download->buffer_size += evt->data_len;
                    if (download->buffer_allocated > download->buffer_size) {
                        download->buffer[download->buffer_size] = '\0'; // Null terminate if space allows
                    }
                }
            }
            break;
        case HTTP_EVENT_ON_CONNECTED:
        case HTTP_EVENT_HEADER_SENT:
        case HTTP_EVENT_ON_FINISH:
        case HTTP_EVENT_DISCONNECTED:
        case HTTP_EVENT_REDIRECT:
            // Reduce logging noise - only log errors and important events
            break;
    }
    return ESP_OK;
}

// Image-specific HTTP event handler (for compatibility with existing code)
static esp_err_t image_http_event_handler(esp_http_client_event_t *evt)
{
    // Get the image index from user_data
    int *image_index_ptr = (int*)evt->user_data;
    int image_index = (image_index_ptr != NULL) ? *image_index_ptr : 0;
    
    if (image_index < 0 || image_index >= MAX_IMAGES) {
        ESP_LOGE(TAG, "Invalid image index: %d", image_index);
        return ESP_FAIL;
    }
    
    char *buffer = NULL;
    size_t buffer_size = 0, buffer_allocated = 0;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR for image %d", image_index);
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED for image %d", image_index);
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT for image %d", image_index);
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER for image %d, key=%s, value=%s", image_index, evt->header_key, evt->header_value);
            // If we get the content-length header, we can pre-allocate the buffer
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                size_t content_length = atoi(evt->header_value);
                get_image_buffer_info(image_index, &buffer, &buffer_size, &buffer_allocated);
                if (content_length > 0 && buffer == NULL) {
                    ESP_LOGI(TAG, "Pre-allocating download buffer for image %d with %zu bytes", image_index, content_length);
                    // Add some extra space to be safe
                    buffer = malloc(content_length + 1024);
                    buffer_allocated = content_length + 1024;
                    if (buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory for download buffer of size %zu", content_length + 1024);
                        return ESP_FAIL;
                    }
                    buffer_size = 0;
                    set_image_buffer_info(image_index, buffer, buffer_size, buffer_allocated, false);
                    ESP_LOGI(TAG, "Successfully allocated download buffer for image %d", image_index);
                } else {
                    ESP_LOGE(TAG, "Content-Length header missing or invalid for image %d", image_index);
                    return ESP_FAIL;
                }
            }
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA for image %d, len=%d", image_index, evt->data_len);
            if (evt->data_len > 0) {
                get_image_buffer_info(image_index, &buffer, &buffer_size, &buffer_allocated);
                if (buffer != NULL) {
                    // Copy new data to buffer
                    memcpy(buffer + buffer_size, evt->data, evt->data_len);
                    buffer_size += evt->data_len;
                    set_image_buffer_info(image_index, buffer, buffer_size, buffer_allocated, false);
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH for image %d", image_index);
            get_image_buffer_info(image_index, &buffer, &buffer_size, &buffer_allocated);
            if (buffer != NULL) {
                ESP_LOGI(TAG, "Download complete for image %d: %zu bytes in buffer", image_index, buffer_size);
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED for image %d", image_index);
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT for image %d", image_index);
            break;
    }
    return ESP_OK;
}

esp_err_t http_download_xml_feed(const char* url, http_download_t* result)
{
    if (url == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Downloading XML feed from: %s", url);
    
    // Initialize result structure
    memset(result, 0, sizeof(http_download_t));
    
    // Configure HTTP client to download XML
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = generic_http_event_handler,
        .user_data = result,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for XML download");
        return ESP_FAIL;
    }
    
    // Perform HTTP GET request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (err == ESP_OK && status_code == 200 && result->buffer != NULL && result->buffer_size > 0) {
        ESP_LOGI(TAG, "XML download successful: %zu bytes", result->buffer_size);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "XML download failed: err=%s, status=%d", esp_err_to_name(err), status_code);
        http_download_free(result);
        return ESP_FAIL;
    }
}

esp_err_t http_download_image(int image_index)
{
    if (image_index < 0 || image_index >= MAX_IMAGES) {
        ESP_LOGE(TAG, "Invalid image index: %d", image_index);
        return ESP_FAIL;
    }
    
    // Reset download buffer before downloading
    reset_image_buffer(image_index);
    
    // Print available memory info for debugging
    ESP_LOGI(TAG, "Available heap: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    
    esp_err_t err;
    int status_code = 0;
    esp_http_client_handle_t client = NULL;
    
    // Using the conversion API to convert and download the NHC image
    ESP_LOGI(TAG, "Using conversion API to convert image %d from: %s", image_index, image_urls[image_index]);
        
    const char *post_data_format;
    if (is_outlook_image(image_urls[image_index])) {
        ESP_LOGI(TAG, "Adding crop parameters for Atlantic outlook image %d", image_index);
        post_data_format = 
            "{"
            "\"url\": \"%s\","
            "\"cf\": \"RGB565\","  // RGB565 format to match lvgl init
            "\"dither\": \"true\","  // Dithering
            "\"output\": \"bin\","    // Binary output format
            "\"bigEndian\": false,"
            "\"maxSize\": \"800x420\","  // TODO: make this configurable
            "\"crop\": {\"top\": 65, \"bottom\": 70}"
            "}";
    } else {
        ESP_LOGI(TAG, "Adding crop parameters for forecast cone %d", image_index);
        post_data_format = 
            "{"
            "\"url\": \"%s\","
            "\"cf\": \"RGB565\","  // RGB565 format to match lvgl init
            "\"dither\": \"true\","  // Dithering
            "\"output\": \"bin\","    // Binary output format
            "\"bigEndian\": false,"
            "\"maxSize\": \"800x420\","
            "\"crop\": {\"top\": 50, \"bottom\": 40, \"left\": 7, \"right\": 7}"
            "}";
    }
    
    // Calculate required buffer size and allocate
    size_t url_len = strlen(image_urls[image_index]);
    size_t post_data_len = strlen(post_data_format) + url_len - 2; // -2 for %s
    char *post_data = malloc(post_data_len + 1);
    if (post_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for POST data");
        return ESP_ERR_NO_MEM;
    }
    
    // Format the POST data with the URL
    snprintf(post_data, post_data_len + 1, post_data_format, image_urls[image_index]);
    
    // Configure HTTP client for POST request to conversion API
    esp_http_client_config_t config = {
        .url = conversion_api_url,
        .event_handler = image_http_event_handler,
        .user_data = &image_index,  // Pass image index to event handler
        .buffer_size = MAX_HTTP_RECV_BUFFER,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use certificate bundle
    };

    client = esp_http_client_init(&config);
    
    // Set POST method and headers
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    ESP_LOGI(TAG, "Sending conversion request to API for image %d...", image_index);
    err = esp_http_client_perform(client);
    
    // Capture status code before cleanup
    status_code = esp_http_client_get_status_code(client);
    
    // Free allocated memory
    free(post_data);
    esp_http_client_cleanup(client);
    client = NULL;
   
    if (err == ESP_OK) {
        char *buffer = NULL;
        size_t buffer_size = 0, buffer_allocated = 0;
        get_image_buffer_info(image_index, &buffer, &buffer_size, &buffer_allocated);
        
        ESP_LOGI(TAG, "HTTP GET Status = %d, download size = %zu bytes for image %d",
                status_code, buffer_size, image_index);
        
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP request returned non-200 status code: %d for image %d", status_code, image_index);
            err = ESP_FAIL;
        } else if (buffer != NULL && buffer_size > 0) {
            ESP_LOGI(TAG, "Downloaded data size: %zu bytes for image %d", buffer_size, image_index);
            
            // Check PSRAM usage after download
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
            size_t psram_used = psram_total - psram_free;
            
            ESP_LOGI(TAG, "PSRAM after image %d download: %zu/%zu bytes used (%.1f%% used, %.1f%% free)", 
                     image_index, psram_used, psram_total, 
                     (psram_used * 100.0) / psram_total,
                     (psram_free * 100.0) / psram_total);
            
            // Verify the downloaded data is large enough
            // For a simple validation, check if the buffer is at least large enough
            // to contain a minimal header (8 bytes) plus some image data
            if (buffer_size < 100) { // Arbitrary small threshold
                ESP_LOGW(TAG, "Downloaded data seems too small for an image (%zu bytes) for image %d", 
                         buffer_size, image_index);
                err = ESP_FAIL;
            } else {
                set_image_buffer_info(image_index, buffer, buffer_size, buffer_allocated, true);
            }
            
        } else {
            ESP_LOGE(TAG, "Failed to download image %d or image is empty", image_index);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed for image %d: %s", image_index, esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t http_download_all_images(void)
{
    int successful_downloads = 0;
    
    if (active_image_count == 0) {
        ESP_LOGW(TAG, "No active images to download");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Downloading all %d active images...", active_image_count);
    
    for (int i = 0; i < active_image_count; i++) {
        if (image_urls[i] == NULL) {
            ESP_LOGW(TAG, "Skipping image %d - no URL available", i);
            continue;
        }
        
        ESP_LOGI(TAG, "Downloading image %d of %d...", i + 1, active_image_count);
        
        if (http_download_image(i) == ESP_OK) {
            successful_downloads++;
            ESP_LOGI(TAG, "Successfully downloaded image %d", i);
        } else {
            ESP_LOGW(TAG, "Failed to download image %d", i);
        }
    }
    
    ESP_LOGI(TAG, "Download complete: %d of %d images downloaded successfully", 
             successful_downloads, active_image_count);
    
    return (successful_downloads > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t http_update_image_urls_from_xml(void)
{
    ESP_LOGI(TAG, "Downloading NHC XML feed from: %s", NHC_XML_FEED_URL);
    
    http_download_t xml_response = {0};
    esp_err_t err = http_download_xml_feed(NHC_XML_FEED_URL, &xml_response);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "XML download successful: %zu bytes", xml_response.buffer_size);
        
        // Clean up old URLs
        http_cleanup_image_urls();
        
        // Always start with Atlantic outlook images
        int current_index = 0;
        int static_to_use = (STATIC_IMAGE_COUNT > MAX_IMAGES) ? MAX_IMAGES : STATIC_IMAGE_COUNT;
        
        // First, add the Atlantic outlook images
        for (int i = 0; i < static_to_use && current_index < MAX_IMAGES; i++) {
            image_urls[current_index] = strdup(static_image_urls[i]);
            image_names[current_index] = strdup(static_image_names[i]);
            ESP_LOGI(TAG, "Added Atlantic outlook image %d: %s", current_index, static_image_names[i]);
            current_index++;
        }
        
        // Parse XML to extract cone image URLs
        int cone_count = 0;
        char** cone_urls = xml_parse_all_cone_image_urls(xml_response.buffer, xml_response.buffer_size, &cone_count);
        
        if (cone_urls && cone_count > 0) {
            ESP_LOGI(TAG, "Found %d cone image URLs in XML", cone_count);
            
            // Add cone images after the Atlantic outlook images (space permitting)
            int cones_to_add = cone_count;
            if (current_index + cones_to_add > MAX_IMAGES) {
                cones_to_add = MAX_IMAGES - current_index;
                ESP_LOGW(TAG, "Limited to %d cone images due to MAX_IMAGES constraint", cones_to_add);
            }
            
            for (int i = 0; i < cones_to_add; i++) {
                image_urls[current_index] = strdup(cone_urls[i]);
                
                // Create a generic storm name - in a real implementation you'd extract from XML
                char temp_name[64];
                snprintf(temp_name, sizeof(temp_name), "Hurricane Cone %d", i + 1);
                image_names[current_index] = strdup(temp_name);
                
                ESP_LOGI(TAG, "Added cone image %d: %s", current_index, cone_urls[i]);
                current_index++;
            }
            
            // Clean up parsed URLs
            xml_parse_free_urls(cone_urls, cone_count);
            
        } else {
            ESP_LOGW(TAG, "No cone image URLs found in XML, only Atlantic outlook images will be displayed");
        }
        
        active_image_count = current_index;
        ESP_LOGI(TAG, "Total images configured: %d (Atlantic outlook + cone images)", active_image_count);
        err = ESP_OK;
        
    } else {
        ESP_LOGE(TAG, "XML download failed, using static URLs");
        
        // Clean up and use static URLs
        http_cleanup_image_urls();
        int static_to_use = (STATIC_IMAGE_COUNT > MAX_IMAGES) ? MAX_IMAGES : STATIC_IMAGE_COUNT;
        for (int i = 0; i < static_to_use; i++) {
            image_urls[i] = strdup(static_image_urls[i]);
            image_names[i] = strdup(static_image_names[i]);
        }
        active_image_count = static_to_use;
        
        err = ESP_OK; // Still return success so we continue with static URLs
    }
    
    // Clean up XML response buffer
    http_download_free(&xml_response);
    
    ESP_LOGI(TAG, "Image URL update complete: %d active images", active_image_count);
    return err;
}

void http_cleanup_image_urls(void)
{
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (image_urls[i] != NULL) {
            free(image_urls[i]);
            image_urls[i] = NULL;
        }
        if (image_names[i] != NULL) {
            free(image_names[i]);
            image_names[i] = NULL;
        }
    }
    active_image_count = 0;
}

void http_download_free(http_download_t* download)
{
    if (download != NULL && download->buffer != NULL) {
        free(download->buffer);
        download->buffer = NULL;
        download->buffer_size = 0;
        download->buffer_allocated = 0;
    }
}
