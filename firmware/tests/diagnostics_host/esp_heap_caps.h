#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 4
size_t heap_caps_get_minimum_free_size(unsigned capabilities);
size_t heap_caps_get_largest_free_block(unsigned capabilities);
