#ifndef LCD_INIT_H
#define LCD_INIT_H

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/**
 * @brief Initialize the LCD panel
 * 
 * @param lp Pointer to LCD panel handle (will be created)
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t lcd_init(esp_lcd_panel_handle_t *lp);

#endif // LCD_INIT_H
