#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_FLASH_BYTES 0x1000000u
#define PET_FLASH_SECTOR_BYTES 0x1000u
#define PET_BOOTLOADER_OFFSET 0u
#define PET_PARTITION_TABLE_OFFSET 0x8000u
#define PET_BOOTLOADER_REGION_BYTES (PET_PARTITION_TABLE_OFFSET - PET_BOOTLOADER_OFFSET)
#define PET_PARTITION_TABLE_BYTES 0xc00u
#define PET_LAYOUT_MANIFEST_BYTES 0x2000u
#define PET_LAYOUT_REGIONS_MAX 9u
typedef enum { PET_LAYOUT_EMBEDDED_V1, PET_LAYOUT_DUAL_V1,
    PET_LAYOUT_SINGLE_2M, PET_LAYOUT_SINGLE_2P5M, PET_LAYOUT_SINGLE_3M,
    PET_LAYOUT_SINGLE_4M, PET_LAYOUT_THREE_3P5M } pet_layout_id_t;
/* Pocket Terminal: two 3.5 MiB firmware copies and one pet store holding three
 * slots. Each slot is a 3,002,368-byte pack area (733 sectors, at least the
 * 3,000,000-byte pack cap) followed by its 8 KiB signed release record. */
#define PET_LAYOUT_THREE_SLOTS 3u
#define PET_LAYOUT_THREE_SLOT_PACK_BYTES 0x2dd000u
#define PET_LAYOUT_THREE_SLOT_BYTES (PET_LAYOUT_THREE_SLOT_PACK_BYTES + PET_LAYOUT_MANIFEST_BYTES)
typedef struct {
    char name[17];
    uint8_t type, subtype;
    uint32_t offset, bytes, flags;
} pet_flash_region_t;
typedef struct {
    pet_layout_id_t id;
    uint32_t app_slot_bytes, journal_offset, pet_offset, pet_partition_bytes, pack_capacity_bytes;
    unsigned pet_slots;
    pet_flash_region_t regions[PET_LAYOUT_REGIONS_MAX];
    size_t count;
} pet_flash_layout_t;
const char *pet_flash_layout_name(pet_layout_id_t id);
bool pet_flash_layout_known(pet_layout_id_t id, pet_flash_layout_t *layout);
/* Runtime receives ESP-IDF's validated partition entries and the detected flash
 * size. Unknown/additional/duplicate/encrypted/read-only regions fail closed.
 * Secure boot/download restrictions need a separate hardware check in USB UI. */
bool pet_flash_layout_identify(const pet_flash_region_t *regions, size_t count,
                               uint32_t flash_bytes, pet_flash_layout_t *layout);
bool pet_flash_layout_select(uint32_t final_app_bytes, pet_flash_layout_t *layout);
/* Call only with a layout returned by the validated physical reader. New
 * single-slot layouts require signed v2 commands, never legacy socket OTA. */
bool pet_flash_layout_allows_legacy_ota(const pet_flash_layout_t *layout);
/* Offsets inside the pet partition of a multi-slot layout's pack area and its
 * release record. False for single-pet layouts and out-of-range slots. */
bool pet_flash_layout_slot(const pet_flash_layout_t *layout, unsigned slot,
                           uint32_t *pack_offset, uint32_t *manifest_offset);
