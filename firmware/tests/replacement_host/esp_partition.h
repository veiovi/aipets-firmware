#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_ANY 0xff
#define ESP_PARTITION_SUBTYPE_ANY 0xff
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
#define ESP_PARTITION_MMAP_DATA 0
typedef struct esp_flash_t esp_flash_t;
typedef struct {
    esp_flash_t *flash_chip;
    uint8_t type, subtype;
    uint32_t address, size, erase_size;
    char label[17];
    bool encrypted, readonly;
} esp_partition_t;
typedef void *esp_partition_iterator_t;
typedef uint32_t esp_partition_mmap_handle_t;
esp_partition_iterator_t esp_partition_find(int type, int subtype, const char *label);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator);
void esp_partition_iterator_release(esp_partition_iterator_t iterator);
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *,size_t,void *,size_t);
esp_err_t esp_partition_write(const esp_partition_t *,size_t,const void *,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *,size_t,size_t);
esp_err_t esp_partition_mmap(const esp_partition_t *,size_t,size_t,int,const void **,esp_partition_mmap_handle_t *);
void esp_partition_munmap(esp_partition_mmap_handle_t);
