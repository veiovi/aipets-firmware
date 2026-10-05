#include "pet_single_store.h"
#include <string.h>

static bool journal_read(void *context, unsigned sector, uint8_t *record)
{
    pet_single_store_t *s = context;
    return s->layout_valid && sector < PET_JOURNAL_SECTORS &&
        esp_partition_read(s->journal_partition, sector * PET_JOURNAL_SECTOR_BYTES,
                           record, PET_JOURNAL_SECTOR_BYTES) == ESP_OK;
}
static bool journal_write(void *context, unsigned sector, const uint8_t *record)
{
    pet_single_store_t *s = context;
    if (!s->layout_valid || sector >= PET_JOURNAL_SECTORS) return false;
    size_t offset = sector * PET_JOURNAL_SECTOR_BYTES;
    if (esp_partition_erase_range(s->journal_partition, offset, PET_JOURNAL_SECTOR_BYTES) != ESP_OK ||
        esp_partition_write(s->journal_partition, offset, record, PET_JOURNAL_SECTOR_BYTES) != ESP_OK) return false;
    uint8_t buffer[512];
    for (size_t at = 0; at < PET_JOURNAL_SECTOR_BYTES; at += sizeof(buffer))
        if (esp_partition_read(s->journal_partition, offset + at, buffer, sizeof(buffer)) != ESP_OK ||
            memcmp(buffer, record + at, sizeof(buffer))) return false;
    return true;
}
static bool read_pet(void *context, uint32_t offset, void *data, size_t bytes)
{
    pet_single_store_t *s = context;
    return s->layout_valid && esp_partition_read(s->pet_partition, offset, data, bytes) == ESP_OK;
}
/* Every attempt to change the pet partition, from any store instance. */
static uint32_t s_writes;
uint32_t pet_single_store_writes(void) { return s_writes; }
static bool erase_pet(void *context, uint32_t offset, size_t bytes)
{
    pet_single_store_t *s = context;
    ++s_writes;
    return s->layout_valid && !s->mapped && esp_partition_erase_range(s->pet_partition, offset, bytes) == ESP_OK;
}
static bool write_pet(void *context, uint32_t offset, const void *data, size_t bytes)
{
    pet_single_store_t *s = context;
    ++s_writes;
    return s->layout_valid && !s->mapped && esp_partition_write(s->pet_partition, offset, data, bytes) == ESP_OK;
}
static void freeze(void *context)
{ pet_single_store_t *s = context; s->readers.freeze(s->readers.context); }
bool pet_single_store_detach(pet_single_store_t *s)
{
    if (!s || !s->initialized || !s->readers.detach || !s->readers.detach(s->readers.context)) return false;
    if (s->mapped) esp_partition_munmap(s->map_handle);
    s->mapped = false; s->map_address = NULL; s->map_handle = 0; return true;
}
static bool detach(void *context) { return pet_single_store_detach(context); }
esp_err_t pet_single_store_open(pet_single_store_t *s, const pet_single_readers_t *readers)
{
    if (!s || !readers || !readers->freeze || !readers->detach) return ESP_ERR_INVALID_ARG;
    if (s->initialized) return ESP_ERR_INVALID_STATE;
    pet_single_readers_t callbacks = *readers;
    memset(s, 0, sizeof(*s)); s->readers = callbacks;
    esp_err_t error = pet_flash_layout_read(&s->layout, s->partition_sha256);
    if (error != ESP_OK) return error;
    if (s->layout.pet_slots != 1) return ESP_ERR_INVALID_STATE;
    s->journal_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "pet_journal");
    s->pet_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "pet_pack");
    if (!s->journal_partition || !s->pet_partition || s->journal_partition->address != s->layout.journal_offset ||
        s->journal_partition->size != 0x10000 || s->pet_partition->address != s->layout.pet_offset ||
        s->pet_partition->size != s->layout.pet_partition_bytes) return ESP_ERR_INVALID_STATE;
    s->layout_valid = s->initialized = true;
    const pet_journal_io_t journal_io = {.read=journal_read,.replace=journal_write,.context=s};
    if (pet_replace_journal_open(&s->state, &journal_io, s->layout.pack_capacity_bytes) != PET_JOURNAL_OK)
        return ESP_ERR_INVALID_CRC;
    const pet_replace_io_t io = {.read=read_pet,.erase=erase_pet,.write=write_pet,
        .freeze=freeze,.detach=detach,.context=s,.partition_bytes=s->layout.pet_partition_bytes};
    return pet_replace_writer_init(&s->writer, &s->state, &io) ? ESP_OK : ESP_FAIL;
}
esp_err_t pet_single_store_map(pet_single_store_t *s, const void **address, size_t *bytes)
{
    if (!s || !s->initialized || !s->layout_valid || !s->state.ready || !s->state.journal.loaded ||
        !pet_replace_valid(&s->state.state) || !address || !bytes) return ESP_ERR_INVALID_STATE;
    const pet_replace_t *state = &s->state.state; uint32_t length = 0;
    if (pet_replace_has_active(state)) length = state->active.bytes;
    else if ((state->phase == PET_REPLACE_DOWNLOADING && state->downloaded_bytes == state->target.bytes) ||
        state->phase == PET_REPLACE_VERIFIED || state->phase == PET_REPLACE_ACTIVATING) length = state->target.bytes;
    if (!length || length > s->layout.pack_capacity_bytes) return ESP_ERR_INVALID_STATE;
    if (!s->mapped) {
        esp_err_t error = esp_partition_mmap(s->pet_partition, 0, length, ESP_PARTITION_MMAP_DATA,
                                             &s->map_address, &s->map_handle);
        if (error != ESP_OK) return error;
        s->mapped = true;
        /* Mapping invalidates write authority even if the journal is later
         * changed. A fresh resume must invoke real renderer detach again. */
        s->writer.detached = s->writer.resume_checked = false;
    }
    *address = s->map_address; *bytes = length; return ESP_OK;
}
esp_err_t pet_single_store_read_manifest(pet_single_store_t *s, void *record, size_t bytes)
{
    if (!s || !s->layout_valid || !record || bytes != PET_REPLACE_MANIFEST_BYTES) return ESP_ERR_INVALID_ARG;
    return esp_partition_read(s->pet_partition, s->layout.pack_capacity_bytes, record, bytes);
}
bool pet_single_store_close(pet_single_store_t *s)
{
    if (!s) return false;
    if (s->initialized && !pet_single_store_detach(s)) return false;
    pet_replace_writer_close(&s->writer); memset(s, 0, sizeof(*s)); return true;
}
