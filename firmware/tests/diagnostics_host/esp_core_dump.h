#pragma once
/* Host stand-in with the ESP-IDF 5.5.3 Xtensa summary fields the firmware reads. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define APP_ELF_SHA256_SZ 10
typedef struct { uint32_t bt[16]; uint32_t depth; bool corrupted; } esp_core_dump_bt_info_t;
typedef struct { uint32_t exc_cause; uint32_t exc_vaddr; } esp_core_dump_summary_extra_info_t;
typedef struct {
    uint32_t exc_tcb;
    char exc_task[16];
    uint32_t exc_pc;
    esp_core_dump_bt_info_t exc_bt_info;
    uint32_t core_dump_version;
    uint8_t app_elf_sha256[APP_ELF_SHA256_SZ];
    esp_core_dump_summary_extra_info_t ex_info;
} esp_core_dump_summary_t;
esp_err_t esp_core_dump_image_check(void);
esp_err_t esp_core_dump_image_erase(void);
esp_err_t esp_core_dump_get_panic_reason(char *reason_buffer, size_t buffer_size);
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *summary);
