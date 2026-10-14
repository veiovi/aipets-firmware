#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "pet_flash_layout.h"
#include "pet_replace_writer.h"
#include "pet_slot_inventory.h"
#include "frame_stream.h"

/* What an installation needs from the renderer and the conversation. */
typedef struct {
    void (*freeze)(void *context); /* Stop conversation and audio; idempotent. */
    bool (*detach)(void *context); /* Hold nothing of the target slot. */
    void *context;
} pet_slot_readers_t;

typedef struct
{
    const esp_partition_t *partition;
    uint32_t offset, bytes;
} pet_slot_source_t;

/*
 * The Pocket Terminal's one-pet store (one-pet-10m-v5). The control task owns
 * the store. Open it once per boot and keep it at a fixed address:
 * its journal callbacks point back at it.
 *
 * Nothing reads a slot through a flash mapping. With code and read-only data
 * in PSRAM (CONFIG_SPIRAM_XIP_FROM_PSRAM), ESP-IDF keeps the cache enabled
 * while flash is written, and a mapped read during any write (NVS, Wi-Fi, a
 * slot installation) can return erased bytes. Hashes and validation stream
 * through the flash driver. Rendering only reads resident/pinned RAM.
 */
typedef struct {
    bool open;
    pet_flash_layout_t layout;
    char partition_sha256[65];
    const esp_partition_t *journal, *store;
    pet_slot_journal_t inventory;
    /* Installation (pet_slot_store_attach_installer). */
    bool installer;
    pet_slot_readers_t readers;
    pet_replace_journal_t machine;
    pet_replace_writer_t writer;
    /* Verified request hint. The selected index becomes durable in the
     * existing inventory before fencing; recovery uses that index. */
    bool has_replacement_target;
    pet_replace_pack_t replacement_target, replacement_incoming;
    pet_slot_source_t source;
} pet_slot_store_t;

/* Only the one-pet-10m-v5 layout opens. A never-written journal becomes an empty
 * inventory; a corrupt or foreign one fails closed and is never formatted. */
esp_err_t pet_slot_store_open(pet_slot_store_t *store);
/* Copy the pack of a ready slot into `buffer`, in slices that yield, and hash
 * the copy. A copy that differs from the slot's recorded SHA-256 is
 * ESP_ERR_INVALID_CRC and must not be shown; a pack larger than `capacity` is
 * ESP_ERR_INVALID_SIZE. */
esp_err_t pet_slot_store_load(pet_slot_store_t *store, unsigned slot, void *buffer, size_t capacity, size_t *bytes);
/* The signed release record of a ready slot, or of the target an
 * installation is writing: the 8 KiB after its pack area, written before the
 * pack's first byte. A pet placed over USB has none. */
esp_err_t pet_slot_store_read_record(pet_slot_store_t *store, unsigned slot, void *record, size_t bytes);
/* Persist a new inventory: a selection, a validator revision, an install step. */
esp_err_t pet_slot_store_commit(pet_slot_store_t *store, const pet_slot_inventory_t *next);

/* Run the v2 install machine and writer on the one-pet store, unchanged.
 * The machine is the inventory's `operation`: each step commits the whole
 * inventory through pet_slot_inventory_apply. Bytes go to the inventory's
 * target, slot 0, once invalidation has taken it off the screen and ended its
 * binding. */
esp_err_t pet_slot_store_attach_installer(pet_slot_store_t *store, const pet_slot_readers_t *readers);
bool pet_slot_store_prepare_replacement(pet_slot_store_t *store, const pet_replace_pack_t *incoming,
                                        const pet_replace_pack_t *replacement);
/* The completely downloaded target, copied for verification as a slot is by
 * pet_slot_store_load and checked against the requested SHA-256. Reading it
 * ends write authority until the writer resumes, as on a single-pet device. */
esp_err_t pet_slot_store_load_target(pet_slot_store_t *store, void *buffer, size_t capacity, size_t *bytes);

/* Hash through the flash driver, then prepare a bounded resident/cache view.
 * target requires the fully downloaded install slot and always validates every
 * resource. Ready slots revalidate after a validator revision change.
 * Caller has detached the old renderer and drained the media worker; source
 * and buffer remain pinned until it does so again. */
esp_err_t pet_slot_store_stream(pet_slot_store_t *store, unsigned slot, bool target,
    void *buffer, uint32_t capacity, uint32_t minimum_tail, fp_stream_t **stream);
