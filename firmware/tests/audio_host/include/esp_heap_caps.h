#pragma once
/* Host stand-in for ESP-IDF esp_heap_caps.h: PSRAM is the host's heap. */
#include <stdlib.h>
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_SPIRAM (1u << 10)
static inline void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}
