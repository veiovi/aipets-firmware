#include "pet_flash_layout.h"
#include "pet_replace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    assert(PET_LAYOUT_MANIFEST_BYTES == PET_REPLACE_MANIFEST_BYTES);
    for (unsigned id = PET_LAYOUT_EMBEDDED_V1; id <= PET_LAYOUT_SINGLE_4M; ++id) {
        pet_flash_layout_t v, parsed; assert(pet_flash_layout_known((pet_layout_id_t)id, &v));
        assert(pet_flash_layout_identify(v.regions, v.count, PET_FLASH_BYTES, &parsed));
        assert(parsed.id == id);
        assert(pet_flash_layout_allows_legacy_ota(&parsed) == (id <= PET_LAYOUT_DUAL_V1));
        assert(v.regions[v.count-1].offset + v.regions[v.count-1].bytes == PET_FLASH_BYTES);
        assert(v.regions[0].offset == 0x9000 && v.regions[0].bytes == 0x6000);
        assert(v.regions[4].bytes == v.regions[5].bytes && v.regions[4].offset == 0x30000);
        assert(!pet_flash_layout_identify(v.regions, v.count, 20*1024*1024, &parsed));
        assert(!pet_flash_layout_identify(v.regions, v.count-1, PET_FLASH_BYTES, &parsed));
        for (size_t i = 0; i < v.count; ++i) {
            pet_flash_layout_t corrupt = v; corrupt.regions[i].flags = 1;
            assert(!pet_flash_layout_identify(corrupt.regions, corrupt.count, PET_FLASH_BYTES, &parsed));
            corrupt = v; corrupt.regions[i].offset += 4096;
            assert(!pet_flash_layout_identify(corrupt.regions, corrupt.count, PET_FLASH_BYTES, &parsed));
            corrupt = v; corrupt.regions[i].bytes -= 1;
            assert(!pet_flash_layout_identify(corrupt.regions, corrupt.count, PET_FLASH_BYTES, &parsed));
            corrupt = v; corrupt.regions[i] = corrupt.regions[(i+1)%v.count];
            assert(!pet_flash_layout_identify(corrupt.regions, corrupt.count, PET_FLASH_BYTES, &parsed));
            corrupt = v; memset(corrupt.regions[i].name, 'x', sizeof(corrupt.regions[i].name));
            assert(!pet_flash_layout_identify(corrupt.regions, corrupt.count, PET_FLASH_BYTES, &parsed));
        }
        pet_flash_region_t swap = v.regions[0]; v.regions[0] = v.regions[v.count-1]; v.regions[v.count-1] = swap;
        assert(pet_flash_layout_identify(v.regions, v.count, PET_FLASH_BYTES, &parsed));
        if (id >= PET_LAYOUT_SINGLE_2M) {
            assert(v.pet_slots == 1 && pet_replace_capacity_valid(v.pack_capacity_bytes));
            uint32_t boundary = (uint32_t)((uint64_t)v.app_slot_bytes * 85 / 100);
            assert(pet_flash_layout_select(boundary, &parsed) && parsed.id == id);
            if (id == PET_LAYOUT_SINGLE_4M) assert(!pet_flash_layout_select(boundary+1, &parsed));
            else assert(pet_flash_layout_select(boundary+1, &parsed) && parsed.id == id+1);
        }
    }
    pet_flash_layout_t v;
    assert(!pet_flash_layout_allows_legacy_ota(NULL));
    memset(&v, 0, sizeof(v)); v.id = (pet_layout_id_t)999;
    assert(!pet_flash_layout_allows_legacy_ota(&v));
    assert(!pet_flash_layout_select(0, &v)); assert(!pet_flash_layout_select(UINT32_MAX, &v));
    assert(!pet_flash_layout_known((pet_layout_id_t)-1, &v)); assert(!pet_flash_layout_known((pet_layout_id_t)7, &v));

    /* Pocket Terminal: three 3 MB slots behind two 3.5 MiB firmware copies. */
    pet_flash_layout_t three, parsed;
    assert(pet_flash_layout_known(PET_LAYOUT_THREE_3P5M, &three));
    assert(!strcmp(pet_flash_layout_name(three.id), "three-pet-3p5m-v3"));
    assert(pet_flash_layout_identify(three.regions, three.count, PET_FLASH_BYTES, &parsed) && parsed.id == PET_LAYOUT_THREE_3P5M);
    assert(three.app_slot_bytes == 0x380000 && three.regions[5].offset == 0x3b0000);
    assert(three.journal_offset == 0x730000 && three.pet_offset == 0x740000 && three.pet_partition_bytes == 0x8c0000);
    assert(three.pet_slots == 3 && three.pack_capacity_bytes == PET_LAYOUT_THREE_SLOT_PACK_BYTES);
    assert(PET_LAYOUT_THREE_SLOT_PACK_BYTES >= 3000000u && PET_LAYOUT_THREE_SLOT_PACK_BYTES % PET_FLASH_SECTOR_BYTES == 0);
    assert(pet_replace_capacity_valid(three.pack_capacity_bytes));
    const pet_flash_region_t *store = &three.regions[three.count - 1];
    assert(!strcmp(store->name, "pet_store") && store->subtype == 0x43 && store->offset + store->bytes == PET_FLASH_BYTES);
    assert(!pet_flash_layout_allows_legacy_ota(&three));
    uint32_t pack_at = 1, manifest_at = 1, previous_end = 0;
    for (unsigned slot = 0; slot < PET_LAYOUT_THREE_SLOTS; ++slot) {
        assert(pet_flash_layout_slot(&three, slot, &pack_at, &manifest_at));
        assert(pack_at == previous_end && manifest_at == pack_at + PET_LAYOUT_THREE_SLOT_PACK_BYTES);
        assert(pack_at % PET_FLASH_SECTOR_BYTES == 0 && manifest_at % PET_FLASH_SECTOR_BYTES == 0);
        previous_end = manifest_at + PET_LAYOUT_MANIFEST_BYTES;
    }
    /* 143,360 bytes of the store remain for the inventory reserve. */
    assert(three.pet_partition_bytes - previous_end == 143360u);
    assert(!pet_flash_layout_slot(&three, PET_LAYOUT_THREE_SLOTS, &pack_at, &manifest_at));
    pet_flash_layout_t single; assert(pet_flash_layout_known(PET_LAYOUT_SINGLE_2M, &single));
    assert(!pet_flash_layout_slot(&single, 0, &pack_at, &manifest_at));
    /* Single-pet sizing never selects the three-pet store. */
    for (uint32_t bytes = 1; bytes < 0x400000; bytes += 0x10000)
        if (pet_flash_layout_select(bytes, &parsed)) assert(parsed.pet_slots == 1);
    puts("flash layouts: exact known regions, duplicates, flags, capacities and smallest-slot 15% boundaries passed");
    return 0;
}
