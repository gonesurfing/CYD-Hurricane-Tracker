#pragma once

/**
 * @file app_config.h
 * @brief Centralized configuration for Hurricane Tracking App
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Application Settings */
#define APP_NAME "hurricane_tracker"

/* Image Management */
#define MAX_IMAGES 10
#define IMAGE_DISPLAY_INTERVAL_MS (10 * 1000)  // 10 seconds between images

/* Backlight Settings */
#define BACKLIGHT_TIMEOUT_MS (60 * 1000)  // 120 seconds timeout

/* PIR sensor configuration (for non-touchscreen version) */
#define PIR_SENSOR_GPIO GPIO_NUM_18

/* LCD Display Settings */
#define APP_LCD_LVGL_FULL_REFRESH           (0)
#define APP_LCD_LVGL_DIRECT_MODE            (1)
#define APP_LCD_LVGL_AVOID_TEAR             (1)
#define APP_LCD_RGB_BOUNCE_BUFFER_MODE      (1)
#define APP_LCD_DRAW_BUFF_DOUBLE            (0)
#define APP_LCD_DRAW_BUFF_HEIGHT            (100)
#define APP_LCD_RGB_BUFFER_NUMS             (2)
#define APP_LCD_RGB_BOUNCE_BUFFER_HEIGHT    (10)

/* Network Settings */
#define MAX_HTTP_RECV_BUFFER 32768
#define NETWORK_STABILIZATION_DELAY_MS 3000
#define HTTP_TIMEOUT_MS 30000
#define XML_TIMEOUT_MS 15000

/* URLs */
#define NHC_XML_FEED_URL "https://www.nhc.noaa.gov/index-at.xml"

/* Static images that always display */
#define STATIC_IMAGE_COUNT 2

// Static image URLs and names as macros
#define STATIC_IMAGE_URL_0 "https://www.nhc.noaa.gov/xgtwo/two_atl_7d0.png"
#define STATIC_IMAGE_URL_1 "https://www.nhc.noaa.gov/xgtwo/two_atl_2d0.png"
#define STATIC_IMAGE_NAME_0 "Atlantic 7-Day Outlook"
#define STATIC_IMAGE_NAME_1 "Atlantic 2-Day Outlook"

// Helper macros to initialize arrays in .c files
#define STATIC_IMAGE_URLS_INIT { STATIC_IMAGE_URL_0, STATIC_IMAGE_URL_1 }
#define STATIC_IMAGE_NAMES_INIT { STATIC_IMAGE_NAME_0, STATIC_IMAGE_NAME_1 }

// Times to update images from nhc 00:10 UTC, and every 3 hours after
#define NHC_UPDATE_TIMES { "00:10", "03:10", "06:10", "09:10", "12:10", "15:10", "18:10", "21:10" }
#define NHC_UPDATE_TIMES_COUNT 8

/* WiFi Settings */
#define MAXIMUM_RETRY 5

/* Task Settings */
#define LVGL_TASK_PRIORITY 4
#define LVGL_TASK_STACK_SIZE 8192
#define DISPLAY_TASK_STACK_SIZE 8192
#define UPDATE_TASK_STACK_SIZE 16384
#define DISPLAY_TASK_PRIORITY 4
#define UPDATE_TASK_PRIORITY 5

/* LVGL Settings */
#define LVGL_TASK_MAX_SLEEP_MS 500
#define LVGL_TIMER_PERIOD_MS 5

/* Time Settings */
#define TIMEZONE_CONFIG "UTC0"
#define UPDATE_INTERVAL_MS (60 * 60 * 1000)  // 1 hour

/* Touchscreen Configuration */
#ifdef CONFIG_ENABLE_TOUCHSCREEN
#define ENABLE_TOUCHSCREEN 1
#else
#define ENABLE_TOUCHSCREEN 0
#endif

/* PIR Backlight Timer Configuration */
#if !ENABLE_TOUCHSCREEN && defined(CONFIG_ENABLE_PIR_BACKLIGHT_TIMER)
#define ENABLE_PIR_BACKLIGHT_TIMER 1
#else
#define ENABLE_PIR_BACKLIGHT_TIMER 0
#endif

/* Image Processing */
#define MIN_VALID_IMAGE_SIZE 100  // Minimum bytes for valid image
#define IMAGE_HEADER_SIZE 12      // Custom binary format header size
#define LVGL_MAGIC_NUMBER 0x19    // Expected magic number for LVGL v9

#ifdef __cplusplus
}
#endif
