#pragma once
#include "esp_err.h"
#include "pet_flash_layout.h"

/* Hash is the exact 0xc00-byte ESP-IDF partition-table artifact, including MD5
 * record/padding. The remaining sector bytes must be erased. This is read-only;
 * recognizing a layout never migrates it or authorizes a download. */
esp_err_t pet_flash_layout_read(pet_flash_layout_t *layout, char partition_sha256[65]);
