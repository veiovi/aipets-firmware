#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Both boards use the same LVGL adapter, lock contract and touch context. */
lv_display_t *pet_display_start(void);
lv_indev_t *pet_display_input(void);
esp_err_t pet_display_lock(int32_t timeout_ms);
void pet_display_unlock(void);
