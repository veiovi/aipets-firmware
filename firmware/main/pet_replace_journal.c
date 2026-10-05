#include "pet_replace_journal.h"
#include <string.h>

static uint32_t get32(const uint8_t **at)
{
    const uint8_t *p = *at; *at += 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t **at, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) *(*at)++ = (uint8_t)(value >> (i * 8)); }
static void put_text(uint8_t **at, const char *text, size_t size)
{ memset(*at, 0, size); memcpy(*at, text, strlen(text)); *at += size; }
static bool get_text(const uint8_t **at, char *text, size_t size)
{
    const uint8_t *p = *at, *end = memchr(p, 0, size); if (!end) return false;
    for (const uint8_t *q = end; q < p + size; ++q) if (*q) return false;
    memcpy(text, p, size); *at += size; return true;
}
static void put_pack(uint8_t **at, const pet_replace_pack_t *pack)
{ put_text(at, pack->build_id, 37); put_text(at, pack->sha256, 65); put32(at, pack->bytes); }
static bool get_pack(const uint8_t **at, pet_replace_pack_t *pack)
{
    if (!get_text(at, pack->build_id, 37) || !get_text(at, pack->sha256, 65)) return false;
    pack->bytes = get32(at); return true;
}
bool pet_replace_encode(const pet_replace_t *s, void *record, size_t bytes)
{
    if (!record || bytes != PET_REPLACE_RECORD_BYTES || !pet_replace_valid(s)) return false;
    uint8_t *at = record; memcpy(at, "PRS2", 4); at += 4;
    put32(&at, s->capacity_bytes); put32(&at, (uint32_t)s->phase); put32(&at, s->downloaded_bytes);
    put_pack(&at, &s->active); put_pack(&at, &s->target);
    put_text(&at, s->request_id, 37); put_text(&at, s->operation_id, 37); put_text(&at, s->fence_id, 37);
    put_text(&at, s->expected_revision, 81); put_text(&at, s->binding_revision, 81);
    put_text(&at, s->relationship_id, 37); put_text(&at, s->config_version, 81); put_text(&at, s->prefix_sha256, 65);
    return (size_t)(at - (uint8_t *)record) == bytes;
}
bool pet_replace_decode(const void *record, size_t bytes, pet_replace_t *s)
{
    if (!record || !s || bytes != PET_REPLACE_RECORD_BYTES || memcmp(record, "PRS2", 4)) return false;
    const uint8_t *at = (const uint8_t *)record + 4; pet_replace_t parsed = {0};
    parsed.capacity_bytes = get32(&at); uint32_t phase = get32(&at);
    if (phase > PET_REPLACE_RECOVERY) return false;
    parsed.phase = (pet_replace_phase_t)phase; parsed.downloaded_bytes = get32(&at);
    if (!get_pack(&at, &parsed.active) || !get_pack(&at, &parsed.target) ||
        !get_text(&at, parsed.request_id, 37) || !get_text(&at, parsed.operation_id, 37) ||
        !get_text(&at, parsed.fence_id, 37) || !get_text(&at, parsed.expected_revision, 81) ||
        !get_text(&at, parsed.binding_revision, 81) || !get_text(&at, parsed.relationship_id, 37) ||
        !get_text(&at, parsed.config_version, 81) || !get_text(&at, parsed.prefix_sha256, 65) ||
        !pet_replace_valid(&parsed)) return false;
    *s = parsed; return true;
}
pet_journal_result_t pet_replace_journal_commit(pet_replace_journal_t *s, const pet_replace_t *next)
{
    uint8_t record[PET_REPLACE_RECORD_BYTES];
    if (!s || !s->ready || !s->journal.loaded || !next || next->capacity_bytes != s->state.capacity_bytes ||
        !pet_replace_encode(next, record, sizeof(record))) return PET_JOURNAL_ARGUMENT;
    pet_journal_result_t result = s->outer_commit ? s->outer_commit(s->outer_context, next)
                                                  : pet_journal_save(&s->journal, record, sizeof(record));
    if (result == PET_JOURNAL_OK) s->state = *next;
    else s->ready = false; /* A failed read-back is an ambiguous commit; reopen. */
    return result;
}
bool pet_replace_journal_attach(pet_replace_journal_t *s, const pet_replace_t *state, uint64_t generation,
                                pet_journal_result_t (*outer_commit)(void *context, const pet_replace_t *next),
                                void *context)
{
    if (!s || !state || !generation || !outer_commit || !pet_replace_valid(state)) return false;
    memset(s, 0, sizeof(*s));
    s->state = *state;
    s->journal.loaded = true;
    s->journal.generation = generation;
    s->outer_commit = outer_commit;
    s->outer_context = context;
    s->ready = true;
    return true;
}
pet_journal_result_t pet_replace_journal_open(pet_replace_journal_t *s,
                                              const pet_journal_io_t *io, uint32_t capacity)
{
    if (!s || !io || !pet_replace_capacity_valid(capacity)) return PET_JOURNAL_ARGUMENT;
    pet_journal_io_t callbacks = *io;
    memset(s, 0, sizeof(*s)); uint8_t record[PET_REPLACE_RECORD_BYTES]; size_t bytes = 0;
    pet_journal_result_t result = pet_journal_open(&s->journal, &callbacks, record, sizeof(record), &bytes);
    if (result == PET_JOURNAL_EMPTY) {
        pet_replace_empty(&s->state, capacity); s->ready = true;
        /* Redundant baseline before any cloud selection or flash mutation. */
        result = pet_replace_journal_commit(s, &s->state);
        if (result == PET_JOURNAL_OK) result = pet_replace_journal_commit(s, &s->state);
        return result;
    }
    if (result != PET_JOURNAL_OK) return result;
    if (!pet_replace_decode(record, bytes, &s->state) || s->state.capacity_bytes != capacity) {
        s->journal.loaded = false; return PET_JOURNAL_CORRUPT;
    }
    s->ready = true; return PET_JOURNAL_OK;
}
