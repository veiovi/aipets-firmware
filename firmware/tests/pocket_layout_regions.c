/* Host-only: print the three-pet layout regions as CSV rows for comparison
 * with partitions-pocket.csv, the table ESP-IDF builds into Pocket firmware. */
#include "pet_flash_layout.h"
#include <inttypes.h>
#include <stdio.h>
int main(void)
{
    pet_flash_layout_t v;
    if (!pet_flash_layout_known(PET_LAYOUT_THREE_3P5M, &v)) return 1;
    for (size_t i = 0; i < v.count; ++i)
        printf("%s,%u,%u,%" PRIu32 ",%" PRIu32 "\n", v.regions[i].name, v.regions[i].type,
               v.regions[i].subtype, v.regions[i].offset, v.regions[i].bytes);
    return 0;
}
