#include "pet_flash_layout.h"
#include <string.h>

const char *pet_flash_layout_name(pet_layout_id_t id)
{
    static const char *names[] = {"embedded-v1","dual-pet-v1","single-pet-2m-v2",
        "single-pet-2p5m-v2","single-pet-3m-v2","single-pet-4m-v2","three-pet-3p5m-v3"};
    return (unsigned)id < sizeof(names)/sizeof(names[0]) ? names[id] : NULL;
}
bool pet_flash_layout_known(pet_layout_id_t id, pet_flash_layout_t *out)
{
    if (!out || !pet_flash_layout_name(id)) return false;
    pet_flash_layout_t v = {.id=id};
    const uint32_t app_sizes[] = {0x620000,0x400000,0x200000,0x280000,0x300000,0x400000,0x380000};
    v.app_slot_bytes = app_sizes[id];
    v.regions[0] = (pet_flash_region_t){"nvs",1,2,0x9000,0x6000,0};
    v.regions[1] = (pet_flash_region_t){"phy_init",1,1,0xf000,0x1000,0};
    v.regions[2] = (pet_flash_region_t){"otadata",1,0,0x10000,0x2000,0};
    v.regions[3] = (pet_flash_region_t){"coredump",1,3,0x12000,0x10000,0};
    v.regions[4] = (pet_flash_region_t){"ota_0",0,0x10,0x30000,v.app_slot_bytes,0};
    v.regions[5] = (pet_flash_region_t){"ota_1",0,0x11,0x30000+v.app_slot_bytes,v.app_slot_bytes,0};
    v.count = 6;
    if (id == PET_LAYOUT_EMBEDDED_V1) {
        v.regions[v.count++] = (pet_flash_region_t){"assets",1,0x40,0xc70000,0x390000,0};
    } else {
        v.journal_offset = 0x30000 + 2*v.app_slot_bytes;
        v.pet_offset = v.journal_offset + 0x10000;
        v.regions[v.count++] = (pet_flash_region_t){"pet_journal",1,0x41,v.journal_offset,0x10000,0};
        if (id == PET_LAYOUT_DUAL_V1) {
            v.pet_slots = 2; v.pet_partition_bytes = 0x3e0000; v.pack_capacity_bytes = 0x2a0000;
            v.regions[v.count++] = (pet_flash_region_t){"pet_a",1,0x42,v.pet_offset,v.pet_partition_bytes,0};
            v.regions[v.count++] = (pet_flash_region_t){"pet_b",1,0x42,0xc20000,v.pet_partition_bytes,0};
        } else if (id == PET_LAYOUT_THREE_3P5M) {
            /* A distinct subtype: single-pet firmware can never mistake the
             * store for its one pack partition. */
            v.pet_slots = PET_LAYOUT_THREE_SLOTS; v.pet_partition_bytes = PET_FLASH_BYTES - v.pet_offset;
            v.pack_capacity_bytes = PET_LAYOUT_THREE_SLOT_PACK_BYTES;
            v.regions[v.count++] = (pet_flash_region_t){"pet_store",1,0x43,v.pet_offset,v.pet_partition_bytes,0};
        } else {
            v.pet_slots = 1; v.pet_partition_bytes = PET_FLASH_BYTES - v.pet_offset;
            v.pack_capacity_bytes = v.pet_partition_bytes - PET_LAYOUT_MANIFEST_BYTES;
            v.regions[v.count++] = (pet_flash_region_t){"pet_pack",1,0x42,v.pet_offset,v.pet_partition_bytes,0};
        }
    }
    *out = v; return true;
}
static bool same_region(const pet_flash_region_t *a, const pet_flash_region_t *b)
{
    return memchr(a->name, 0, sizeof(a->name)) && !strcmp(a->name, b->name) &&
        a->type == b->type && a->subtype == b->subtype && a->offset == b->offset &&
        a->bytes == b->bytes && a->flags == b->flags;
}
bool pet_flash_layout_identify(const pet_flash_region_t *regions, size_t count,
                               uint32_t flash_bytes, pet_flash_layout_t *out)
{
    if (!regions || !out || flash_bytes != PET_FLASH_BYTES || count < 7 || count > PET_LAYOUT_REGIONS_MAX)
        return false;
    for (unsigned id = PET_LAYOUT_EMBEDDED_V1; id <= PET_LAYOUT_THREE_3P5M; ++id) {
        pet_flash_layout_t v; if (!pet_flash_layout_known((pet_layout_id_t)id, &v) || v.count != count) continue;
        bool match = true; unsigned seen = 0;
        for (size_t i = 0; match && i < count; ++i) {
            bool found = false;
            for (size_t j = 0; j < count; ++j) if (!(seen & (1u << j)) && same_region(&regions[i], &v.regions[j])) {
                seen |= 1u << j; found = true; break;
            }
            if (!found) match = false;
        }
        if (match) { *out = v; return true; }
    }
    return false;
}
bool pet_flash_layout_select(uint32_t image_bytes, pet_flash_layout_t *out)
{
    if (!image_bytes || !out) return false;
    for (unsigned id = PET_LAYOUT_SINGLE_2M; id <= PET_LAYOUT_SINGLE_4M; ++id) {
        pet_flash_layout_t v; if (!pet_flash_layout_known((pet_layout_id_t)id, &v)) return false;
        if ((uint64_t)image_bytes * 100 <= (uint64_t)v.app_slot_bytes * 85) { *out = v; return true; }
    }
    return false;
}
bool pet_flash_layout_allows_legacy_ota(const pet_flash_layout_t *layout)
{
    return layout && (layout->id == PET_LAYOUT_EMBEDDED_V1 || layout->id == PET_LAYOUT_DUAL_V1);
}
bool pet_flash_layout_slot(const pet_flash_layout_t *layout, unsigned slot,
                           uint32_t *pack_offset, uint32_t *manifest_offset)
{
    if (!layout || layout->id != PET_LAYOUT_THREE_3P5M || slot >= PET_LAYOUT_THREE_SLOTS ||
        (uint64_t)PET_LAYOUT_THREE_SLOTS * PET_LAYOUT_THREE_SLOT_BYTES > layout->pet_partition_bytes) return false;
    uint32_t base = slot * PET_LAYOUT_THREE_SLOT_BYTES;
    if (pack_offset) *pack_offset = base;
    if (manifest_offset) *manifest_offset = base + PET_LAYOUT_THREE_SLOT_PACK_BYTES;
    return true;
}
