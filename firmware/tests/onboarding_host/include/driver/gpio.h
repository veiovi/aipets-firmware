#pragma once

typedef int gpio_num_t;

#define GPIO_NUM_NC (-1)

int gpio_get_level(gpio_num_t gpio_num);
