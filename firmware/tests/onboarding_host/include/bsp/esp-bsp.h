#pragma once
#include "lvgl.h"
#include "esp_err.h"
typedef struct {
    struct { unsigned task_priority; unsigned task_max_delay_ms; } lv_adapter_cfg;
    unsigned rotation;
    unsigned tear_avoid_mode;
} bsp_display_cfg_t;
lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *config);
lv_indev_t *bsp_display_get_input_dev(void);
esp_err_t bsp_display_lock(int timeout);
void bsp_display_unlock(void);
esp_err_t bsp_display_brightness_set(int brightness);
