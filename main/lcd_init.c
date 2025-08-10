#include "lcd_init.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "bsp.h"

/* LCD settings */
#define APP_LCD_RGB_BUFFER_NUMS             (2)
#define APP_LCD_RGB_BOUNCE_BUFFER_HEIGHT    (10)
#define APP_LCD_RGB_BOUNCE_BUFFER_MODE      (1)

static const char TAG[] = "lcd_init";

esp_err_t lcd_init(esp_lcd_panel_handle_t *lp)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initialize RGB panel");
    const esp_lcd_rgb_panel_config_t conf = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = BSP_LCD_PANEL_TIMING(),
        .data_width = 16,
        .num_fbs = APP_LCD_RGB_BUFFER_NUMS,
#ifdef APP_LCD_RGB_BOUNCE_BUFFER_MODE
        .bounce_buffer_size_px = BSP_LCD_H_RES * APP_LCD_RGB_BOUNCE_BUFFER_HEIGHT,
#endif
        .hsync_gpio_num = BSP_LCD_GPIO_HSYNC,
        .vsync_gpio_num = BSP_LCD_GPIO_VSYNC,
        .de_gpio_num = BSP_LCD_GPIO_DE,
        .pclk_gpio_num = BSP_LCD_GPIO_PCLK,
        .disp_gpio_num = BSP_LCD_GPIO_DISP,
        .data_gpio_nums = BSP_LCD_GPIO_DATA(),
        .flags.fb_in_psram = 1,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_rgb_panel(&conf, lp),
                      err, TAG, "RGB init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(*lp),
                      err, TAG, "LCD init failed");
    return ret;

err:
    if (*lp)
    {
        esp_lcd_panel_del(*lp);
    }
    return ret;
}
