#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PET_INSTALL_PACK_MAX 0x2a0000u
#define PET_INSTALL_CHECKPOINT_BYTES 0x10000u
#define PET_INSTALL_UUID_MAX 37
#define PET_INSTALL_HASH_MAX 65
#define PET_INSTALL_REVISION_MAX 81

typedef enum { PET_SLOT_EMPTY, PET_SLOT_PARTIAL, PET_SLOT_VERIFIED, PET_SLOT_ACTIVE } pet_slot_state_t;
typedef struct {
    pet_slot_state_t state;
    char build_id[PET_INSTALL_UUID_MAX];
    char sha256[PET_INSTALL_HASH_MAX];
    uint32_t bytes;
} pet_install_slot_t;
typedef enum { PET_INSTALL_IDLE, PET_INSTALL_REQUESTED, PET_INSTALL_DOWNLOADING,
               PET_INSTALL_VERIFIED, PET_INSTALL_ACTIVATING } pet_install_phase_t;
typedef struct {
    pet_install_phase_t phase;
    int8_t active_slot;
    int8_t candidate_slot;
    pet_install_slot_t slots[2];
    char request_id[PET_INSTALL_UUID_MAX];
    char installation_id[PET_INSTALL_UUID_MAX];
    char expected_revision[PET_INSTALL_REVISION_MAX];
    uint32_t downloaded_bytes;
    char binding_revision[PET_INSTALL_REVISION_MAX];
    char relationship_id[PET_INSTALL_UUID_MAX];
    char config_version[PET_INSTALL_REVISION_MAX];
    char cached_context[2048]; /* Same CRC/generation as committed active slot; offline display only. */
} pet_install_t;

/* Pure state transitions. Caller copies state, transitions the copy, persists
 * the complete copy to the journal, then publishes it. No side effects before
 * that commit. Activation never implicitly rolls back after a timeout. */
void pet_install_empty(pet_install_t *state);
bool pet_install_valid(const pet_install_t *state);
bool pet_install_request(pet_install_t *state, const char *request_id,
                         const char *build_id, const char *sha256, uint32_t bytes,
                         const char *expected_revision);
bool pet_install_attach(pet_install_t *state, const char *installation_id,
                        const char *build_id, const char *sha256, uint32_t bytes);
bool pet_install_progress(pet_install_t *state, uint32_t durable_bytes);
bool pet_install_verified(pet_install_t *state);
bool pet_install_activate(pet_install_t *state);
bool pet_install_commit(pet_install_t *state, const char *installation_id,
                        const char *build_id, const char *sha256,
                        const char *binding_revision, const char *relationship_id,
                        const char *config_version);
/* Only before activation. Afterwards reconciliation must decide the result. */
bool pet_install_cancel(pet_install_t *state);
bool pet_install_binding_matches(const pet_install_t *state, const char *revision,
                                  const char *relationship_id, const char *build_id,
                                  const char *sha256, const char *config_version);
