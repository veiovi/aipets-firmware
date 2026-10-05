#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 64 KiB journal, one complete record per independent 4 KiB erase sector.
 * Append around the ring: never erase the newest committed record. */
#define PET_JOURNAL_SECTOR_BYTES 4096
#define PET_JOURNAL_SECTORS 16
#define PET_JOURNAL_PAYLOAD_MAX (PET_JOURNAL_SECTOR_BYTES - 24)

typedef struct {
    bool (*read)(void *context, unsigned sector, uint8_t *record);
    /* Erase exactly this sector, write record, then read back/verify. */
    bool (*replace)(void *context, unsigned sector, const uint8_t *record);
    void *context;
} pet_journal_io_t;
typedef struct {
    pet_journal_io_t io;
    uint64_t generation;
    unsigned newest_sector;
    bool loaded;
} pet_journal_t;
typedef enum { PET_JOURNAL_OK, PET_JOURNAL_EMPTY, PET_JOURNAL_IO,
               PET_JOURNAL_CORRUPT, PET_JOURNAL_ARGUMENT } pet_journal_result_t;

pet_journal_result_t pet_journal_open(pet_journal_t *journal, const pet_journal_io_t *io,
                                      void *payload, size_t capacity, size_t *length);
pet_journal_result_t pet_journal_save(pet_journal_t *journal, const void *payload, size_t length);
