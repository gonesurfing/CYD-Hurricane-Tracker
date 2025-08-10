#ifndef TOUCH_INIT_H
#define TOUCH_INIT_H

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "driver/i2c_master.h"

/**
 * @brief Initialize the touch controller
 * 
 * @param bus Pointer to I2C master bus handle (will be created if NULL)
 * @param tp_io Pointer to touch panel IO handle (will be created if NULL)
 * @param tp Pointer to touch handle (will be created)
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t touch_init(i2c_master_bus_handle_t *bus,
                     esp_lcd_panel_io_handle_t *tp_io,
                     esp_lcd_touch_handle_t *tp);

#endif // TOUCH_INIT_H