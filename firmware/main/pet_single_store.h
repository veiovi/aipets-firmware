#pragma once
#include "esp_partition.h"
#include "pet_flash_layout_esp.h"
#include "pet_replace_writer.h"

typedef struct {
    void (*freeze)(void *context);
    bool (*detach)(void *context);
    void *context;
} pet_single_readers_t;
typedef struct {
    bool initialized, layout_valid, mapped;
    pet_flash_layout_t layout;
    char partition_sha256[65];
    const esp_partition_t *journal_partition, *pet_partition;
    pet_replace_journal_t state;
    pet_replace_writer_t writer;
    pet_single_readers_t readers;
    esp_partition_mmap_handle_t map_handle;
    const void *map_address;
} pet_single_store_t;

/* Only a validated single-pet layout. Callers keep independent HTTPS control
 * alive when this returns an error; corrupt journals are never autoformatted.
 * Zero-initialize once; close after detaching before reopening. Every operation
 * (including map, journal transitions and close) belongs to the one serialized
 * control worker; rendering may only read a pinned mapping. */
esp_err_t pet_single_store_open(pet_single_store_t *store, const pet_single_readers_t *readers);
/* Verification may map only completely downloaded or active data. No mapping
 * implies trusted/approved/activated content; signature/hash/frame checks follow. */
esp_err_t pet_single_store_map(pet_single_store_t *store, const void **address, size_t *bytes);
/* Renderer detach must have succeeded before unmapping. */
bool pet_single_store_detach(pet_single_store_t *store);
bool pet_single_store_close(pet_single_store_t *store);
/* Counts every erase or write of the pet partition since boot. Only this store
 * writes it, so an unchanged count means the flash holds the same bytes. */
uint32_t pet_single_store_writes(void);
esp_err_t pet_single_store_read_manifest(pet_single_store_t *store, void *record, size_t bytes);
