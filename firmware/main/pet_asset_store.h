#pragma once

#include "esp_err.h"
#include "esp_partition.h"
#include "pet_journal.h"
#include "pet_control_wire.h"

#define PET_ASSET_SLOT_BYTES 0x3e0000u
#define PET_ASSET_MANIFEST_BYTES 8192u
#define PET_ASSET_MANIFEST_OFFSET (PET_ASSET_SLOT_BYTES-PET_ASSET_MANIFEST_BYTES)

typedef struct {
    bool initialized;
    const esp_partition_t *journal_partition, *slots[2];
    pet_journal_t journal;
    pet_install_t state;
    bool mapped[2];
    esp_partition_mmap_handle_t map_handles[2];
    const void *map_addresses[2];
} pet_asset_store_t;

/* One control worker owns writes. Renderers hold read-only mmap until unmap.
 * Refuses old/incorrect partition maps; never formats or migrates automatically.
 * Caller must zero-initialize once. Reopening requires explicit close after all
 * renderers have detached; open never discards live mapping handles. */
esp_err_t pet_asset_store_open(pet_asset_store_t *store);
void pet_asset_store_close(pet_asset_store_t *store);
esp_err_t pet_asset_store_commit(pet_asset_store_t *store,const pet_install_t *next);
esp_err_t pet_asset_store_write_block(pet_asset_store_t *store,const void *data,size_t bytes);
esp_err_t pet_asset_store_save_manifest(pet_asset_store_t *store,const pet_control_operation_t *operation);
esp_err_t pet_asset_store_load_manifest(pet_asset_store_t *store,unsigned slot,pet_control_operation_t *operation);
esp_err_t pet_asset_store_map(pet_asset_store_t *store,unsigned slot,const void **address,size_t *bytes);
void pet_asset_store_unmap(pet_asset_store_t *store,unsigned slot);
