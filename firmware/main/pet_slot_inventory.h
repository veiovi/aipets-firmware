#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pet_journal.h"
#include "pet_replace.h"

/*
 * Inventory of the three-pet store: what each slot holds, which pet is on
 * screen, which one the cloud binds for conversation, and where an
 * installation in flight is written.
 *
 * `operation` is the single-slot machine of pet_replace.h for the bound pet,
 * unchanged: its ACTIVE pack is the pet the device talks as, and an
 * installation replaces that binding exactly as on a single-pet device. The
 * new bytes go to `target`, a slot that is neither on screen nor bound, so the
 * previous pet stays installed and can be selected again. The whole inventory
 * is one versioned journal record: binding, slots and selection always change
 * together.
 */

#define PET_SLOT_COUNT 3u
#define PET_SLOT_RECORD_BYTES 1042u

typedef enum { PET_SLOT_FREE, PET_SLOT_INSTALLING, PET_SLOT_READY } pet_slot_status_t;

typedef struct {
    pet_slot_status_t state;
    pet_replace_pack_t pack;
    /* Validator revision that last checked every frame of this pack. */
    uint16_t validator_revision;
    /* selection_revision when this pet was last shown: the stalest is reused. */
    uint32_t shown_at;
} pet_slot_t;

typedef struct {
    pet_slot_t slots[PET_SLOT_COUNT];
    int active;                 /* The pet on screen; -1 when none is ready. */
    int bound;                  /* The slot of operation's pack; -1 when none. */
    int target;                 /* The slot receiving an installation; -1 when idle. */
    uint32_t selection_revision;
    pet_replace_t operation;    /* The bound pet's single-slot machine. */
} pet_slot_inventory_t;

bool pet_slot_inventory_empty(pet_slot_inventory_t *inventory);
bool pet_slot_inventory_valid(const pet_slot_inventory_t *inventory);

/* Local selection, e.g. a swipe. Only a ready slot can be shown. */
bool pet_slot_inventory_select(pet_slot_inventory_t *inventory, unsigned slot);
/* The next ready slot after `from` in `direction` (+1 or -1), wrapping around.
 * Returns `from` when it is the only ready slot and -1 when none is ready. */
int pet_slot_inventory_step(const pet_slot_inventory_t *inventory, int from, int direction);

/* Apply the result of a pet_replace_* transition of `operation`.
 * - A new request picks the target: a free slot, else the slot already holding
 *   the same bytes, else the least recently shown slot. Never the slot on
 *   screen or the bound one.
 * - Invalidation starts rewriting the target and ends the old binding. If the
 *   target is on screen, another installed pet is shown instead.
 * - A commit makes the target ready, bound and shown, and records
 *   `validator_revision` (nonzero, used only on commit).
 * - A request cancelled before its fence leaves every slot as it was. */
bool pet_slot_inventory_apply(pet_slot_inventory_t *inventory, const pet_replace_t *next,
                              uint16_t validator_revision);
/* An authenticated replacement names the exact old pack, never a raw slot.
 * It may replace the shown/bound pet. Missing or ambiguous targets fail;
 * there is no free-slot/LRU fallback. Only used when creating REQUESTED. */
int pet_slot_inventory_find(const pet_slot_inventory_t *inventory, const pet_replace_pack_t *pack);
/* Idle-only exact removal; an absent target succeeds without changing state.
 * Clears only this slot/binding, selects another ready pet when needed. */
bool pet_slot_inventory_remove(pet_slot_inventory_t *inventory, const pet_replace_pack_t *pack);
bool pet_slot_inventory_apply_target(pet_slot_inventory_t *inventory, const pet_replace_t *next,
                                     uint16_t validator_revision, const pet_replace_pack_t *replacement);
/* The cloud confirmed another installed pet as the conversation (a selection
 * after a swipe). Only between installations. */
bool pet_slot_inventory_rebind(pet_slot_inventory_t *inventory, unsigned slot, const char *binding_revision,
                               const char *relationship_id, const char *config_version);

/* Explicit little-endian fields and zero-padded strings, never C ABI bytes. */
bool pet_slot_inventory_encode(const pet_slot_inventory_t *inventory, void *record, size_t bytes);
bool pet_slot_inventory_decode(const void *record, size_t bytes, pet_slot_inventory_t *inventory);

typedef struct {
    pet_journal_t journal;
    pet_slot_inventory_t state;
    bool ready;
} pet_slot_journal_t;
/* A never-written journal starts an empty inventory. Corrupt or foreign
 * journals (including single-pet records) fail closed and are never formatted. */
pet_journal_result_t pet_slot_journal_open(pet_slot_journal_t *store, const pet_journal_io_t *io);
pet_journal_result_t pet_slot_journal_commit(pet_slot_journal_t *store, const pet_slot_inventory_t *next);
