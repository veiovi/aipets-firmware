#include "pet_journal.h"

#include <string.h>

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static bool erased(const uint8_t *p)
{
    for (unsigned i = 0; i < PET_JOURNAL_SECTOR_BYTES; ++i) if (p[i] != 0xff) return false;
    return true;
}

static bool valid(const uint8_t *record, uint64_t *generation, size_t *length)
{
    if (memcmp(record, "PJR1", 4) || u32(record + 4) != 1) return false;
    *generation = (uint64_t)u32(record + 8) | ((uint64_t)u32(record + 12) << 32);
    *length = u32(record + 16);
    return *generation && *length <= PET_JOURNAL_PAYLOAD_MAX &&
        u32(record + PET_JOURNAL_SECTOR_BYTES - 4) == crc32(record, PET_JOURNAL_SECTOR_BYTES - 4);
}

pet_journal_result_t pet_journal_open(pet_journal_t *journal, const pet_journal_io_t *io,
                                      void *payload, size_t capacity, size_t *length)
{
    if (!journal || !io || !io->read || !io->replace || !payload || !length)
        return PET_JOURNAL_ARGUMENT;
    pet_journal_io_t callbacks = *io;
    memset(journal, 0, sizeof(*journal));
    journal->io = callbacks;
    *length = 0;
    uint8_t record[PET_JOURNAL_SECTOR_BYTES];
    bool all_erased = true;
    uint32_t selected_crc = 0;
    for (unsigned sector = 0; sector < PET_JOURNAL_SECTORS; ++sector) {
        if (!callbacks.read(callbacks.context, sector, record)) return PET_JOURNAL_IO;
        all_erased = all_erased && erased(record);
        uint64_t generation;
        size_t bytes;
        if (!valid(record, &generation, &bytes)) continue;
        uint32_t crc = u32(record + PET_JOURNAL_SECTOR_BYTES - 4);
        if (generation == journal->generation && selected_crc != crc) return PET_JOURNAL_CORRUPT;
        if (generation > journal->generation) {
            if (bytes > capacity) return PET_JOURNAL_ARGUMENT;
            memcpy(payload, record + 20, bytes);
            *length = bytes;
            journal->generation = generation;
            journal->newest_sector = sector;
            selected_crc = crc;
        }
    }
    journal->loaded = journal->generation || all_erased;
    if (!journal->loaded) return PET_JOURNAL_CORRUPT;
    return journal->generation ? PET_JOURNAL_OK : PET_JOURNAL_EMPTY;
}

pet_journal_result_t pet_journal_save(pet_journal_t *journal, const void *payload, size_t length)
{
    if (!journal || !journal->loaded || !payload || length > PET_JOURNAL_PAYLOAD_MAX)
        return PET_JOURNAL_ARGUMENT;
    if (journal->generation == UINT64_MAX) return PET_JOURNAL_CORRUPT;
    uint8_t record[PET_JOURNAL_SECTOR_BYTES];
    memset(record, 0xff, sizeof(record));
    memcpy(record, "PJR1", 4);
    put32(record + 4, 1);
    uint64_t generation = journal->generation + 1;
    put32(record + 8, (uint32_t)generation);
    put32(record + 12, (uint32_t)(generation >> 32));
    put32(record + 16, (uint32_t)length);
    memcpy(record + 20, payload, length);
    put32(record + sizeof(record) - 4, crc32(record, sizeof(record) - 4));
    unsigned next = journal->generation ? (journal->newest_sector + 1) % PET_JOURNAL_SECTORS : 0;
    if (!journal->io.replace(journal->io.context, next, record)) {
        journal->loaded = false; /* uncertain write: must reread before continuing */
        return PET_JOURNAL_IO;
    }
    journal->generation = generation;
    journal->newest_sector = next;
    return PET_JOURNAL_OK;
}
