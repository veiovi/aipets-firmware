#include "pet_replace_journal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[PET_JOURNAL_SECTORS][PET_JOURNAL_SECTOR_BYTES];
static uint8_t baseline[sizeof(flash)];
static size_t cut = PET_JOURNAL_SECTOR_BYTES;
static bool fail;
static unsigned writes;
static bool read_sector(void *unused, unsigned sector, uint8_t *record)
{ (void)unused; memcpy(record, flash[sector], PET_JOURNAL_SECTOR_BYTES); return true; }
static bool replace_sector(void *unused, unsigned sector, const uint8_t *record)
{
    (void)unused; ++writes; memset(flash[sector], 0xff, PET_JOURNAL_SECTOR_BYTES);
    memcpy(flash[sector], record, fail ? cut : PET_JOURNAL_SECTOR_BYTES); return !fail;
}
static const pet_journal_io_t io = {.read=read_sector,.replace=replace_sector};
static const char *id = "00000000-0000-4000-8000-000000000001";
static const char *op = "00000000-0000-4000-8000-000000000002";
static const char *fence = "00000000-0000-4000-8000-000000000003";
static const char *sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *oldsha = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const uint32_t capacity = PET_REPLACE_MAX_BYTES;

static void record_roundtrip(const pet_replace_t *s)
{
    uint8_t bytes[PET_REPLACE_RECORD_BYTES], copy[PET_REPLACE_RECORD_BYTES]; pet_replace_t decoded;
    assert(pet_replace_encode(s, bytes, sizeof(bytes)));
    assert(pet_replace_decode(bytes, sizeof(bytes), &decoded));
    assert(pet_replace_encode(&decoded, copy, sizeof(copy))); assert(!memcmp(bytes, copy, sizeof(bytes)));
    assert(!pet_replace_decode(bytes, sizeof(bytes)-1, &decoded));
    bytes[0] = 'X'; assert(!pet_replace_decode(bytes, sizeof(bytes), &decoded)); bytes[0] = 'P';
    bytes[8] = 255; assert(!pet_replace_decode(bytes, sizeof(bytes), &decoded));
}
static void persist_with_cuts(pet_replace_journal_t *s, const pet_replace_t *next, bool overwritten)
{
    record_roundtrip(next); memcpy(baseline, flash, sizeof(flash));
    const size_t cuts[] = {0,1,19,20,24,128,512,1024,4091,4092,4095,4096};
    const pet_replace_t previous = s->state;
    for (unsigned i = 0; i < sizeof(cuts)/sizeof(cuts[0]); ++i) {
        memcpy(flash, baseline, sizeof(flash)); fail = false; pet_replace_journal_t attempt;
        assert(pet_replace_journal_open(&attempt, &io, capacity) == PET_JOURNAL_OK);
        cut = cuts[i]; fail = true;
        assert(pet_replace_journal_commit(&attempt, next) == PET_JOURNAL_IO);
        assert(!attempt.ready && !attempt.journal.loaded);
        assert(pet_replace_journal_commit(&attempt, next) == PET_JOURNAL_ARGUMENT);
        fail = false; pet_replace_journal_t reboot;
        assert(pet_replace_journal_open(&reboot, &io, capacity) == PET_JOURNAL_OK);
        uint8_t got[PET_REPLACE_RECORD_BYTES], before[PET_REPLACE_RECORD_BYTES], after[PET_REPLACE_RECORD_BYTES];
        assert(pet_replace_encode(&reboot.state, got, sizeof(got)));
        assert(pet_replace_encode(&previous, before, sizeof(before)));
        assert(pet_replace_encode(next, after, sizeof(after)));
        assert(!memcmp(got, before, sizeof(got)) || !memcmp(got, after, sizeof(got)));
        if (overwritten && pet_replace_has_active(&reboot.state)) {
            /* Only the cloud-acknowledged NEW pack can be active now. */
            assert(next->phase == PET_REPLACE_ACTIVE);
            assert(!strcmp(reboot.state.active.sha256, sha));
        }
    }
    memcpy(flash, baseline, sizeof(flash)); fail = false;
    assert(pet_replace_journal_open(s, &io, capacity) == PET_JOURNAL_OK);
    assert(pet_replace_journal_commit(s, next) == PET_JOURNAL_OK);
}
int main(void)
{
    memset(flash, 0xff, sizeof(flash)); pet_replace_journal_t s;
    assert(pet_replace_journal_open(&s, &io, capacity) == PET_JOURNAL_OK);
    assert(s.ready && s.journal.generation == 2 && s.state.phase == PET_REPLACE_EMPTY);
    assert(pet_replace_journal_open(&s, &s.journal.io, capacity) == PET_JOURNAL_OK);
    pet_replace_pack_t old = {.bytes=3}, target = {.bytes=0x10003};
    strcpy(old.build_id, id); strcpy(old.sha256, oldsha);
    strcpy(target.build_id, op); strcpy(target.sha256, sha);
    pet_replace_t next = s.state;
    assert(pet_replace_request(&next, id, &old, "r0"));
    assert(pet_replace_fenced(&next, op, fence, &old));
    assert(pet_replace_invalidate(&next)); assert(pet_replace_begin_download(&next));
    assert(pet_replace_progress(&next, 3, oldsha)); assert(pet_replace_verified(&next, oldsha));
    assert(pet_replace_activate(&next)); assert(pet_replace_commit(&next, op, fence, &old, "r1", id, "1"));
    assert(pet_replace_journal_commit(&s, &next) == PET_JOURNAL_OK);
    assert(pet_replace_request(&next, id, &target, "r1")); persist_with_cuts(&s, &next, false);
    assert(pet_replace_fenced(&next, op, fence, &target)); persist_with_cuts(&s, &next, false);
    assert(pet_replace_invalidate(&next)); persist_with_cuts(&s, &next, false);
    assert(!pet_replace_has_active(&s.state) && !pet_replace_can_erase(&s.state));
    assert(pet_replace_begin_download(&next)); persist_with_cuts(&s, &next, false);
    assert(pet_replace_can_erase(&s.state)); /* External flash writer may start here. */
    assert(pet_replace_progress(&next, 0x10000, oldsha)); persist_with_cuts(&s, &next, true);
    assert(pet_replace_progress(&next, target.bytes, sha)); persist_with_cuts(&s, &next, true);
    assert(pet_replace_verified(&next, sha)); persist_with_cuts(&s, &next, true);
    pet_replace_t cancelled = next; assert(pet_replace_cancel(&cancelled));
    persist_with_cuts(&s, &cancelled, true);
    assert(pet_replace_retry(&cancelled, op, fence, &target)); persist_with_cuts(&s, &cancelled, true);
    assert(pet_replace_begin_download(&cancelled)); persist_with_cuts(&s, &cancelled, true);
    assert(pet_replace_progress(&cancelled, 0x10000, oldsha)); persist_with_cuts(&s, &cancelled, true);
    assert(pet_replace_progress(&cancelled, target.bytes, sha)); persist_with_cuts(&s, &cancelled, true);
    assert(pet_replace_verified(&cancelled, sha)); persist_with_cuts(&s, &cancelled, true);
    assert(pet_replace_activate(&cancelled)); persist_with_cuts(&s, &cancelled, true);
    assert(pet_replace_commit(&cancelled, op, fence, &target, "r2", id, "2"));
    persist_with_cuts(&s, &cancelled, true);
    unsigned before = writes;
    assert(pet_replace_journal_open(&s, &io, 0xabe000) == PET_JOURNAL_CORRUPT);
    assert(!s.ready && !s.journal.loaded && before == writes);
    memset(flash, 0, sizeof(flash));
    assert(pet_replace_journal_open(&s, &io, capacity) == PET_JOURNAL_CORRUPT);
    assert(!s.ready && before == writes); /* No implicit journal formatting. */
    assert(pet_replace_empty(&next, capacity));
    uint8_t bytes[PET_REPLACE_RECORD_BYTES]; assert(pet_replace_encode(&next, bytes, sizeof(bytes)));
    bytes[17] = 1; /* Hidden tail bytes after an empty string are noncanonical. */
    assert(!pet_replace_decode(bytes, sizeof(bytes), &next));
    puts("single-slot journal: canonical encoding, 180 torn/full-write cuts, recovery, capacity and corrupt-state rejection passed");
    return 0;
}
