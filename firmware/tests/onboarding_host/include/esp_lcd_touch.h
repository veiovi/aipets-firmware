#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    uint16_t x;
    uint16_t y;
} esp_lcd_touch_point_data_t;

typedef struct esp_lcd_touch_fixture {
    struct {
        gpio_num_t int_gpio_num;
        struct {
            int interrupt;
        } levels;
    } config;
} *esp_lcd_touch_handle_t;

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t handle);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t handle,
                                 esp_lcd_touch_point_data_t *points,
                                 uint8_t *count, uint8_t capacity);
