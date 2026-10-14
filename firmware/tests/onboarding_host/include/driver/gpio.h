#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_NC (-1)
#define GPIO_NUM_0 0

typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
} gpio_config_t;

int gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_config(const gpio_config_t *config);
