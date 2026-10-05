#include "pet_slot_store.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "frame_player.h"
#include "pet_flash_layout_esp.h"

/* A slot's install area: its pack area and, after it, its signed record. */
#define SLOT_INSTALL_BYTES (PET_LAYOUT_THREE_SLOT_PACK_BYTES + PET_REPLACE_MANIFEST_BYTES)

/* A 3 MB copy is split into slices so it never holds a core for long. */
#define COPY_SLICE_BYTES 0x10000u

static bool journal_read(void *context, unsigned sector, uint8_t *record)
{
    pet_slot_store_t *s = context;
    return sector < PET_JOURNAL_SECTORS &&
        esp_partition_read(s->journal, sector * PET_JOURNAL_SECTOR_BYTES, record, PET_JOURNAL_SECTOR_BYTES) == ESP_OK;
}

static bool journal_replace(void *context, unsigned sector, const uint8_t *record)
{
    pet_slot_store_t *s = context;
    if (sector >= PET_JOURNAL_SECTORS) return false;
    size_t offset = sector * PET_JOURNAL_SECTOR_BYTES;
    if (esp_partition_erase_range(s->journal, offset, PET_JOURNAL_SECTOR_BYTES) != ESP_OK ||
        esp_partition_write(s->journal, offset, record, PET_JOURNAL_SECTOR_BYTES) != ESP_OK) return false;
    uint8_t buffer[512];
    for (size_t at = 0; at < PET_JOURNAL_SECTOR_BYTES; at += sizeof(buffer))
        if (esp_partition_read(s->journal, offset + at, buffer, sizeof(buffer)) != ESP_OK ||
            memcmp(buffer, record + at, sizeof(buffer))) return false;
    return true;
}

/* `bytes` of the pet store at `offset`, copied through the flash driver (see
 * pet_slot_store_t) and hashed as they arrive. */
static esp_err_t copy_verified(const pet_slot_store_t *s, uint32_t offset, size_t bytes, const char *expected,
                               uint8_t *copy)
{
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    esp_err_t error = mbedtls_sha256_starts(&sha, 0) ? ESP_FAIL : ESP_OK;
    for (size_t at = 0; error == ESP_OK && at < bytes; at += COPY_SLICE_BYTES) {
        size_t length = bytes - at < COPY_SLICE_BYTES ? bytes - at : COPY_SLICE_BYTES;
        error = esp_partition_read(s->store, offset + at, copy + at, length);
        if (error == ESP_OK && mbedtls_sha256_update(&sha, copy + at, length)) error = ESP_FAIL;
        vTaskDelay(1);
    }
    uint8_t digest[32];
    if (error == ESP_OK && mbedtls_sha256_finish(&sha, digest)) error = ESP_FAIL;
    mbedtls_sha256_free(&sha);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; error == ESP_OK && i < sizeof(digest); ++i)
        if (expected[2 * i] != hex[digest[i] >> 4] || expected[2 * i + 1] != hex[digest[i] & 15]) error = ESP_ERR_INVALID_CRC;
    return error;
}

esp_err_t pet_slot_store_open(pet_slot_store_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));
    esp_err_t error = pet_flash_layout_read(&s->layout, s->partition_sha256);
    if (error != ESP_OK) return error;
    if (s->layout.id != PET_LAYOUT_THREE_3P5M || s->layout.pet_slots != PET_SLOT_COUNT) return ESP_ERR_INVALID_STATE;
    s->journal = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "pet_journal");
    s->store = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x43, "pet_store");
    if (!s->journal || !s->store || s->journal->address != s->layout.journal_offset || s->journal->size != 0x10000 ||
        s->store->address != s->layout.pet_offset || s->store->size != s->layout.pet_partition_bytes)
        return ESP_ERR_INVALID_STATE;
    const pet_journal_io_t io = {.read = journal_read, .replace = journal_replace, .context = s};
    if (pet_slot_journal_open(&s->inventory, &io) != PET_JOURNAL_OK) return ESP_ERR_INVALID_CRC;
    s->open = true;
    return ESP_OK;
}

esp_err_t pet_slot_store_load(pet_slot_store_t *s, unsigned slot, void *buffer, size_t capacity, size_t *bytes)
{
    if (!s || !s->open || slot >= PET_SLOT_COUNT || !buffer || !bytes) return ESP_ERR_INVALID_ARG;
    const pet_slot_t *record = &s->inventory.state.slots[slot];
    uint32_t pack_offset = 0, manifest_offset = 0;
    if (record->state != PET_SLOT_READY || !record->pack.bytes || record->pack.bytes > PET_LAYOUT_THREE_SLOT_PACK_BYTES ||
        !pet_flash_layout_slot(&s->layout, slot, &pack_offset, &manifest_offset)) return ESP_ERR_INVALID_STATE;
    if (record->pack.bytes > capacity) return ESP_ERR_INVALID_SIZE;
    esp_err_t error = copy_verified(s, pack_offset, record->pack.bytes, record->pack.sha256, buffer);
    if (error == ESP_OK) *bytes = record->pack.bytes;
    return error;
}

esp_err_t pet_slot_store_read_record(pet_slot_store_t *s, unsigned slot, void *record, size_t bytes)
{
    if (!s || !s->open || slot >= PET_SLOT_COUNT || !record || bytes != PET_REPLACE_MANIFEST_BYTES) return ESP_ERR_INVALID_ARG;
    const pet_slot_inventory_t *inventory = &s->inventory.state;
    const pet_slot_status_t state = inventory->slots[slot].state;
    uint32_t pack_offset = 0, record_offset = 0;
    if ((state != PET_SLOT_READY && (state != PET_SLOT_INSTALLING || (int)slot != inventory->target)) ||
        !pet_flash_layout_slot(&s->layout, slot, &pack_offset, &record_offset)) return ESP_ERR_INVALID_STATE;
    return esp_partition_read(s->store, record_offset, record, bytes);
}

/* Keep the installer's copy of the machine equal to the inventory's. */
static void sync_machine(pet_slot_store_t *s)
{
    if (!s->installer) return;
    s->machine.state = s->inventory.state.operation;
    s->machine.journal.generation = s->inventory.journal.generation;
}

esp_err_t pet_slot_store_commit(pet_slot_store_t *s, const pet_slot_inventory_t *next)
{
    if (!s || !s->open || !next) return ESP_ERR_INVALID_ARG;
    esp_err_t result = pet_slot_journal_commit(&s->inventory, next) == PET_JOURNAL_OK ? ESP_OK : ESP_FAIL;
    sync_machine(s);
    return result;
}

/* An install step: the machine's next state, applied to the whole inventory.
 * Its commit follows the control worker's `verify`, which checked every frame
 * with this firmware's validator. */
static pet_journal_result_t machine_commit(void *context, const pet_replace_t *next)
{
    pet_slot_store_t *s = context;
    pet_slot_inventory_t inventory = s->inventory.state;
    if (next->phase == PET_REPLACE_REQUESTED && s->has_replacement_target &&
        (next->target.bytes != s->replacement_incoming.bytes || strcmp(next->target.build_id,s->replacement_incoming.build_id) ||
         strcmp(next->target.sha256,s->replacement_incoming.sha256))) return PET_JOURNAL_ARGUMENT;
    if (!pet_slot_inventory_apply_target(&inventory, next, FP_VALIDATOR_REVISION,
          s->has_replacement_target ? &s->replacement_target : NULL)) return PET_JOURNAL_ARGUMENT;
    pet_journal_result_t result = pet_slot_journal_commit(&s->inventory, &inventory);
    s->machine.journal.generation = s->inventory.journal.generation;
    return result;
}

bool pet_slot_store_prepare_replacement(pet_slot_store_t *s, const pet_replace_pack_t *incoming,
                                        const pet_replace_pack_t *replacement)
{
    if (!s || !s->open || !s->inventory.ready || !incoming) return false;
    s->has_replacement_target = false;
    if (!replacement) return true; /* Legacy/new-pet allocation is unchanged. */
    const pet_slot_inventory_t *inv = &s->inventory.state;
    if (inv->target >= 0) {
        /* A retry uses its already durable slot, never a different recovery's. */
        const pet_replace_pack_t *pending = &inv->operation.target;
        return pending->bytes == incoming->bytes && !strcmp(pending->build_id,incoming->build_id) &&
            !strcmp(pending->sha256,incoming->sha256);
    }
    if (pet_slot_inventory_find(inv,replacement) < 0) return false;
    s->replacement_target = *replacement; s->replacement_incoming = *incoming;
    s->has_replacement_target = true;
    return true;
}

/* Offset of `bytes` at `offset` in the target slot's install area. */
static bool target_offset(const pet_slot_store_t *s, uint32_t offset, size_t bytes, size_t *at)
{
    int target = s->inventory.state.target;
    uint32_t pack_offset = 0, record_offset = 0;
    if (target < 0 || !pet_flash_layout_slot(&s->layout, (unsigned)target, &pack_offset, &record_offset) ||
        record_offset != pack_offset + PET_LAYOUT_THREE_SLOT_PACK_BYTES ||
        offset > SLOT_INSTALL_BYTES || bytes > SLOT_INSTALL_BYTES - offset) return false;
    *at = (size_t)pack_offset + offset;
    return true;
}
/* Never a pet anyone can see or talk to. */
static bool target_writable(const pet_slot_store_t *s)
{
    const pet_slot_inventory_t *inventory = &s->inventory.state;
    int target = inventory->target;
    return target >= 0 && target != inventory->active && target != inventory->bound;
}
static bool read_target(void *context, uint32_t offset, void *data, size_t bytes)
{
    pet_slot_store_t *s = context;
    size_t at = 0;
    return target_offset(s, offset, bytes, &at) && esp_partition_read(s->store, at, data, bytes) == ESP_OK;
}
static bool erase_target(void *context, uint32_t offset, size_t bytes)
{
    pet_slot_store_t *s = context;
    size_t at = 0;
    return target_writable(s) && target_offset(s, offset, bytes, &at) &&
        esp_partition_erase_range(s->store, at, bytes) == ESP_OK;
}
static bool write_target(void *context, uint32_t offset, const void *data, size_t bytes)
{
    pet_slot_store_t *s = context;
    size_t at = 0;
    return target_writable(s) && target_offset(s, offset, bytes, &at) &&
        esp_partition_write(s->store, at, data, bytes) == ESP_OK;
}
static void freeze(void *context)
{
    pet_slot_store_t *s = context;
    s->readers.freeze(s->readers.context);
}
static bool detach(void *context)
{
    pet_slot_store_t *s = context;
    int target = s->inventory.state.target;
    return target >= 0 && target != s->inventory.state.active && s->readers.detach(s->readers.context);
}

esp_err_t pet_slot_store_attach_installer(pet_slot_store_t *s, const pet_slot_readers_t *readers)
{
    if (!s || !s->open || !readers || !readers->freeze || !readers->detach) return ESP_ERR_INVALID_ARG;
    if (s->installer) return ESP_ERR_INVALID_STATE;
    s->readers = *readers;
    if (!pet_replace_journal_attach(&s->machine, &s->inventory.state.operation, s->inventory.journal.generation,
                                    machine_commit, s)) return ESP_ERR_INVALID_STATE;
    const pet_replace_io_t io = {.read = read_target, .erase = erase_target, .write = write_target,
        .freeze = freeze, .detach = detach, .context = s, .partition_bytes = SLOT_INSTALL_BYTES};
    if (!pet_replace_writer_init(&s->writer, &s->machine, &io)) return ESP_FAIL;
    s->installer = true;
    return ESP_OK;
}

esp_err_t pet_slot_store_load_target(pet_slot_store_t *s, void *buffer, size_t capacity, size_t *bytes)
{
    if (!s || !s->installer || !buffer || !bytes) return ESP_ERR_INVALID_ARG;
    const pet_replace_t *m = &s->inventory.state.operation;
    int target = s->inventory.state.target;
    bool complete = (m->phase == PET_REPLACE_DOWNLOADING && m->downloaded_bytes == m->target.bytes) ||
        m->phase == PET_REPLACE_VERIFIED || m->phase == PET_REPLACE_ACTIVATING;
    uint32_t pack_offset = 0, record_offset = 0;
    if (target < 0 || !complete || !m->target.bytes || m->target.bytes > PET_LAYOUT_THREE_SLOT_PACK_BYTES ||
        !pet_flash_layout_slot(&s->layout, (unsigned)target, &pack_offset, &record_offset))
        return ESP_ERR_INVALID_STATE;
    if (m->target.bytes > capacity) return ESP_ERR_INVALID_SIZE;
    s->writer.detached = s->writer.resume_checked = false;
    esp_err_t error = copy_verified(s, pack_offset, m->target.bytes, m->target.sha256, buffer);
    if (error == ESP_OK) *bytes = m->target.bytes;
    return error;
}
