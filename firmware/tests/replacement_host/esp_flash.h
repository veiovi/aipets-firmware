#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_flash_t {unsigned fixture;} esp_flash_t;
extern esp_flash_t *esp_flash_default_chip;
esp_err_t esp_flash_get_physical_size(esp_flash_t *,uint32_t *);
esp_err_t esp_flash_read(esp_flash_t *,void *,uint32_t,uint32_t);
