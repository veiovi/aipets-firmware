/* Host-only data exporter. Link the actual firmware geometry/selector rather
 * than maintaining a second table of capacities or installer write offsets. */
#include "pet_flash_layout.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    pet_flash_layout_t selected;
    bool has_selection = argc == 2;
    uint32_t image_bytes = 0;
    if (argc > 2) return 2;
    if (has_selection) {
        if (!argv[1][0] || strspn(argv[1], "0123456789") != strlen(argv[1])) return 2;
        char *end; errno = 0;
        unsigned long long value = strtoull(argv[1], &end, 10);
        if (errno || *end || !value || value > UINT32_MAX ||
            !pet_flash_layout_select((uint32_t)value, &selected)) return 2;
        image_bytes = (uint32_t)value;
    }
    printf("{\"flashBytes\":%u,\"sectorBytes\":%u,\"partitionTableOffset\":%u,"
           "\"partitionTableBytes\":%u,\"manifestReservationBytes\":%u,"
           "\"bootloaderOffset\":%u,\"bootloaderRegionBytes\":%u,\"selection\":",
           PET_FLASH_BYTES, PET_FLASH_SECTOR_BYTES, PET_PARTITION_TABLE_OFFSET,
           PET_PARTITION_TABLE_BYTES, PET_LAYOUT_MANIFEST_BYTES,
           PET_BOOTLOADER_OFFSET, PET_BOOTLOADER_REGION_BYTES);
    if (has_selection) printf("{\"appBytes\":%" PRIu32 ",\"layoutId\":\"%s\"}",
                              image_bytes, pet_flash_layout_name(selected.id));
    else printf("null");
    printf(",\"layouts\":[");
    for (unsigned id = PET_LAYOUT_EMBEDDED_V1; id <= PET_LAYOUT_THREE_3P5M; ++id) {
        pet_flash_layout_t v, identified;
        if (!pet_flash_layout_known((pet_layout_id_t)id, &v) ||
            !pet_flash_layout_identify(v.regions, v.count, PET_FLASH_BYTES, &identified) ||
            identified.id != v.id) return 3;
        printf("%s{\"id\":\"%s\",\"appSlotBytes\":%" PRIu32 ",\"petSlots\":%u,"
               "\"journalOffset\":%" PRIu32 ",\"petOffset\":%" PRIu32 ","
               "\"petPartitionBytes\":%" PRIu32 ",\"packCapacityBytes\":%" PRIu32 ",\"regions\":[",
               id ? "," : "", pet_flash_layout_name(v.id), v.app_slot_bytes, v.pet_slots,
               v.journal_offset, v.pet_offset, v.pet_partition_bytes, v.pack_capacity_bytes);
        for (size_t i = 0; i < v.count; ++i) {
            const pet_flash_region_t *r = &v.regions[i];
            printf("%s{\"name\":\"%s\",\"type\":%u,\"subtype\":%u,\"offset\":%" PRIu32
                   ",\"bytes\":%" PRIu32 ",\"flags\":%" PRIu32 "}",
                   i ? "," : "", r->name, r->type, r->subtype, r->offset, r->bytes, r->flags);
        }
        printf("]}");
    }
    printf("]}\n");
    return ferror(stdout) ? 4 : 0;
}
