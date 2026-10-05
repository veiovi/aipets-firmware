#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PET_REPLACE_CHECKPOINT_BYTES 0x10000u
#define PET_REPLACE_MANIFEST_BYTES 0x2000u
#define PET_REPLACE_MAX_BYTES 0xbbe000u

/* This v2 state machine is deliberately separate from legacy pet_install_t.
 * A transition changes only a copy. Persist the copy before publishing it or
 * performing the newly permitted side effect. Never serialize this C struct:
 * the journal uses a versioned, validated wire representation. */
typedef enum {
    PET_REPLACE_EMPTY, PET_REPLACE_ACTIVE, PET_REPLACE_REQUESTED,
    PET_REPLACE_FENCED, PET_REPLACE_INVALIDATED, PET_REPLACE_DOWNLOADING,
    PET_REPLACE_VERIFIED, PET_REPLACE_ACTIVATING, PET_REPLACE_RECOVERY
} pet_replace_phase_t;
typedef struct {
    char build_id[37], sha256[65];
    uint32_t bytes;
} pet_replace_pack_t;
typedef struct {
    pet_replace_phase_t phase;
    uint32_t capacity_bytes;
    pet_replace_pack_t active, target;
    char request_id[37], operation_id[37], fence_id[37];
    char expected_revision[81], binding_revision[81];
    char relationship_id[37], config_version[81];
    uint32_t downloaded_bytes;
    char prefix_sha256[65]; /* Hash of read-back bytes [0, downloaded_bytes). */
} pet_replace_t;

/* Only the four explicit single-pet 16 MiB layouts and one three-pet slot are
 * accepted; no caller-provided capacity can enlarge flash. Firmware sizing
 * still selects the final layout. */
bool pet_replace_capacity_valid(uint32_t capacity);
bool pet_replace_empty(pet_replace_t *state, uint32_t capacity);
bool pet_replace_valid(const pet_replace_t *state);
bool pet_replace_request(pet_replace_t *state, const char *request_id,
                         const pet_replace_pack_t *target, const char *expected_revision);
/* Caller has authenticated the operation, verified the immutable signed
 * manifest/capabilities and obtained the durable cloud fence for this target.
 * The cloud must already refuse old-session admission before returning it. */
bool pet_replace_fenced(pet_replace_t *state, const char *operation_id,
                        const char *fence_id, const pet_replace_pack_t *target);
/* First stop audio/conversation. Persist INVALIDATED (no active pack), then
 * detach/unmap all readers. Only then persist begin_download and permit erase. */
bool pet_replace_invalidate(pet_replace_t *state);
bool pet_replace_begin_download(pet_replace_t *state);
bool pet_replace_progress(pet_replace_t *state, uint32_t durable_bytes,
                          const char *readback_prefix_sha256);
/* Every new worker/boot must recompute the saved prefix before resuming. A
 * mismatch restarts the SAME target from zero; it never restores the old pet. */
bool pet_replace_resume_matches(const pet_replace_t *state, const pet_replace_pack_t *target,
                                uint32_t checked_bytes, const char *readback_prefix_sha256);
bool pet_replace_restart(pet_replace_t *state, const pet_replace_pack_t *target);
/* Caller has additionally checked full hash/signature/structure/every frame. */
bool pet_replace_verified(pet_replace_t *state, const char *full_sha256);
bool pet_replace_activate(pet_replace_t *state);
/* No local activation until this exact cloud acknowledgement is authenticated.
 * ACTIVATING cannot be cancelled on timeout: reconcile a lost acknowledgement. */
bool pet_replace_commit(pet_replace_t *state, const char *operation_id,
                        const char *fence_id, const pet_replace_pack_t *target,
                        const char *binding_revision, const char *relationship_id,
                        const char *config_version);
bool pet_replace_cancel(pet_replace_t *state);
/* Three-pet devices only: the cloud confirmed another installed pet as the
 * device's conversation. Allowed between installations, from EMPTY or ACTIVE;
 * the result is ACTIVE with that pack and binding, as after a commit. */
bool pet_replace_rebind(pet_replace_t *state, const pet_replace_pack_t *pack,
                        const char *binding_revision, const char *relationship_id,
                        const char *config_version);
/* RECOVERY cannot start an ordinary request and forget its durable old fence.
 * Retry only after cloud reauthorizes the SAME operation/fence, or supersede
 * after it atomically acknowledges old->new fencing. The replacement is a
 * validated FENCED state with no active pack and a distinct operation/fence. */
bool pet_replace_retry(pet_replace_t *state, const char *operation_id,
                       const char *fence_id, const pet_replace_pack_t *target);
bool pet_replace_supersede(pet_replace_t *state, const char *previous_operation_id,
                           const char *previous_fence_id, const pet_replace_t *replacement);
bool pet_replace_has_active(const pet_replace_t *state);
bool pet_replace_can_erase(const pet_replace_t *state);
