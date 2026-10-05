#pragma once

#include "esp_err.h"

/* Install the resilient CST816S reader and an independent high-frequency LVGL
 * input timer on the display's existing touch device. Safe to call again when
 * the pet renderer takes over the display from the native setup shell. */
esp_err_t pet_touch_enable_resilient(void);
