#include "touch_init.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "bsp.h"

static const char TAG[] = "touch_init";

esp_err_t touch_init(i2c_master_bus_handle_t *bus,
                     esp_lcd_panel_io_handle_t *tp_io,
                     esp_lcd_touch_handle_t *tp)
{
    if (!*bus)
    {
        ESP_LOGI(TAG, "Creating I2C master bus");
        const i2c_master_bus_config_t i2c_conf = {
            .i2c_port = -1,
            .sda_io_num = BSP_TOUCH_GPIO_SDA,
            .scl_io_num = BSP_TOUCH_GPIO_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = 1,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_conf, bus),
                            TAG, "Failed to create I2C master bus");
    }

    if (!*tp_io)
    {
        ESP_LOGI(TAG, "Creating touch panel IO");
        esp_lcd_panel_io_i2c_config_t tp_io_cfg =
            ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_cfg.scl_speed_hz = 400000;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v2(*bus, &tp_io_cfg, tp_io),
                            TAG, "Failed to create touch panel IO");
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_GPIO_RST,
        .int_gpio_num = BSP_TOUCH_GPIO_INT,
    };

    ESP_LOGI(TAG, "Initializing GT911 touch controller");
    return esp_lcd_touch_new_i2c_gt911(*tp_io, &tp_cfg, tp);
}