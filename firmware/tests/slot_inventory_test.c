#include "pet_flash_layout.h"
#include "pet_replace_journal.h"
#include "pet_slot_inventory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[PET_JOURNAL_SECTORS][PET_JOURNAL_SECTOR_BYTES];
static uint8_t baseline[sizeof(flash)];
static size_t cut = PET_JOURNAL_SECTOR_BYTES;
static bool fail;
static bool read_sector(void *unused, unsigned sector, uint8_t *record)
{ (void)unused; memcpy(record, flash[sector], PET_JOURNAL_SECTOR_BYTES); return true; }
static bool replace_sector(void *unused, unsigned sector, const uint8_t *record)
{
    (void)unused; memset(flash[sector], 0xff, PET_JOURNAL_SECTOR_BYTES);
    memcpy(flash[sector], record, fail ? cut : PET_JOURNAL_SECTOR_BYTES); return !fail;
}
static const pet_journal_io_t io = {.read = read_sector, .replace = replace_sector};

static pet_replace_pack_t pack(unsigned n)
{
    pet_replace_pack_t p = {0};
    snprintf(p.build_id, sizeof(p.build_id), "00000000-0000-4000-8000-%012u", n);
    memset(p.sha256, "0123456789abcdef"[n % 16], 64);
    p.bytes = 1000u + n;
    return p;
}
static void uuid(char out[37], unsigned n) { snprintf(out, 37, "00000000-0000-4000-9000-%012u", n); }

/* Journal every step, and prove a torn write leaves the previous or next state. */
static pet_slot_journal_t store;
static void same(const pet_slot_inventory_t *a, const pet_slot_inventory_t *b)
{
    uint8_t x[PET_SLOT_RECORD_BYTES], y[PET_SLOT_RECORD_BYTES];
    assert(pet_slot_inventory_encode(a, x, sizeof(x)) && pet_slot_inventory_encode(b, y, sizeof(y)));
    assert(!memcmp(x, y, sizeof(x)));
}
static void persist(const pet_slot_inventory_t *next)
{
    memcpy(baseline, flash, sizeof(flash));
    const size_t cuts[] = {0, 1, 19, 20, 24, 512, 1042, 4095};
    const pet_slot_inventory_t previous = store.state;
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
        memcpy(flash, baseline, sizeof(flash)); fail = false;
        pet_slot_journal_t attempt; assert(pet_slot_journal_open(&attempt, &io) == PET_JOURNAL_OK);
        cut = cuts[i]; fail = true;
        assert(pet_slot_journal_commit(&attempt, next) == PET_JOURNAL_IO && !attempt.ready);
        fail = false;
        pet_slot_journal_t reboot; assert(pet_slot_journal_open(&reboot, &io) == PET_JOURNAL_OK);
        uint8_t got[PET_SLOT_RECORD_BYTES], before[PET_SLOT_RECORD_BYTES], after[PET_SLOT_RECORD_BYTES];
        assert(pet_slot_inventory_encode(&reboot.state, got, sizeof(got)));
        assert(pet_slot_inventory_encode(&previous, before, sizeof(before)));
        assert(pet_slot_inventory_encode(next, after, sizeof(after)));
        assert(!memcmp(got, before, sizeof(got)) || !memcmp(got, after, sizeof(got)));
    }
    memcpy(flash, baseline, sizeof(flash)); fail = false;
    assert(pet_slot_journal_open(&store, &io) == PET_JOURNAL_OK);
    assert(pet_slot_journal_commit(&store, next) == PET_JOURNAL_OK);
    pet_slot_journal_t reopened; assert(pet_slot_journal_open(&reopened, &io) == PET_JOURNAL_OK);
    same(&reopened.state, next);
}

/* Apply one single-slot transition to a copy of the operation. */
typedef bool (*step_t)(pet_replace_t *op, const pet_replace_pack_t *target, unsigned n);
static bool fence(pet_replace_t *op, const pet_replace_pack_t *t, unsigned n)
{ char o[37], f[37]; uuid(o, n); uuid(f, n + 500); return pet_replace_fenced(op, o, f, t); }
static bool invalidate(pet_replace_t *op, const pet_replace_pack_t *t, unsigned n)
{ (void)t; (void)n; return pet_replace_invalidate(op); }
static bool download(pet_replace_t *op, const pet_replace_pack_t *t, unsigned n)
{ (void)n; return pet_replace_begin_download(op) && pet_replace_progress(op, t->bytes, t->sha256); }
static bool verify(pet_replace_t *op, const pet_replace_pack_t *t, unsigned n)
{ (void)n; return pet_replace_verified(op, t->sha256); }
static bool activate(pet_replace_t *op, const pet_replace_pack_t *t, unsigned n)
{ (void)t; (void)n; return pet_replace_activate(op); }
static bool commit(pet_replace_t *op, const pet_replace_pack_t *t, unsigned n)
{
    char o[37], f[37], relationship[37]; uuid(o, n); uuid(f, n + 500); uuid(relationship, n + 900);
    return pet_replace_commit(op, o, f, t, "binding-1", relationship, "config-1");
}
static void apply(pet_slot_inventory_t *inv, step_t step, const pet_replace_pack_t *t, unsigned n)
{
    pet_replace_t op = inv->operation;
    assert(step(&op, t, n));
    assert(pet_slot_inventory_apply(inv, &op, 7));
    assert(pet_slot_inventory_valid(inv));
    persist(inv);
}
static bool request(pet_replace_t *op, const pet_replace_pack_t *t, unsigned n)
{
    char r[37]; uuid(r, n + 100);
    return pet_replace_request(op, r, t, op->phase == PET_REPLACE_ACTIVE ? op->binding_revision : "revision-0");
}
/* One installation through every step; returns the slot it landed in. */
static int install(pet_slot_inventory_t *inv, unsigned n)
{
    const pet_replace_pack_t t = pack(n);
    apply(inv, request, &t, n);
    const int slot = inv->target;
    assert(slot >= 0 && slot != inv->active && slot != inv->bound);
    apply(inv, fence, &t, n);
    apply(inv, invalidate, &t, n);
    assert(inv->slots[slot].state == PET_SLOT_INSTALLING && inv->bound == -1 && inv->active != slot);
    apply(inv, download, &t, n);
    apply(inv, verify, &t, n);
    apply(inv, activate, &t, n);
    apply(inv, commit, &t, n);
    assert(inv->target == -1 && inv->active == slot && inv->bound == slot && inv->slots[slot].state == PET_SLOT_READY);
    assert(!strcmp(inv->slots[slot].pack.sha256, t.sha256) && inv->slots[slot].validator_revision == 7);
    return slot;
}

static void codec_rejects(const pet_slot_inventory_t *inv)
{
    uint8_t bytes[PET_SLOT_RECORD_BYTES]; pet_slot_inventory_t out;
    assert(pet_slot_inventory_encode(inv, bytes, sizeof(bytes)));
    assert(pet_slot_inventory_decode(bytes, sizeof(bytes), &out)); same(&out, inv);
    assert(!pet_slot_inventory_decode(bytes, sizeof(bytes) - 1, &out));
    for (unsigned slot = 0; slot < PET_SLOT_COUNT; ++slot) {
        if (inv->slots[slot].state == PET_SLOT_READY) continue;
        uint8_t copy[PET_SLOT_RECORD_BYTES]; memcpy(copy, bytes, sizeof(copy));
        copy[7] = (uint8_t)slot; /* active names a slot that is not ready */
        assert(!pet_slot_inventory_decode(copy, sizeof(copy), &out));
    }
    const struct { size_t at; uint8_t value; } corrupt[] = {
        {0, 'X'},           /* magic */
        {4, 1},             /* version 1 (never written by a device) */
        {6, 4},             /* slot count */
        {7, 9},             /* active out of range */
        {13, 9},            /* bound out of range */
        {14, 1},            /* reserved */
        {16, 3},            /* slot state */
        {17, 1},            /* slot reserved */
        {16 + 12 + 36, 'x'},       /* garbage after a string terminator */
        {16 + 3 * 114, 'X'},       /* embedded single-slot record */
    };
    for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        uint8_t copy[PET_SLOT_RECORD_BYTES]; memcpy(copy, bytes, sizeof(copy));
        copy[corrupt[i].at] = corrupt[i].value;
        assert(!pet_slot_inventory_decode(copy, sizeof(copy), &out));
    }
}

/* A cloud release of bytes already installed over USB replaces that copy, not
 * the stalest pet, so no pet ends up twice. The shown and bound pets stay. */
static void installs_over_its_copy(void)
{
    memset(flash, 0xff, sizeof(flash));
    assert(pet_slot_journal_open(&store, &io) == PET_JOURNAL_OK);
    pet_slot_inventory_t inv = store.state;
    assert(install(&inv, 1) == 0 && install(&inv, 2) == 1 && install(&inv, 3) == 2);
    char relationship[37]; uuid(relationship, 903);
    assert(pet_slot_inventory_select(&inv, 0) && pet_slot_inventory_rebind(&inv, 0, "binding-copy", relationship, "config-copy"));
    assert(inv.slots[1].shown_at < inv.slots[2].shown_at && inv.active == 0 && inv.bound == 0);
    pet_replace_pack_t signed_copy = pack(3); uuid(signed_copy.build_id, 777);
    apply(&inv, request, &signed_copy, 7);
    assert(inv.target == 2);
    pet_replace_t op = inv.operation; assert(pet_replace_cancel(&op));
    assert(pet_slot_inventory_apply(&inv, &op, 7) && inv.target == -1);
    pet_replace_pack_t on_screen = pack(1); uuid(on_screen.build_id, 778);
    apply(&inv, request, &on_screen, 8);
    assert(inv.target == 1);
}

static void exact_replacement(void)
{
    /* Includes the only pet, the shown/bound pet, and a non-LRU other pet. */
    for (unsigned count=1;count<=3;++count) for (unsigned target=0;target<count;++target) {
        memset(flash,0xff,sizeof(flash));
        assert(pet_slot_journal_open(&store,&io)==PET_JOURNAL_OK);
        pet_slot_inventory_t inv=store.state;
        for (unsigned n=1;n<=count;++n) assert(install(&inv,n)==(int)n-1);
        const pet_slot_inventory_t before=inv;
        pet_replace_pack_t old=inv.slots[target].pack,incoming=pack(9),wrong=old;
        pet_replace_t op=inv.operation;assert(request(&op,&incoming,9));
        wrong.sha256[0]=wrong.sha256[0]=='a'?'b':'a';
        assert(!pet_slot_inventory_apply_target(&inv,&op,7,&wrong));same(&inv,&before);
        wrong=old;wrong.bytes++;
        assert(!pet_slot_inventory_apply_target(&inv,&op,7,&wrong));same(&inv,&before);
        wrong=old;uuid(wrong.build_id,999);
        assert(!pet_slot_inventory_apply_target(&inv,&op,7,&wrong));same(&inv,&before);
        assert(pet_slot_inventory_apply_target(&inv,&op,7,&old)&&inv.target==(int)target);
        persist(&inv); /* Includes power loss before fencing an active target. */
        pet_slot_inventory_t cancelled=inv;pet_replace_t cancel=inv.operation;
        assert(pet_replace_cancel(&cancel)&&pet_slot_inventory_apply(&cancelled,&cancel,7));
        assert(!memcmp(cancelled.slots,before.slots,sizeof(before.slots)));
        apply(&inv,fence,&incoming,9);apply(&inv,invalidate,&incoming,9);
        assert(inv.active!=(int)target&&inv.bound<0);
        apply(&inv,download,&incoming,9);apply(&inv,verify,&incoming,9);
        apply(&inv,activate,&incoming,9);apply(&inv,commit,&incoming,9);
        assert(inv.bound==(int)target&&inv.active==(int)target);
        for(unsigned i=0;i<PET_SLOT_COUNT;++i) if(i!=target)
            assert(!memcmp(&inv.slots[i],&before.slots[i],offsetof(pet_slot_t,shown_at)));
    }
}

static void exact_removal(void)
{
    for (unsigned count = 1; count <= 3; ++count)
    {
        for (unsigned target = 0; target < count; ++target)
        {
            memset(flash, 0xff, sizeof(flash));
            assert(pet_slot_journal_open(&store, &io) == PET_JOURNAL_OK);
            pet_slot_inventory_t inv = store.state;
            for (unsigned n = 1; n <= count; ++n)
                assert(install(&inv, n) == (int)n - 1);
            const pet_slot_inventory_t before = inv;
            pet_replace_pack_t old = inv.slots[target].pack;
            for (unsigned field = 0; field < 3; ++field)
            {
                pet_replace_pack_t absent = old;
                if (field == 0) uuid(absent.build_id, 999);
                if (field == 1) absent.sha256[0] = 'f';
                if (field == 2) ++absent.bytes;
                assert(pet_slot_inventory_remove(&inv, &absent));
                same(&inv, &before);
            }
            assert(pet_slot_inventory_remove(&inv, &old));
            assert(inv.slots[target].state == PET_SLOT_FREE);
            assert(inv.active != (int)target && inv.bound != (int)target);
            for (unsigned i = 0; i < PET_SLOT_COUNT; ++i)
                if (i != target)
                    assert(!memcmp(&inv.slots[i], &before.slots[i], offsetof(pet_slot_t, shown_at)));
            if (before.bound != (int)target)
                assert(!memcmp(&before.operation, &inv.operation, sizeof(inv.operation)));
            persist(&inv); /* Existing torn-write matrix reopens old or new complete inventory. */
            const pet_slot_inventory_t removed = store.state;
            assert(pet_slot_inventory_remove(&store.state, &old)); /* Reboot before/lost acknowledgement. */
            same(&store.state, &removed);
            if (count == 1)
                assert(inv.active == -1 && inv.bound == -1 && inv.operation.phase == PET_REPLACE_EMPTY);
        }
    }
    pet_slot_inventory_t installing = store.state;
    pet_replace_pack_t incoming = pack(8);
    pet_replace_t op = installing.operation;
    assert(request(&op, &incoming, 8) && pet_slot_inventory_apply(&installing, &op, 7));
    assert(!pet_slot_inventory_remove(&installing, &incoming));
}

int main(void)
{
    pet_flash_layout_t layout; uint32_t pack_at, manifest_at;
    assert(pet_flash_layout_known(PET_LAYOUT_THREE_3P5M, &layout) && layout.pet_slots == PET_SLOT_COUNT);
    assert(pet_replace_capacity_valid(PET_LAYOUT_THREE_SLOT_PACK_BYTES) && PET_LAYOUT_THREE_SLOT_PACK_BYTES >= 3000000u);
    assert(pet_flash_layout_slot(&layout, 2, &pack_at, &manifest_at));
    assert(manifest_at + PET_LAYOUT_MANIFEST_BYTES <= layout.pet_partition_bytes);

    memset(flash, 0xff, sizeof(flash));
    assert(pet_slot_journal_open(&store, &io) == PET_JOURNAL_OK);
    pet_slot_inventory_t inv = store.state, before;
    assert(pet_slot_inventory_valid(&inv) && inv.active == -1 && inv.bound == -1 && inv.target == -1);
    assert(pet_slot_inventory_step(&inv, -1, 1) == -1 && !pet_slot_inventory_select(&inv, 0));

    /* Each new pet fills a free slot and becomes the bound, shown pet; the
     * previous pets stay installed. */
    assert(install(&inv, 1) == 0 && install(&inv, 2) == 1 && install(&inv, 3) == 2);
    for (unsigned slot = 0; slot < PET_SLOT_COUNT; ++slot) assert(inv.slots[slot].state == PET_SLOT_READY);
    assert(pet_slot_inventory_step(&inv, 2, 1) == 0 && pet_slot_inventory_step(&inv, 0, -1) == 2);
    assert(pet_slot_inventory_step(&inv, 1, 2) == -1);
    codec_rejects(&inv);

    /* A swipe shows another pet; the cloud's confirmation rebinds to it. */
    uint32_t revision = inv.selection_revision;
    assert(pet_slot_inventory_select(&inv, 0) && inv.active == 0 && inv.selection_revision == revision + 1);
    assert(inv.bound == 2 && !pet_slot_inventory_select(&inv, 3));
    char relationship[37]; uuid(relationship, 901);
    before = inv; assert(!pet_slot_inventory_rebind(&before, 3, "binding-2", relationship, "config-2"));
    assert(!pet_slot_inventory_rebind(&before, 0, "", relationship, "config-2"));
    assert(pet_slot_inventory_rebind(&inv, 0, "binding-2", relationship, "config-2") && inv.bound == 0);
    assert(inv.operation.phase == PET_REPLACE_ACTIVE && !strcmp(inv.operation.binding_revision, "binding-2"));
    persist(&inv);

    /* With every slot full, the stalest pet neither shown nor bound makes room. */
    assert(inv.slots[1].shown_at < inv.slots[2].shown_at);
    assert(install(&inv, 4) == 1 && !strcmp(inv.slots[0].pack.sha256, pack(1).sha256));

    /* A request cancelled before its fence changes nothing. */
    const pet_replace_pack_t fifth = pack(5);
    before = inv; apply(&inv, request, &fifth, 5);
    const int target = inv.target;
    assert(target == 2 && !pet_slot_inventory_rebind(&inv, 0, "binding-3", relationship, "config-3"));
    pet_replace_t op = inv.operation; assert(pet_replace_cancel(&op));
    assert(pet_slot_inventory_apply(&inv, &op, 7) && inv.target == -1 && inv.bound == before.bound);
    same(&inv, &before);
    persist(&inv);

    /* The target was on screen when invalidation came: another pet shows. */
    apply(&inv, request, &fifth, 5);
    assert(inv.target == 2 && pet_slot_inventory_select(&inv, 2) && inv.active == 2);
    apply(&inv, fence, &fifth, 5);
    apply(&inv, invalidate, &fifth, 5);
    assert(inv.active == 1 && inv.bound == -1 && inv.slots[2].state == PET_SLOT_INSTALLING);
    assert(!pet_slot_inventory_select(&inv, 2) && pet_slot_inventory_step(&inv, 1, 1) == 0);
    codec_rejects(&inv);

    /* A cancel after the fence is recovery on the same slot: retry, then finish. */
    op = inv.operation; assert(pet_replace_cancel(&op) && op.phase == PET_REPLACE_RECOVERY);
    assert(pet_slot_inventory_apply(&inv, &op, 7) && inv.slots[2].state == PET_SLOT_INSTALLING && inv.bound == -1);
    persist(&inv);
    char o[37], f[37]; uuid(o, 5); uuid(f, 505);
    op = inv.operation; assert(pet_replace_retry(&op, o, f, &fifth) && pet_slot_inventory_apply(&inv, &op, 7));
    persist(&inv);

    /* A superseding operation keeps writing the same slot. */
    op = inv.operation; assert(pet_replace_cancel(&op) && pet_slot_inventory_apply(&inv, &op, 7));
    const pet_replace_pack_t sixth = pack(6);
    pet_replace_t replacement; char r[37], o6[37], f6[37]; uuid(r, 106); uuid(o6, 6); uuid(f6, 506);
    assert(pet_replace_empty(&replacement, PET_LAYOUT_THREE_SLOT_PACK_BYTES) &&
           pet_replace_request(&replacement, r, &sixth, "revision-6") && pet_replace_fenced(&replacement, o6, f6, &sixth));
    op = inv.operation; assert(pet_replace_supersede(&op, o, f, &replacement));
    assert(pet_slot_inventory_apply(&inv, &op, 7) && inv.target == 2 && inv.slots[2].state == PET_SLOT_INSTALLING);
    persist(&inv);
    apply(&inv, invalidate, &sixth, 6); apply(&inv, download, &sixth, 6); apply(&inv, verify, &sixth, 6);
    apply(&inv, activate, &sixth, 6);
    op = inv.operation; assert(commit(&op, &sixth, 6));
    assert(!pet_slot_inventory_apply(&inv, &op, 0));
    apply(&inv, commit, &sixth, 6);
    assert(inv.active == 2 && inv.bound == 2 && !strcmp(inv.slots[2].pack.sha256, sixth.sha256));
    codec_rejects(&inv);

    /* A capacity from another layout is refused. */
    op = inv.operation; op.capacity_bytes = PET_REPLACE_MAX_BYTES;
    assert(!pet_slot_inventory_apply(&inv, &op, 7));

    /* A single-pet journal is foreign here: fail closed, never format it. */
    memset(flash, 0xff, sizeof(flash));
    pet_replace_journal_t single; assert(pet_replace_journal_open(&single, &io, PET_REPLACE_MAX_BYTES) == PET_JOURNAL_OK);
    pet_slot_journal_t foreign; assert(pet_slot_journal_open(&foreign, &io) == PET_JOURNAL_CORRUPT && !foreign.ready);
    installs_over_its_copy();
    exact_replacement();
    exact_removal();
    puts("slot inventory: installs over a copy, into free and stalest slots, swipes and rebinding, cancel, recovery, supersession, torn writes and foreign journals passed");
    return 0;
}
