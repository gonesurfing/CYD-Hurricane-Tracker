/*
 * Hurricane Tracking App for ESP32-S3
 * 
 * This application downloads hurricane tracking images from NHC website and displays them.
 * 
 * For offline development and testing, the image can be pre-converted to C array format 
 * using the following curl command:
 * 
 * curl -X POST http://127.0.0.1:8080/convert \
 * -H "Content-Type: application/json" \
 * -d '{
 *   "url": "https://www.nhc.noaa.gov/xgtwo/two_atl_7d0.png",
 *   "cf": "RGB565A8",
 *   "output": "bin",
 *   "maxSize": "800x480"
 * }' --output test.bin
 * 
 * The custom binary format from the conversion API consists of:
 * 1. A 12-byte header containing:
 *    - byte 0: magic number (0x19 for LVGL v9)
 *    - byte 1: color format
 *    - bytes 2-3: flags (16-bit)
 *    - bytes 4-5: width (16-bit)
 *    - bytes 6-7: height (16-bit)
 *    - bytes 8-9: stride (16-bit)
 *    - bytes 10-11: reserved (16-bit)
 * 2. Followed by the raw pixel data in the specified format
 *    - For RGB565, each pixel is 2 bytes
 *    - Data is stored row by row (all pixels in first row, then second row, etc.)
 */

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"

#include "bsp.h"
#include "esp_lvgl_port.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#include "app_config.h"
#include "touch_init.h"
#include "lcd_init.h"
#include "xml_parse.h"
#include "time_sync.h"
#include "wifi_manager.h"
#include "http_client.h"

// Include the pre-converted image data
extern const lv_image_dsc_t error_image;

static esp_lcd_panel_handle_t lcd_panel = NULL;

static i2c_master_bus_handle_t my_bus = NULL;
static esp_lcd_panel_io_handle_t touch_io_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

/* Backlight control */
static TimerHandle_t backlight_timer = NULL;
static bool backlight_on = true;
static SemaphoreHandle_t backlight_mutex = NULL;

#if ENABLE_PIR_BACKLIGHT_TIMER
static SemaphoreHandle_t pir_motion_semaphore = NULL;
#endif

static const char TAG[] = APP_NAME;

// Globals accessed by http_client module
char* image_urls[MAX_IMAGES] = {NULL}; // Dynamic array of URLs
char* image_names[MAX_IMAGES] = {NULL}; // Dynamic array of storm names
int active_image_count = 0; // Number of active storm images

// Function to check if current time matches NHC update times
static bool is_nhc_update_time(void) {
    static const char* nhc_update_times[NHC_UPDATE_TIMES_COUNT] = NHC_UPDATE_TIMES;
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo);  // Use UTC time
    
    char current_time[6];
    strftime(current_time, sizeof(current_time), "%H:%M", &timeinfo);
    
    for (int i = 0; i < NHC_UPDATE_TIMES_COUNT; i++) {
        if (strcmp(current_time, nhc_update_times[i]) == 0) {
            ESP_LOGI(TAG, "Current time %s matches NHC update time", current_time);
            return true;
        }
    }
    
    return false;
}

// Structure to hold multiple downloaded images
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t buffer_allocated;
    lv_img_dsc_t img_dsc;
    bool is_valid;
    time_t download_timestamp;  // When this image was downloaded/processed
} image_data_t;

static image_data_t s_images[MAX_IMAGES];
static int s_current_image_index = 0;
static TimerHandle_t s_image_cycle_timer = NULL;

// Image buffer management functions (called from http_client.c)
esp_err_t get_image_buffer_info(int image_index, char **buffer, size_t *buffer_size, size_t *buffer_allocated)
{
    if (image_index < 0 || image_index >= MAX_IMAGES) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (buffer) *buffer = s_images[image_index].buffer;
    if (buffer_size) *buffer_size = s_images[image_index].buffer_size;
    if (buffer_allocated) *buffer_allocated = s_images[image_index].buffer_allocated;
    
    return ESP_OK;
}

void set_image_buffer_info(int image_index, char *buffer, size_t buffer_size, size_t buffer_allocated, bool is_valid)
{
    if (image_index >= 0 && image_index < MAX_IMAGES) {
        s_images[image_index].buffer = buffer;
        s_images[image_index].buffer_size = buffer_size;
        s_images[image_index].buffer_allocated = buffer_allocated;
        s_images[image_index].is_valid = is_valid;
    }
}

void reset_image_buffer(int image_index)
{
    if (image_index >= 0 && image_index < MAX_IMAGES) {
        if (s_images[image_index].buffer != NULL) {
            free(s_images[image_index].buffer);
        }
        s_images[image_index].buffer = NULL;
        s_images[image_index].buffer_size = 0;
        s_images[image_index].buffer_allocated = 0;
        s_images[image_index].is_valid = false;
        s_images[image_index].download_timestamp = 0;
    }
}

/**
 * @brief Get the backlight state safely
 * 
 * @return true if backlight is on, false if off
 */
static bool get_backlight_state(void)
{
    bool state = false;
    if (xSemaphoreTake(backlight_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = backlight_on;
        xSemaphoreGive(backlight_mutex);
    }
    return state;
}

/**
 * @brief Set the backlight state (on/off) with mutex protection
 * 
 * @param state true to turn on, false to turn off
 */
static void set_backlight_state(bool state)
{
    if (xSemaphoreTake(backlight_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        gpio_set_level(BSP_LCD_GPIO_BK_LIGHT, state ? BSP_LCD_BK_LIGHT_ON_LEVEL : !BSP_LCD_BK_LIGHT_ON_LEVEL);
        backlight_on = state;
        xSemaphoreGive(backlight_mutex);
        ESP_LOGI(TAG, "Backlight %s", state ? "ON" : "OFF");
    }
}

/**
 * @brief Backlight timer callback with different behavior based on configuration
 * This runs only once per timeout and uses minimal stack
 */
static void backlight_timer_callback(TimerHandle_t timer)
{
    // Use protected access instead of direct global access
    if (get_backlight_state()) {
#if ENABLE_TOUCHSCREEN || ENABLE_PIR_BACKLIGHT_TIMER
        // Normal behavior: turn off backlight when there's a way to turn it back on
        ESP_LOGI(TAG, "Backlight timer expired, turning off backlight");
        set_backlight_state(false);
#else
        // No input method available - keep backlight on permanently
        ESP_LOGI(TAG, "Backlight timer expired, but no input method available - keeping backlight on");
        // Reset the timer to fire again (effectively keeping backlight on)
        xTimerReset(timer, 0);
#endif
    }
}

#if ENABLE_PIR_BACKLIGHT_TIMER
/**
 * @brief PIR sensor interrupt handler (for non-touchscreen version)
 * This is called from interrupt context, so it must be in IRAM and use minimal stack
 */
static void IRAM_ATTR pir_sensor_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    // Signal the semaphore from ISR instead of setting a flag
    if (pir_motion_semaphore != NULL) {
        xSemaphoreGiveFromISR(pir_motion_semaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
#endif

/**
 * @brief Touch event callback to track user activity
 */
#if ENABLE_TOUCHSCREEN
static void touch_event_cb(lv_event_t *e)
{    
    // Use protected access
    if (!get_backlight_state()) {
        set_backlight_state(true);
    }
    
    // Reset and restart the one-shot timer
    if (backlight_timer != NULL) {
        xTimerReset(backlight_timer, 0);
    }
}
#endif





// Function to initialize an LVGL image descriptor from the downloaded buffer
static esp_err_t process_downloaded_image(int image_index)
{
    if (image_index < 0 || image_index >= MAX_IMAGES) {
        ESP_LOGE(TAG, "Invalid image index: %d", image_index);
        return ESP_FAIL;
    }
    
    image_data_t *img_data = &s_images[image_index];
    
    if (img_data->buffer != NULL && img_data->buffer_size > 0) {
        ESP_LOGI(TAG, "Processing downloaded image %d", image_index);
        
        // Check if the buffer is large enough to contain at least a valid header
        if (img_data->buffer_size < 8) {
            ESP_LOGE(TAG, "Downloaded buffer too small to be a valid image for image %d", image_index);
            return ESP_FAIL;
        }
        
        // Set up the image descriptor using the downloaded buffer
        // Your API uses this custom binary format:
        // byte 0: magic number (0x19 for LVGL v9)
        // byte 1: color format
        // bytes 2-3: flags (16-bit)
        // bytes 4-5: width (16-bit)
        // bytes 6-7: height (16-bit)
        // bytes 8-9: stride (16-bit)
        // bytes 10-11: reserved (16-bit)
        // Total header size: 12 bytes
        
        if (img_data->buffer_size < 12) {
            ESP_LOGE(TAG, "Downloaded buffer too small for custom header format for image %d", image_index);
            return ESP_FAIL;
        }
        
        // Parse the custom header format
        uint8_t magic = img_data->buffer[0];
        uint8_t color_format = img_data->buffer[1];
        uint16_t flags = *(uint16_t*)(img_data->buffer + 2);
        uint16_t width = *(uint16_t*)(img_data->buffer + 4);
        uint16_t height = *(uint16_t*)(img_data->buffer + 6);
        uint16_t stride = *(uint16_t*)(img_data->buffer + 8);
        uint16_t reserved = *(uint16_t*)(img_data->buffer + 10);
        
        ESP_LOGI(TAG, "Parsed custom header for image %d: magic=0x%02x, cf=%d, flags=%d, w=%d, h=%d, stride=%d", 
                 image_index, magic, color_format, flags, width, height, stride);
        
        // Verify magic number
        if (magic != 0x19) {
            ESP_LOGW(TAG, "Unexpected magic number: 0x%02x (expected 0x19) for image %d", magic, image_index);
        }
        
        img_data->img_dsc.header.w = width;
        img_data->img_dsc.header.h = height;
        
        // Check if the width and height seem reasonable
        if (img_data->img_dsc.header.w > 4096 || img_data->img_dsc.header.h > 4096 ||
            img_data->img_dsc.header.w == 0 || img_data->img_dsc.header.h == 0) {
            ESP_LOGW(TAG, "Image dimensions invalid: %dx%d for image %d", 
                    img_data->img_dsc.header.w, img_data->img_dsc.header.h, image_index);
            img_data->is_valid = false;
            return ESP_FAIL;
        }
        
        // Map the color format from your API to LVGL color format
        switch (color_format) {
            case 0x12: // RGB565
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                ESP_LOGI(TAG, "Using RGB565 format for image %d", image_index);
                break;
            case 0x0F: // RGB888
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
                ESP_LOGI(TAG, "Using RGB888 format for image %d", image_index);
                break;
            case 0x10: // ARGB8888
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
                ESP_LOGI(TAG, "Using ARGB8888 format for image %d", image_index);
                break;
            case 0x14: // RGB565A8 (RGB565 with alpha channel)
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
                ESP_LOGI(TAG, "Using RGB565A8 format for image %d", image_index);
                break;
            case 0x13: // ARGB8565
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_ARGB8565;
                ESP_LOGI(TAG, "Using ARGB8565 format for image %d", image_index);
                break;
            case 0x11: // XRGB8888
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
                ESP_LOGI(TAG, "Using XRGB8888 format for image %d", image_index);
                break;
            case 0x06: // L8 (grayscale)
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_L8;
                ESP_LOGI(TAG, "Using L8 format for image %d", image_index);
                break;
            default:
                // Default to RGB565 if format is unknown
                ESP_LOGW(TAG, "Unknown color format 0x%02x, defaulting to RGB565 for image %d", color_format, image_index);
                img_data->img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                break;
        }
        
        // Point to the pixel data after the 12-byte header
        img_data->img_dsc.data = (const uint8_t *)(img_data->buffer + 12);
        
        // Calculate the expected data size (width * height * bytes per pixel)
        size_t bytes_per_pixel;
        switch (img_data->img_dsc.header.cf) {
            case LV_COLOR_FORMAT_RGB565:
                bytes_per_pixel = 2;
                break;
            case LV_COLOR_FORMAT_RGB888:
                bytes_per_pixel = 3;
                break;
            case LV_COLOR_FORMAT_ARGB8888:
            case LV_COLOR_FORMAT_XRGB8888:
                bytes_per_pixel = 4;
                break;
            case LV_COLOR_FORMAT_RGB565A8:
                bytes_per_pixel = 3; // 2 bytes RGB565 + 1 byte alpha
                break;
            case LV_COLOR_FORMAT_ARGB8565:
                bytes_per_pixel = 3; // 1 byte alpha + 2 bytes RGB565
                break;
            case LV_COLOR_FORMAT_L8:
                bytes_per_pixel = 1;
                break;
            default:
                bytes_per_pixel = 2; // Default to 2 bytes per pixel
                break;
        }
        
        size_t expected_size = img_data->img_dsc.header.w * img_data->img_dsc.header.h * bytes_per_pixel;
        
        // Check if the buffer contains enough data for the image
        if (img_data->buffer_size < expected_size + 12) {
            ESP_LOGW(TAG, "Downloaded buffer size (%u) is smaller than expected for image %d (%u + 12 byte header)",
                     img_data->buffer_size, image_index, expected_size);
            
            img_data->is_valid = false;
            return ESP_FAIL;
        }
        
        img_data->img_dsc.data_size = expected_size;
        
        ESP_LOGI(TAG, "Created LVGL image %d: %dx%d, format: %d, data size: %u bytes",
                 image_index, img_data->img_dsc.header.w, img_data->img_dsc.header.h, 
                 img_data->img_dsc.header.cf, img_data->img_dsc.data_size);
        
        img_data->is_valid = true;
        
        // Store the timestamp when this image was successfully processed
        time(&img_data->download_timestamp);
        
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "No image data available for image %d", image_index);
        return ESP_FAIL;
    }
}


// Function to get the next valid image to display
static const lv_img_dsc_t* get_next_valid_image(void)
{
    int attempts = 0;
    int images_to_check = (active_image_count > 0) ? active_image_count : MAX_IMAGES;
    
    // Try to find the next valid image, with a maximum number of attempts
    while (attempts < images_to_check) {
        if (s_images[s_current_image_index].is_valid) {
            return &s_images[s_current_image_index].img_dsc;
        }
        
        // Move to next image (cycle only through active images)
        s_current_image_index = (s_current_image_index + 1) % images_to_check;
        attempts++;
    }
    
    // If no valid images found, return error image
    ESP_LOGW(TAG, "No valid images available, using error image");
    return &error_image;
}

// Add after the existing global variables
static const lv_img_dsc_t *s_current_display_image = NULL;
static TaskHandle_t s_display_task_handle = NULL;

// Lightweight timer callback that just signals the display task
static void image_cycle_timer_callback(TimerHandle_t timer)
{
    // Minimal stack usage - just notify the display task
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_display_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_display_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static esp_err_t app_lvgl_init(esp_lcd_panel_handle_t lp, esp_lcd_touch_handle_t tp,
                               lv_display_t **lv_disp, lv_indev_t **lv_touch_indev)
{
    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = LVGL_TASK_PRIORITY,
        .task_stack = LVGL_TASK_STACK_SIZE,
        .task_affinity = -1,
        .task_max_sleep_ms = LVGL_TASK_MAX_SLEEP_MS,
        .timer_period_ms = LVGL_TIMER_PERIOD_MS
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    uint32_t buff_size = BSP_LCD_H_RES * APP_LCD_DRAW_BUFF_HEIGHT;
#if APP_LCD_LVGL_FULL_REFRESH || APP_LCD_LVGL_DIRECT_MODE
    buff_size = BSP_LCD_H_RES * BSP_LCD_V_RES;
#endif

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = lp,
        .buffer_size = buff_size,
        .double_buffer = APP_LCD_DRAW_BUFF_DOUBLE,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = false,
#if APP_LCD_LVGL_FULL_REFRESH
            .full_refresh = true,
#elif APP_LCD_LVGL_DIRECT_MODE
            .direct_mode = true,
#endif
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
        }
    };
    
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
#if APP_LCD_RGB_BOUNCE_BUFFER_MODE
            .bb_mode = true,
#else
            .bb_mode = false,
#endif
#if APP_LCD_LVGL_AVOID_TEAR
            .avoid_tearing = true,
#else
            .avoid_tearing = false,
#endif
        }
    };
    
    *lv_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);

#if ENABLE_TOUCHSCREEN
    if (tp != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = *lv_disp,
            .handle = tp,
        };
        *lv_touch_indev = lvgl_port_add_touch(&touch_cfg);
        
        // Add touch event to the screen to track activity
        lv_obj_t *scr = lv_screen_active();
        lv_obj_add_event_cb(scr, touch_event_cb, LV_EVENT_PRESSED, NULL);
    } else {
        *lv_touch_indev = NULL;
    }
#else
    *lv_touch_indev = NULL;
#endif

    return ESP_OK;
}

// Simplified display function that uses the global pointer
static void display_image_from_global_pointer(void)
{
    if (s_current_display_image == NULL) {
        ESP_LOGW(TAG, "No image to display, using error image");
        s_current_display_image = &error_image;
    }
    
    lvgl_port_lock(0);
    
    // Clear the screen first
    lv_obj_clean(lv_screen_active());
    
    // First, set the default screen background to black
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    
    // Create a container for the image that fills the screen
    lv_obj_t *cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(cont, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    
#if ENABLE_TOUCHSCREEN
    // Add touch event callback
    lv_obj_add_event_cb(cont, touch_event_cb, LV_EVENT_PRESSED, NULL);
#endif
    
    // Explicitly remove ALL borders and outline
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_outline_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    
    // Disable scrolling and remove padding
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_pos(cont, 0, 0);
    
    // Create image object
    lv_obj_t *img_obj = lv_image_create(cont);
    
#if ENABLE_TOUCHSCREEN
    lv_obj_add_event_cb(img_obj, touch_event_cb, LV_EVENT_PRESSED, NULL);
#endif
    
    // Set the image source from global pointer
    lv_image_set_src(img_obj, s_current_display_image);
    
    // Configure image display
    lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_align(img_obj, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(img_obj, 0, 0);
    lv_obj_set_size(img_obj, BSP_LCD_H_RES, BSP_LCD_V_RES - 40);
    
    lv_obj_set_style_img_recolor_opa(img_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_img_opa(img_obj, LV_OPA_COVER, 0);
    
    // Also set the default screen style to have no borders
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    
    // Create a label for the last updated timestamp and image info
    lv_obj_t *timestamp = lv_label_create(cont);
    
    // Get timestamp info
    char time_str[128];
    time_t display_timestamp;
    struct tm timeinfo;
    
    // Find which image is currently being displayed
    int current_img_idx = -1;
    for (int i = 0; i < active_image_count; i++) {
        if (s_images[i].is_valid && s_current_display_image == &s_images[i].img_dsc) {
            current_img_idx = i;
            break;
        }
    }
    
    // Use the download timestamp from the image, or current time as fallback
    if (current_img_idx >= 0 && s_images[current_img_idx].download_timestamp > 0) {
        display_timestamp = s_images[current_img_idx].download_timestamp;
        ESP_LOGD(TAG, "Using stored timestamp for image %d", current_img_idx);
    } else {
        // Fallback to current time if no stored timestamp available (error image case)
        time(&display_timestamp);
        ESP_LOGD(TAG, "Using current time as fallback timestamp");
    }
    
    gmtime_r(&display_timestamp, &timeinfo);  // Use UTC time for consistency
    
    // Format timestamp with dynamic image name
    if (current_img_idx >= 0 && current_img_idx < active_image_count && image_names[current_img_idx] != NULL) {
        snprintf(time_str, sizeof(time_str), "%s\nLast updated: %04d-%02d-%02d %02d:%02d:%02d UTC", 
                    image_names[current_img_idx], 
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        // Fallback if no name is available
        snprintf(time_str, sizeof(time_str), "Hurricane Tracking Image\nLast updated: %04d-%02d-%02d %02d:%02d:%02d UTC", 
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    
    if (current_img_idx >= 0 || active_image_count == 0) {
    
        lv_label_set_text(timestamp, time_str);
        lv_obj_align(timestamp, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_text_color(timestamp, lv_color_white(), 0);
        lv_obj_set_style_text_align(timestamp, LV_TEXT_ALIGN_CENTER, 0);
    }
    lvgl_port_unlock();
}

// New display task that handles all LVGL operations
static void display_image_task(void *pvParameters)
{
    while (1) {
        // Wait for notification from timer or update task
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) > 0) {
            
            // Update the global pointer to the current valid image
            s_current_display_image = get_next_valid_image();
            
            ESP_LOGI(TAG, "Displaying image %d", s_current_image_index);
            
            // Now do the actual display work with the global pointer
            display_image_from_global_pointer();
            
            // Move to next image for next cycle (after displaying current one)
            int images_to_cycle = (active_image_count > 0) ? active_image_count : MAX_IMAGES;
            s_current_image_index = (s_current_image_index + 1) % images_to_cycle;
        }
    }
}

// Remove the old display_image function and replace the update task
static void update_image_task(void *pvParameters)
{
    // Flag to track if we've done an initial update
    bool initial_update_done = false;
    
    while (1) {
        bool should_update = false;
        
        if (!initial_update_done) {
            ESP_LOGI(TAG, "Performing initial image update...");
            should_update = true;
            initial_update_done = true;
        } else if (is_nhc_update_time()) {
            ESP_LOGI(TAG, "NHC update time reached, downloading images...");
            should_update = true;
        } else {
            ESP_LOGD(TAG, "Not NHC update time, skipping download");
        }
        
        if (should_update) {
            ESP_LOGI(TAG, "Starting image update cycle...");
            
            // First, update the image URLs from the NHC XML feed
            if (http_update_image_urls_from_xml() == ESP_OK) {
                ESP_LOGI(TAG, "Successfully updated image URLs from XML feed");
            } else {
                ESP_LOGW(TAG, "Failed to update URLs from XML, using current URLs");
            }
            
            // Now download images using the updated URLs
            ESP_LOGI(TAG, "Downloading %d images...", active_image_count);
            
            if (http_download_all_images() == ESP_OK) {
                ESP_LOGI(TAG, "Processing downloaded images...");
                int processed_images = 0;
                
                // Process all downloaded images
                for (int i = 0; i < MAX_IMAGES; i++) {
                    if (s_images[i].buffer != NULL && s_images[i].buffer_size > 0) {
                        if (process_downloaded_image(i) == ESP_OK) {
                            processed_images++;
                            ESP_LOGI(TAG, "Successfully processed image %d", i);
                        } else {
                            ESP_LOGW(TAG, "Failed to process image %d", i);
                            s_images[i].is_valid = false;
                        }
                    }
                }
                
                if (processed_images > 0) {
                    ESP_LOGI(TAG, "Successfully processed %d images", processed_images);
                    
                    // Update global pointer to first valid image
                    s_current_display_image = get_next_valid_image();
                    
                    // Notify display task to show the first image immediately
                    if (s_display_task_handle != NULL) {
                        xTaskNotifyGive(s_display_task_handle);
                    }
                    
                    // Start or restart the image cycling timer
                    if (s_image_cycle_timer != NULL) {
                        xTimerStop(s_image_cycle_timer, 0);
                        xTimerStart(s_image_cycle_timer, 0);
                    } else {
                        // Create the image cycling timer
                        s_image_cycle_timer = xTimerCreate(
                            "image_cycle_timer",
                            pdMS_TO_TICKS(IMAGE_DISPLAY_INTERVAL_MS),
                            pdTRUE,  // Auto-reload timer
                            NULL,    // Timer ID
                            image_cycle_timer_callback
                        );
                        
                        if (s_image_cycle_timer != NULL) {
                            xTimerStart(s_image_cycle_timer, 0);
                            ESP_LOGI(TAG, "Started image cycling timer with %d ms interval", IMAGE_DISPLAY_INTERVAL_MS);
                        } else {
                            ESP_LOGE(TAG, "Failed to create image cycling timer");
                        }
                    }
                    
                } else {
                    ESP_LOGW(TAG, "No images were processed successfully, using error image");
                    s_current_display_image = &error_image;
                    if (s_display_task_handle != NULL) {
                        xTaskNotifyGive(s_display_task_handle);
                    }
                }
                
            } else {
                // If download fails, fall back to the error image
                ESP_LOGW(TAG, "Image download failed, using error image");
                s_current_display_image = &error_image;
                if (s_display_task_handle != NULL) {
                    xTaskNotifyGive(s_display_task_handle);
                }
            }
        }
        
        // Check every minute to see if it's time to update
        vTaskDelay(pdMS_TO_TICKS(60000)); // 1 minute
    }
}

#if ENABLE_PIR_BACKLIGHT_TIMER
/**
 * @brief PIR monitoring task for non-touchscreen version
 * This task monitors the PIR motion detection semaphore and handles backlight control
 */
static void pir_monitoring_task(void *pvParameters)
{
    ESP_LOGI(TAG, "PIR monitoring task started");
    
    while (1) {
        // Wait for motion detection semaphore
        if (xSemaphoreTake(pir_motion_semaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "PIR motion detected!");
            
            // Use protected backlight access
            if (!get_backlight_state()) {
                set_backlight_state(true);
            }
            
            // Reset and restart the backlight timer
            if (backlight_timer != NULL) {
                ESP_LOGI(TAG, "Resetting backlight timer due to motion detection: %d ms", BACKLIGHT_TIMEOUT_MS);
                xTimerReset(backlight_timer, 0);
            }
        }
        
        // Optional: still log GPIO level changes for debugging
        int current_gpio_level = gpio_get_level(PIR_SENSOR_GPIO);
        ESP_LOGD(TAG, "PIR GPIO level: %d", current_gpio_level);
    }
}
#endif

// Cleanup function for proper resource management
static void cleanup_resources(void)
{
    // Cleanup image URLs
    http_cleanup_image_urls();
    
    // Stop timers
    if (s_image_cycle_timer != NULL) {
        xTimerStop(s_image_cycle_timer, 0);
        xTimerDelete(s_image_cycle_timer, 0);
        s_image_cycle_timer = NULL;
    }
    
    if (backlight_timer != NULL) {
        xTimerStop(backlight_timer, 0);
        xTimerDelete(backlight_timer, 0);
        backlight_timer = NULL;
    }
    
    // Cleanup synchronization primitives
    if (backlight_mutex != NULL) {
        vSemaphoreDelete(backlight_mutex);
        backlight_mutex = NULL;
    }
    
#if ENABLE_PIR_BACKLIGHT_TIMER
    if (pir_motion_semaphore != NULL) {
        vSemaphoreDelete(pir_motion_semaphore);
        pir_motion_semaphore = NULL;
    }
#endif
    
    // Free image buffers
    for (int i = 0; i < MAX_IMAGES; i++) {
        reset_image_buffer(i);
    }
    
    ESP_LOGI(TAG, "Resource cleanup completed");
}

void app_main(void)
{       
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize synchronization primitives
    backlight_mutex = xSemaphoreCreateMutex();
    if (backlight_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create backlight mutex");
        return;
    }

#if ENABLE_PIR_BACKLIGHT_TIMER
    pir_motion_semaphore = xSemaphoreCreateBinary();
    if (pir_motion_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create PIR motion semaphore");
        return;
    }
#endif
    
    // Print heap info
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "Total allocatable memory: %d bytes (%d MB)", 
             total_heap, total_heap / (1024 * 1024));
    
    // Initialize image data structures
    for (int i = 0; i < MAX_IMAGES; i++) {
        s_images[i].buffer = NULL;
        s_images[i].buffer_size = 0;
        s_images[i].buffer_allocated = 0;
        s_images[i].is_valid = false;
        s_images[i].download_timestamp = 0;
        memset(&s_images[i].img_dsc, 0, sizeof(lv_img_dsc_t));
    }
    s_current_image_index = 0;
    s_image_cycle_timer = NULL;
    
    ESP_LOGI(TAG, "Initialized %d image slots", MAX_IMAGES);
    
    // Initialize LCD
    ESP_ERROR_CHECK(lcd_init(&lcd_panel));
    vTaskDelay(pdMS_TO_TICKS(100)); // Add 100ms delay

    
#if ENABLE_TOUCHSCREEN
    ESP_ERROR_CHECK(touch_init(&my_bus, &touch_io_handle, &touch_handle));
    ESP_ERROR_CHECK(app_lvgl_init(lcd_panel, touch_handle, &lvgl_disp, &lvgl_touch_indev));
#else
    ESP_LOGI(TAG, "Touchscreen disabled - skipping touch initialization");
    ESP_ERROR_CHECK(app_lvgl_init(lcd_panel, NULL, &lvgl_disp, &lvgl_touch_indev));
#endif

    // Configure backlight GPIO
    const gpio_config_t bk_light = {
        .pin_bit_mask = (1ULL << BSP_LCD_GPIO_BK_LIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_light));

    ESP_LOGI(TAG, "Backlight GPIO=%d (mask 0x%llx), PIR GPIO=%d", BSP_LCD_GPIO_BK_LIGHT, (1ULL << BSP_LCD_GPIO_BK_LIGHT), PIR_SENSOR_GPIO);

#if ENABLE_PIR_BACKLIGHT_TIMER
   
    // Configure PIR sensor GPIO for input with both pull-up and pull-down tests
    ESP_LOGI(TAG, "Configuring GPIO %d for PIR sensor input...", PIR_SENSOR_GPIO);
    
    const gpio_config_t pir_final_config = {
        .pin_bit_mask = (1ULL << PIR_SENSOR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     // Use pull-up since PIR drives low normally
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE        // Trigger on positive edge (low to high = motion detected)
    };
    ESP_ERROR_CHECK(gpio_config(&pir_final_config));
    
    // Install GPIO ISR service
    esp_err_t isr_result = gpio_install_isr_service(0);
    if (isr_result == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "GPIO ISR service already installed");
    } else if (isr_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(isr_result));
        return;
    }
    
    // Add ISR handler for PIR sensor
    esp_err_t handler_result = gpio_isr_handler_add(PIR_SENSOR_GPIO, pir_sensor_isr_handler, NULL);
    if (handler_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %d: %s", PIR_SENSOR_GPIO, esp_err_to_name(handler_result));
        return;
    }
    
#endif
    
    // Turn on backlight
    set_backlight_state(true);
    
    // Initialize backlight timer as a one-shot timer for both touchscreen and non-touchscreen versions
    backlight_timer = xTimerCreate(
        "backlight_timer",
        pdMS_TO_TICKS(BACKLIGHT_TIMEOUT_MS), // Fire once after timeout period
        pdFALSE,              // One-shot timer (not auto-reload)
        NULL,                 // Timer ID
        backlight_timer_callback
    );
    
    if (backlight_timer != NULL) {
        // Start the timer - it will fire once after the timeout
        xTimerStart(backlight_timer, 0);
    }

#if ENABLE_TOUCHSCREEN
    ESP_LOGI(TAG, "Touchscreen enabled - backlight controlled by touch events");
#elif ENABLE_PIR_BACKLIGHT_TIMER
    ESP_LOGI(TAG, "PIR sensor enabled - backlight controlled by motion detection");
#else
    ESP_LOGI(TAG, "No touchscreen or PIR - backlight timer only");
#endif

    // Show initial loading screen
    lvgl_port_lock(0);
    lv_obj_t *loading = lv_obj_create(lv_screen_active());
    lv_obj_set_size(loading, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(loading, lv_color_black(), 0);
    
#if ENABLE_TOUCHSCREEN
    // Add touch event to loading screen
    lv_obj_add_event_cb(loading, touch_event_cb, LV_EVENT_PRESSED, NULL);
#endif
    
    lv_obj_t *loading_label = lv_label_create(loading);
    lv_label_set_text(loading_label, "Loading images...");
    lv_obj_center(loading_label);
    lv_obj_set_style_text_color(loading_label, lv_color_white(), 0);
    lvgl_port_unlock();
    
    // Initialize WiFi
    ESP_LOGI(TAG, "Connecting to WiFi...");
    if (wifi_init_sta() == ESP_OK) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        
        // Add delay to allow network stack to fully stabilize
        // This prevents "connection reset by peer" errors on first HTTP requests
        ESP_LOGI(TAG, "Waiting %d seconds for network stack stabilization...", NETWORK_STABILIZATION_DELAY_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(NETWORK_STABILIZATION_DELAY_MS));
        
        ESP_LOGI(TAG, "Network stabilized, initializing time synchronization...");        
        
        // Initialize time synchronization based on menuconfig selection
#ifdef CONFIG_TIME_SYNC_WORLDTIME_API
        ESP_LOGI(TAG, "Using WorldTimeAPI for time synchronization");
        if (initialize_worldtime_api() == ESP_OK) {
            ESP_LOGI(TAG, "WorldTimeAPI time synchronization successful");
        } else {
            ESP_LOGW(TAG, "WorldTimeAPI time synchronization failed, time may not be accurate");
        }
#else
        ESP_LOGI(TAG, "Using SNTP for time synchronization");
        if (initialize_sntp() == ESP_OK) {
            ESP_LOGI(TAG, "SNTP initialized successfully");
        } else {
            ESP_LOGW(TAG, "SNTP initialization failed, time may not be accurate");
        }
#endif
        
        if (xTaskCreate(display_image_task, "display_image_task", DISPLAY_TASK_STACK_SIZE, NULL, DISPLAY_TASK_PRIORITY, &s_display_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create display task");
            cleanup_resources();
            return;
        }

#if ENABLE_PIR_BACKLIGHT_TIMER
        // Create PIR monitoring task for non-touchscreen version
        ESP_LOGI(TAG, "Starting PIR monitoring task...");
        TaskHandle_t pir_task_handle = NULL;
        if (xTaskCreate(pir_monitoring_task, "pir_monitoring_task", 4096, NULL, 3, &pir_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create PIR monitoring task");
            cleanup_resources();
            return;
        }
#endif

        ESP_LOGI(TAG, "Starting image refresh task...");
        TaskHandle_t update_task_handle = NULL;
        if (xTaskCreate(update_image_task, "update_image_task", UPDATE_TASK_STACK_SIZE, NULL, UPDATE_TASK_PRIORITY, &update_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create update task");
            cleanup_resources();
            return;
        }
        
        ESP_LOGI(TAG, "Application initialized successfully");
    } else {
        ESP_LOGE(TAG, "WiFi connection failed - cleaning up and exiting");
        cleanup_resources();
        return;
    }
}
