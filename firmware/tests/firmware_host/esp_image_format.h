#pragma once
#include "esp_partition.h"
typedef struct {uint32_t offset,size;} esp_partition_pos_t;
typedef struct {uint32_t image_len;struct {uint8_t hash_appended;} image;} esp_image_metadata_t;
#define ESP_IMAGE_VERIFY_SILENT 1
esp_err_t esp_image_verify(int,const esp_partition_pos_t *,esp_image_metadata_t *);
