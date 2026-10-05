#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_MMAP_DATA 0
typedef struct { uint32_t address; uint32_t size; const char *label; } esp_partition_t;
typedef uint32_t esp_partition_mmap_handle_t;
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition,size_t offset,void *bytes,size_t length);
esp_err_t esp_partition_write(const esp_partition_t *partition,size_t offset,const void *bytes,size_t length);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,size_t offset,size_t length);
esp_err_t esp_partition_mmap(const esp_partition_t *partition,size_t offset,size_t length,int memory,
                             const void **address,esp_partition_mmap_handle_t *handle);
void esp_partition_munmap(esp_partition_mmap_handle_t handle);
