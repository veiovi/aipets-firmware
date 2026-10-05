#pragma once
#include "esp_err.h"
#include "esp_lcd_touch.h"

typedef struct {
    void *unused;
} bsp_touch_config_t;

#ifdef __cplusplus
extern "C" {
#endif

/* The CST816S on the shared I2C bus, in the panel's orientation. */
esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *touch);

#ifdef __cplusplus
}
#endif
