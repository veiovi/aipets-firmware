#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ESP-IDF 5.5.3's two 32-byte otadata records. Only sequence is CRC-covered;
 * state is interpreted using the pinned bootloader contract, not authenticated.
 * This module is read-only and is not a partition-table or image validator. */
#define PET_OTA_RECORD_BYTES 32u
#define PET_OTA_STATE_NEW 0u
#define PET_OTA_STATE_PENDING 1u
#define PET_OTA_STATE_VALID 2u
#define PET_OTA_STATE_INVALID 3u
#define PET_OTA_STATE_ABORTED 4u
#define PET_OTA_STATE_UNDEFINED UINT32_MAX
/* IDF 5.5.3 allocates a new sequence with a linear loop from zero. Bound writes
 * before that API; large/near-wrap records remain readable for recovery. */
#define PET_OTA_WRITE_SEQUENCE_MAX 65534u
typedef struct {
    uint32_t sequence,state;
    unsigned slot;
    bool erased,crc_valid,bootable;
} pet_ota_record_t;
typedef struct {pet_ota_record_t records[2];int active;} pet_ota_selection_t;

/* Exactly two OTA app slots. Torn/erased copies may coexist with one valid
 * copy. Unknown CRC-valid state, sequence zero and conflicting equal-sequence
 * copies fail closed; failure clears output and never manufactures authority. */
bool pet_ota_selection_parse(const uint8_t records[2][PET_OTA_RECORD_BYTES],pet_ota_selection_t *out);
bool pet_ota_selection_is(const pet_ota_selection_t *selection,unsigned slot,uint32_t state);
/* Includes INVALID/ABORTED records (which cannot be active). Missing is -1,
 * and does NOT prove the slot was never attempted by the bootloader. */
int pet_ota_selection_newest(const pet_ota_selection_t *selection,unsigned slot);
