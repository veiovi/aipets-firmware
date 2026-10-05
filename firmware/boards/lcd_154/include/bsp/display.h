#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef struct {
    size_t max_transfer_sz;
} bsp_display_config_t;

#ifdef __cplusplus
extern "C" {
#endif

/* The ST7789 panel on its SPI bus: reset, colors inverted as the panel
 * needs, and switched on. The backlight stays off until
 * bsp_display_backlight_on(). */
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *panel,
                          esp_lcd_panel_io_handle_t *io);

#ifdef __cplusplus
}
#endif
