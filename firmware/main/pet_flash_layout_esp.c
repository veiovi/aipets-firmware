#include "pet_flash_layout_esp.h"
#include "pet_board.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_rom_md5.h"
#include "mbedtls/sha256.h"
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static bool raw_table_valid(const uint8_t *table, const pet_flash_layout_t *expected)
{
    pet_flash_region_t raw[PET_LAYOUT_REGIONS_MAX];
    for (size_t i = 0; i < expected->count; ++i) {
        const uint8_t *p = table + 32*i;
        if (p[0] != 0xaa || p[1] != 0x50 || !memchr(p+12, 0, 16)) return false;
        pet_flash_region_t *r = &raw[i]; memset(r, 0, sizeof(*r));
        memcpy(r->name, p+12, 16); r->type = p[2]; r->subtype = p[3];
        r->offset = u32(p+4); r->bytes = u32(p+8); r->flags = u32(p+28);
        /* ESP-IDF normalizes flags in esp_partition_t; raw unknown/encrypted
         * flags must also be rejected, even with flash encryption disabled. */
        if (r->flags) return false;
        size_t length = strlen(r->name);
        for (size_t j = length; j < 16; ++j) if (p[12+j]) return false;
    }
    pet_flash_layout_t identified;
    if (!pet_flash_layout_identify(raw, expected->count, PET_FLASH_BYTES, &identified) ||
        identified.id != expected->id) return false;
    size_t end = expected->count * 32;
    if (table[end] != 0xeb || table[end+1] != 0xeb) return false;
    for (size_t i = end+2; i < end+16; ++i) if (table[i] != 0xff) return false;
    md5_context_t context; uint8_t md5[16];
    esp_rom_md5_init(&context); esp_rom_md5_update(&context, table, (uint32_t)end); esp_rom_md5_final(md5, &context);
    if (memcmp(md5, table+end+16, sizeof(md5))) return false;
    for (size_t i = end+32; i < 4096; ++i) if (table[i] != 0xff) return false;
    return true;
}

esp_err_t pet_flash_layout_read(pet_flash_layout_t *layout, char partition_sha256[65])
{
    if (!layout || !partition_sha256) return ESP_ERR_INVALID_ARG;
    uint32_t physical = 0;
    if (esp_flash_get_physical_size(NULL, &physical) != ESP_OK || physical != pet_board_current()->flash_bytes)
        return ESP_ERR_INVALID_SIZE;
    pet_flash_region_t regions[PET_LAYOUT_REGIONS_MAX]; size_t count = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p || count >= PET_LAYOUT_REGIONS_MAX || p->flash_chip != esp_flash_default_chip ||
            p->erase_size != 4096 || !memchr(p->label, 0, sizeof(p->label))) {
            esp_partition_iterator_release(it); return ESP_ERR_INVALID_STATE;
        }
        pet_flash_region_t *r = &regions[count++]; memset(r, 0, sizeof(*r));
        memcpy(r->name, p->label, sizeof(r->name)); r->type = p->type; r->subtype = p->subtype;
        r->offset = p->address; r->bytes = p->size; r->flags = (p->encrypted ? 1u : 0u) | (p->readonly ? 2u : 0u);
        it = esp_partition_next(it);
    }
    pet_flash_layout_t identified;
    if (!pet_flash_layout_identify(regions, count, PET_FLASH_BYTES, &identified)) return ESP_ERR_INVALID_STATE;
    uint8_t *table = malloc(4096), digest[32]; if (!table) return ESP_ERR_NO_MEM;
    bool ok = esp_flash_read(NULL, table, PET_PARTITION_TABLE_OFFSET, 4096) == ESP_OK &&
        raw_table_valid(table, &identified) && !mbedtls_sha256(table, PET_PARTITION_TABLE_BYTES, digest, 0);
    free(table); if (!ok) return ESP_FAIL;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) { partition_sha256[2*i] = hex[digest[i] >> 4]; partition_sha256[2*i+1] = hex[digest[i] & 15]; }
    partition_sha256[64] = 0; *layout = identified; return ESP_OK;
}
