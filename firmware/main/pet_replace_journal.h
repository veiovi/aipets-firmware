#pragma once
#include "pet_journal.h"
#include "pet_replace.h"

/* Explicit little-endian fields and zero-padded strings, never C ABI bytes.
 * An outer PJR1 record supplies generation, CRC and sector-atomic recovery. */
#define PET_REPLACE_RECORD_BYTES 684u
bool pet_replace_encode(const pet_replace_t *state, void *record, size_t bytes);
bool pet_replace_decode(const void *record, size_t bytes, pet_replace_t *state);
typedef struct {
    pet_journal_t journal;
    pet_replace_t state;
    bool ready;
    /* Set when another journaled record carries the machine (the three-pet
     * inventory). Commits go through it, and `journal` mirrors that record's
     * journal: outer_commit keeps journal.generation current. */
    pet_journal_result_t (*outer_commit)(void *context, const pet_replace_t *next);
    void *outer_context;
} pet_replace_journal_t;
/* Expected capacity comes from validated physical partitions, not the journal.
 * Corrupt or legacy journals fail closed; this never formats them. */
pet_journal_result_t pet_replace_journal_open(pet_replace_journal_t *store,
                                              const pet_journal_io_t *io, uint32_t capacity);
pet_journal_result_t pet_replace_journal_commit(pet_replace_journal_t *store,
                                                const pet_replace_t *next);
/* A machine persisted inside an outer record whose journal is loaded at
 * `generation`. Its state is the record's copy; nothing is written here. */
bool pet_replace_journal_attach(pet_replace_journal_t *store, const pet_replace_t *state, uint64_t generation,
                                pet_journal_result_t (*outer_commit)(void *context, const pet_replace_t *next),
                                void *context);
