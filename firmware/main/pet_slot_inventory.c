#include "pet_slot_inventory.h"

#include <string.h>
#include "pet_flash_layout.h"
#include "pet_replace_journal.h"

#define NONE 0xffu

static bool text(const char *s, size_t capacity) { return s && s[0] && memchr(s, 0, capacity); }
static bool terminated(const char *s, size_t capacity) { return memchr(s, 0, capacity) != NULL; }
static bool uuid(const char *s)
{
    if (!text(s, 37) || strlen(s) != 36) return false;
    for (unsigned i = 0; i < 36; ++i)
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!strchr("0123456789abcdef", s[i])) return false;
    return true;
}
static bool hash(const char *s) { return text(s, 65) && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64; }
static bool empty_pack(const pet_replace_pack_t *p) { return !p->build_id[0] && !p->sha256[0] && !p->bytes; }
static bool same_pack(const pet_replace_pack_t *a, const pet_replace_pack_t *b)
{ return a->bytes == b->bytes && !strcmp(a->build_id, b->build_id) && !strcmp(a->sha256, b->sha256); }

static void clear_slot(pet_slot_t *slot, pet_slot_status_t state)
{
    memset(slot, 0, sizeof(*slot));
    slot->state = state;
}

static bool slot_valid(const pet_slot_t *s)
{
    if (!terminated(s->pack.build_id, sizeof(s->pack.build_id)) || !terminated(s->pack.sha256, sizeof(s->pack.sha256)))
        return false;
    if (s->state == PET_SLOT_READY)
        return uuid(s->pack.build_id) && hash(s->pack.sha256) && s->pack.bytes &&
            s->pack.bytes <= PET_LAYOUT_THREE_SLOT_PACK_BYTES && s->validator_revision;
    return (s->state == PET_SLOT_FREE || s->state == PET_SLOT_INSTALLING) && empty_pack(&s->pack) &&
        !s->validator_revision && !s->shown_at;
}

bool pet_slot_inventory_empty(pet_slot_inventory_t *inv)
{
    if (!inv) return false;
    memset(inv, 0, sizeof(*inv));
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i) clear_slot(&inv->slots[i], PET_SLOT_FREE);
    inv->active = inv->bound = inv->target = -1;
    return pet_replace_empty(&inv->operation, PET_LAYOUT_THREE_SLOT_PACK_BYTES);
}

static bool index_ok(int slot) { return slot >= -1 && slot < (int)PET_SLOT_COUNT; }

bool pet_slot_inventory_valid(const pet_slot_inventory_t *inv)
{
    if (!inv || !index_ok(inv->active) || !index_ok(inv->bound) || !index_ok(inv->target) ||
        !pet_replace_valid(&inv->operation) || inv->operation.capacity_bytes != PET_LAYOUT_THREE_SLOT_PACK_BYTES) return false;
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i) {
        if (!slot_valid(&inv->slots[i])) return false;
        if (inv->slots[i].state == PET_SLOT_INSTALLING && (int)i != inv->target) return false;
    }
    if (inv->active >= 0 && inv->slots[inv->active].state != PET_SLOT_READY) return false;
    const pet_replace_t *op = &inv->operation;
    /* The bound pet is installed exactly where the machine's pack says. */
    if (empty_pack(&op->active)) {
        if (inv->bound >= 0) return false;
    } else if (inv->bound < 0 || inv->slots[inv->bound].state != PET_SLOT_READY ||
               !same_pack(&inv->slots[inv->bound].pack, &op->active)) return false;
    switch (op->phase) {
        case PET_REPLACE_EMPTY:
        case PET_REPLACE_ACTIVE:
            return inv->target < 0;
        case PET_REPLACE_REQUESTED:
        case PET_REPLACE_FENCED:
            /* Before invalidation the target still holds its pet or nothing; a
             * retried or superseded attempt keeps rewriting the same slot. */
            return inv->target >= 0 &&
                (inv->slots[inv->target].state != PET_SLOT_INSTALLING || empty_pack(&op->active));
        case PET_REPLACE_INVALIDATED:
        case PET_REPLACE_DOWNLOADING:
        case PET_REPLACE_VERIFIED:
        case PET_REPLACE_ACTIVATING:
        case PET_REPLACE_RECOVERY:
            return inv->target >= 0 && inv->slots[inv->target].state == PET_SLOT_INSTALLING;
        default:
            return false;
    }
}

static void show(pet_slot_inventory_t *inv, int slot)
{
    inv->active = slot;
    inv->selection_revision++;
    if (slot >= 0) inv->slots[slot].shown_at = inv->selection_revision;
}

bool pet_slot_inventory_select(pet_slot_inventory_t *inv, unsigned slot)
{
    if (!pet_slot_inventory_valid(inv) || slot >= PET_SLOT_COUNT || inv->slots[slot].state != PET_SLOT_READY) return false;
    if (inv->active != (int)slot) show(inv, (int)slot);
    return true;
}

int pet_slot_inventory_step(const pet_slot_inventory_t *inv, int from, int direction)
{
    if (!pet_slot_inventory_valid(inv) || (direction != 1 && direction != -1) ||
        from < -1 || from >= (int)PET_SLOT_COUNT) return -1;
    int at = from < 0 ? (direction > 0 ? -1 : 0) : from;
    for (unsigned n = 0; n < PET_SLOT_COUNT; ++n) {
        at = (at + direction + (int)PET_SLOT_COUNT) % (int)PET_SLOT_COUNT;
        if (inv->slots[at].state == PET_SLOT_READY) return at;
    }
    return -1;
}

/* A free slot, else the pet already holding these exact bytes (such as the USB
 * copy of a pet now signed by the cloud), else the stalest installed pet. Never
 * the pet on screen or the bound one. */
static int choose_target(const pet_slot_inventory_t *inv, const pet_replace_pack_t *incoming)
{
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i)
        if (inv->slots[i].state == PET_SLOT_FREE) return (int)i;
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i)
        if (inv->slots[i].state == PET_SLOT_READY && (int)i != inv->active && (int)i != inv->bound &&
            !strcmp(inv->slots[i].pack.sha256, incoming->sha256)) return (int)i;
    int best = -1;
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i)
        if (inv->slots[i].state == PET_SLOT_READY && (int)i != inv->active && (int)i != inv->bound &&
            (best < 0 || inv->slots[i].shown_at < inv->slots[best].shown_at)) best = (int)i;
    return best;
}

static int latest_ready_other_than(const pet_slot_inventory_t *inv, int excluded)
{
    int best = -1;
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i)
        if ((int)i != excluded && inv->slots[i].state == PET_SLOT_READY &&
            (best < 0 || inv->slots[i].shown_at > inv->slots[best].shown_at)) best = (int)i;
    return best;
}

int pet_slot_inventory_find(const pet_slot_inventory_t *inv, const pet_replace_pack_t *pack)
{
    if (!pet_slot_inventory_valid(inv) || !pack || !uuid(pack->build_id) || !hash(pack->sha256) || !pack->bytes) return -1;
    int found = -1;
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i) if (inv->slots[i].state == PET_SLOT_READY && same_pack(&inv->slots[i].pack, pack)) {
        if (found >= 0) return -1;
        found = (int)i;
    }
    return found;
}

bool pet_slot_inventory_apply(pet_slot_inventory_t *inv, const pet_replace_t *op, uint16_t validator_revision)
{ return pet_slot_inventory_apply_target(inv, op, validator_revision, NULL); }

bool pet_slot_inventory_remove(pet_slot_inventory_t *inv, const pet_replace_pack_t *pack)
{
    if (!pet_slot_inventory_valid(inv) || inv->target >= 0 || !pack || !uuid(pack->build_id) ||
        !hash(pack->sha256) || !pack->bytes || pack->bytes > PET_REPLACE_MAX_BYTES)
        return false;
    int slot = -1;
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i)
    {
        if (inv->slots[i].state != PET_SLOT_READY || !same_pack(&inv->slots[i].pack, pack))
            continue;
        if (slot >= 0)
            return false; /* Ambiguous inventory cannot acknowledge absence. */
        slot = (int)i;
    }
    if (slot < 0)
        return true;
    pet_slot_inventory_t next = *inv;
    clear_slot(&next.slots[slot], PET_SLOT_FREE);
    if (next.bound == slot)
    {
        next.bound = -1;
        if (!pet_replace_empty(&next.operation, inv->operation.capacity_bytes))
            return false;
    }
    if (next.active == slot)
        show(&next, latest_ready_other_than(&next, slot));
    if (!pet_slot_inventory_valid(&next))
        return false;
    *inv = next;
    return true;
}

bool pet_slot_inventory_apply_target(pet_slot_inventory_t *inv, const pet_replace_t *op, uint16_t validator_revision,
                                     const pet_replace_pack_t *replacement)
{
    if (!pet_slot_inventory_valid(inv) || !pet_replace_valid(op) ||
        op->capacity_bytes != PET_LAYOUT_THREE_SLOT_PACK_BYTES) return false;
    const pet_replace_phase_t was = inv->operation.phase;
    pet_slot_inventory_t next = *inv;
    next.operation = *op;
    switch (op->phase) {
        case PET_REPLACE_EMPTY:
        case PET_REPLACE_ACTIVE:
            if (was == PET_REPLACE_ACTIVATING && op->phase == PET_REPLACE_ACTIVE) {
                /* A committed installation: ready, bound and shown. */
                if (!validator_revision) return false;
                pet_slot_t *t = &next.slots[next.target];
                t->state = PET_SLOT_READY; t->pack = op->active; t->validator_revision = validator_revision;
                next.bound = next.target;
                show(&next, next.target);
            } else if (was != PET_REPLACE_REQUESTED) {
                /* Rebinding is pet_slot_inventory_rebind; nothing else ends an operation. */
                return false;
            }
            next.target = -1;
            break;
        case PET_REPLACE_REQUESTED:
            if (inv->target >= 0) return false;
            next.target = replacement ? pet_slot_inventory_find(inv, replacement) : choose_target(&next, &op->target);
            if (next.target < 0) return false;
            break;
        case PET_REPLACE_FENCED:
            break;
        case PET_REPLACE_INVALIDATED:
        case PET_REPLACE_DOWNLOADING:
        case PET_REPLACE_VERIFIED:
        case PET_REPLACE_ACTIVATING:
        case PET_REPLACE_RECOVERY:
            /* The binding ended with invalidation (or a cancelled fence). The
             * previous pet keeps its slot; the target's content is gone. */
            next.bound = -1;
            if (next.slots[next.target].state != PET_SLOT_INSTALLING) {
                clear_slot(&next.slots[next.target], PET_SLOT_INSTALLING);
                if (next.active == next.target) show(&next, latest_ready_other_than(&next, next.target));
            }
            break;
        default:
            return false;
    }
    if (!pet_slot_inventory_valid(&next)) return false;
    *inv = next; return true;
}

bool pet_slot_inventory_rebind(pet_slot_inventory_t *inv, unsigned slot, const char *binding_revision,
                               const char *relationship_id, const char *config_version)
{
    if (!pet_slot_inventory_valid(inv) || inv->target >= 0 || slot >= PET_SLOT_COUNT ||
        inv->slots[slot].state != PET_SLOT_READY) return false;
    pet_slot_inventory_t next = *inv;
    if (!pet_replace_rebind(&next.operation, &inv->slots[slot].pack, binding_revision, relationship_id, config_version))
        return false;
    next.bound = (int)slot;
    if (!pet_slot_inventory_valid(&next)) return false;
    *inv = next; return true;
}

static void put8(uint8_t **at, uint8_t v) { *(*at)++ = v; }
static void put16(uint8_t **at, uint16_t v) { put8(at, (uint8_t)v); put8(at, (uint8_t)(v >> 8)); }
static void put32(uint8_t **at, uint32_t v) { put16(at, (uint16_t)v); put16(at, (uint16_t)(v >> 16)); }
static void put_text(uint8_t **at, const char *s, size_t size) { memset(*at, 0, size); memcpy(*at, s, strlen(s)); *at += size; }
static uint8_t get8(const uint8_t **at) { return *(*at)++; }
static uint16_t get16(const uint8_t **at) { uint16_t lo = get8(at); return (uint16_t)(lo | ((uint16_t)get8(at) << 8)); }
static uint32_t get32(const uint8_t **at) { uint32_t lo = get16(at); return lo | ((uint32_t)get16(at) << 16); }
static bool get_text(const uint8_t **at, char *s, size_t size)
{
    const uint8_t *p = *at, *end = memchr(p, 0, size);
    if (!end) return false;
    for (const uint8_t *q = end; q < p + size; ++q) if (*q) return false;
    memcpy(s, p, size); *at += size; return true;
}
static uint8_t index_byte(int slot) { return slot < 0 ? NONE : (uint8_t)slot; }
static bool index_read(uint8_t value, int *slot)
{
    if (value != NONE && value >= PET_SLOT_COUNT) return false;
    *slot = value == NONE ? -1 : value; return true;
}

/* Record v2: 16-byte header, three 114-byte slots, the 684-byte machine. */
bool pet_slot_inventory_encode(const pet_slot_inventory_t *inv, void *record, size_t bytes)
{
    if (!record || bytes != PET_SLOT_RECORD_BYTES || !pet_slot_inventory_valid(inv)) return false;
    uint8_t *at = record;
    memcpy(at, "PSI3", 4); at += 4;
    put16(&at, 2); put8(&at, PET_SLOT_COUNT); put8(&at, index_byte(inv->active));
    put32(&at, inv->selection_revision); put8(&at, index_byte(inv->target)); put8(&at, index_byte(inv->bound));
    put8(&at, 0); put8(&at, 0);
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i) {
        const pet_slot_t *s = &inv->slots[i];
        put8(&at, (uint8_t)s->state); put8(&at, 0); put16(&at, s->validator_revision); put32(&at, s->pack.bytes);
        put32(&at, s->shown_at); put_text(&at, s->pack.build_id, 37); put_text(&at, s->pack.sha256, 65);
    }
    if (!pet_replace_encode(&inv->operation, at, PET_REPLACE_RECORD_BYTES)) return false;
    at += PET_REPLACE_RECORD_BYTES;
    return (size_t)(at - (uint8_t *)record) == bytes;
}

bool pet_slot_inventory_decode(const void *record, size_t bytes, pet_slot_inventory_t *inv)
{
    if (!record || !inv || bytes != PET_SLOT_RECORD_BYTES || memcmp(record, "PSI3", 4)) return false;
    const uint8_t *at = (const uint8_t *)record + 4;
    pet_slot_inventory_t parsed = {0};
    if (get16(&at) != 2 || get8(&at) != PET_SLOT_COUNT || !index_read(get8(&at), &parsed.active)) return false;
    parsed.selection_revision = get32(&at);
    if (!index_read(get8(&at), &parsed.target) || !index_read(get8(&at), &parsed.bound) || get8(&at) || get8(&at))
        return false;
    for (unsigned i = 0; i < PET_SLOT_COUNT; ++i) {
        pet_slot_t *s = &parsed.slots[i];
        uint8_t state = get8(&at);
        if (state > PET_SLOT_READY || get8(&at)) return false;
        s->state = (pet_slot_status_t)state; s->validator_revision = get16(&at); s->pack.bytes = get32(&at);
        s->shown_at = get32(&at);
        if (!get_text(&at, s->pack.build_id, 37) || !get_text(&at, s->pack.sha256, 65)) return false;
    }
    if (!pet_replace_decode(at, PET_REPLACE_RECORD_BYTES, &parsed.operation) || !pet_slot_inventory_valid(&parsed)) return false;
    *inv = parsed; return true;
}

pet_journal_result_t pet_slot_journal_commit(pet_slot_journal_t *s, const pet_slot_inventory_t *next)
{
    uint8_t record[PET_SLOT_RECORD_BYTES];
    if (!s || !s->ready || !s->journal.loaded || !next || !pet_slot_inventory_encode(next, record, sizeof(record)))
        return PET_JOURNAL_ARGUMENT;
    pet_journal_result_t result = pet_journal_save(&s->journal, record, sizeof(record));
    if (result == PET_JOURNAL_OK) s->state = *next;
    else s->ready = false; /* A failed read-back is an ambiguous commit; reopen. */
    return result;
}

pet_journal_result_t pet_slot_journal_open(pet_slot_journal_t *s, const pet_journal_io_t *io)
{
    if (!s || !io) return PET_JOURNAL_ARGUMENT;
    pet_journal_io_t callbacks = *io;
    memset(s, 0, sizeof(*s));
    uint8_t record[PET_SLOT_RECORD_BYTES]; size_t bytes = 0;
    pet_journal_result_t result = pet_journal_open(&s->journal, &callbacks, record, sizeof(record), &bytes);
    if (result == PET_JOURNAL_EMPTY) {
        if (!pet_slot_inventory_empty(&s->state)) return PET_JOURNAL_ARGUMENT;
        s->ready = true;
        /* Redundant baseline before any cloud selection or flash mutation. */
        result = pet_slot_journal_commit(s, &s->state);
        if (result == PET_JOURNAL_OK) result = pet_slot_journal_commit(s, &s->state);
        return result;
    }
    if (result != PET_JOURNAL_OK) return result;
    if (!pet_slot_inventory_decode(record, bytes, &s->state)) { s->journal.loaded = false; return PET_JOURNAL_CORRUPT; }
    s->ready = true; return PET_JOURNAL_OK;
}
