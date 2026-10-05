#include "pet_install.h"

#include <string.h>

static bool bounded(const char *value, unsigned max)
{
    if (!value || !value[0]) return false;
    for (unsigned i = 0; i < max; ++i) if (!value[i]) return true;
    return false;
}
static bool uuid(const char *value)
{
    if (!bounded(value, 37) || strlen(value) != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!strchr("0123456789abcdef", value[i])) return false;
    }
    return true;
}
static bool hash(const char *value)
{
    return bounded(value, 65) && strlen(value) == 64 && strspn(value, "0123456789abcdef") == 64;
}

void pet_install_empty(pet_install_t *state)
{
    memset(state, 0, sizeof(*state));
    state->active_slot = state->candidate_slot = -1;
}

bool pet_install_valid(const pet_install_t *state)
{
    if (!state || state->phase < PET_INSTALL_IDLE || state->phase > PET_INSTALL_ACTIVATING ||
        state->active_slot < -1 || state->active_slot > 1 ||
        state->candidate_slot < -1 || state->candidate_slot > 1) return false;
    /* Optional fields still serialize in IDLE. Every fixed string must have
     * a terminator before any consumer treats it as a C string. */
    if (!memchr(state->request_id, 0, sizeof(state->request_id)) ||
        !memchr(state->installation_id, 0, sizeof(state->installation_id)) ||
        !memchr(state->expected_revision, 0, sizeof(state->expected_revision)) ||
        !memchr(state->binding_revision, 0, sizeof(state->binding_revision)) ||
        !memchr(state->relationship_id, 0, sizeof(state->relationship_id)) ||
        !memchr(state->config_version, 0, sizeof(state->config_version))||
        !memchr(state->cached_context,0,sizeof(state->cached_context))) return false;
    for (int i = 0; i < 2; ++i) {
        const pet_install_slot_t *slot = &state->slots[i];
        if (slot->state < PET_SLOT_EMPTY || slot->state > PET_SLOT_ACTIVE) return false;
        if ((slot->state == PET_SLOT_ACTIVE) != (state->active_slot == i)) return false;
        if (slot->state == PET_SLOT_EMPTY) {
            if (slot->build_id[0] || slot->sha256[0] || slot->bytes) return false;
        } else if (!uuid(slot->build_id) || !hash(slot->sha256) || !slot->bytes ||
                   slot->bytes > PET_INSTALL_PACK_MAX) return false;
    }
    if (state->active_slot != -1 &&
        (!bounded(state->binding_revision, sizeof(state->binding_revision)) ||
         !uuid(state->relationship_id) || !bounded(state->config_version, sizeof(state->config_version)))) return false;
    if (state->phase == PET_INSTALL_IDLE) return state->candidate_slot == -1;
    if (state->candidate_slot == -1 || state->candidate_slot == state->active_slot ||
        !uuid(state->request_id) || !bounded(state->expected_revision, sizeof(state->expected_revision))) return false;
    const pet_install_slot_t *candidate = &state->slots[state->candidate_slot];
    if (candidate->state == PET_SLOT_EMPTY || state->downloaded_bytes > candidate->bytes) return false;
    if (state->phase != PET_INSTALL_REQUESTED && !uuid(state->installation_id)) return false;
    if (state->phase >= PET_INSTALL_VERIFIED)
        return candidate->state == PET_SLOT_VERIFIED && state->downloaded_bytes == candidate->bytes;
    return candidate->state == PET_SLOT_PARTIAL;
}

bool pet_install_request(pet_install_t *state, const char *request_id,
                         const char *build_id, const char *sha256, uint32_t bytes,
                         const char *expected_revision)
{
    if (!pet_install_valid(state) || state->phase != PET_INSTALL_IDLE || !uuid(request_id) ||
        !uuid(build_id) || !hash(sha256) || !bytes || bytes > PET_INSTALL_PACK_MAX ||
        !bounded(expected_revision, sizeof(state->expected_revision))) return false;
    state->candidate_slot = state->active_slot == 0 ? 1 : 0;
    pet_install_slot_t *candidate = &state->slots[state->candidate_slot];
    memset(candidate, 0, sizeof(*candidate));
    candidate->state = PET_SLOT_PARTIAL;
    strcpy(candidate->build_id, build_id);
    strcpy(candidate->sha256, sha256);
    candidate->bytes = bytes;
    strcpy(state->request_id, request_id);
    strcpy(state->expected_revision, expected_revision);
    state->installation_id[0] = 0;
    state->downloaded_bytes = 0;
    state->phase = PET_INSTALL_REQUESTED;
    return true;
}

bool pet_install_attach(pet_install_t *state, const char *installation_id,
                        const char *build_id, const char *sha256, uint32_t bytes)
{
    if (!pet_install_valid(state) || state->phase != PET_INSTALL_REQUESTED ||
        !uuid(installation_id) || !uuid(build_id) || !hash(sha256)) return false;
    const pet_install_slot_t *candidate = &state->slots[state->candidate_slot];
    if (strcmp(candidate->build_id, build_id) || strcmp(candidate->sha256, sha256) ||
        candidate->bytes != bytes) return false;
    strcpy(state->installation_id, installation_id);
    state->phase = PET_INSTALL_DOWNLOADING;
    return true;
}

bool pet_install_progress(pet_install_t *state, uint32_t durable_bytes)
{
    if (!pet_install_valid(state) || state->phase != PET_INSTALL_DOWNLOADING) return false;
    uint32_t total = state->slots[state->candidate_slot].bytes;
    if (durable_bytes < state->downloaded_bytes || durable_bytes > total ||
        (durable_bytes != total && durable_bytes % PET_INSTALL_CHECKPOINT_BYTES)) return false;
    state->downloaded_bytes = durable_bytes;
    return true;
}

bool pet_install_verified(pet_install_t *state)
{
    if (!pet_install_valid(state) || state->phase != PET_INSTALL_DOWNLOADING ||
        state->downloaded_bytes != state->slots[state->candidate_slot].bytes) return false;
    state->slots[state->candidate_slot].state = PET_SLOT_VERIFIED;
    state->phase = PET_INSTALL_VERIFIED;
    return true;
}

bool pet_install_activate(pet_install_t *state)
{
    if (!pet_install_valid(state) || state->phase != PET_INSTALL_VERIFIED) return false;
    state->phase = PET_INSTALL_ACTIVATING;
    return true;
}

bool pet_install_commit(pet_install_t *state, const char *installation_id,
                        const char *build_id, const char *sha256,
                        const char *binding_revision, const char *relationship_id,
                        const char *config_version)
{
    if (!pet_install_valid(state) || state->phase != PET_INSTALL_ACTIVATING ||
        !uuid(installation_id) || !uuid(build_id) || !hash(sha256) ||
        !bounded(binding_revision, sizeof(state->binding_revision)) || !uuid(relationship_id) ||
        !bounded(config_version, sizeof(state->config_version))) return false;
    pet_install_slot_t *candidate = &state->slots[state->candidate_slot];
    if (strcmp(state->installation_id, installation_id) || strcmp(candidate->build_id, build_id) ||
        strcmp(candidate->sha256, sha256)) return false;
    if (state->active_slot != -1) state->slots[state->active_slot].state = PET_SLOT_VERIFIED;
    candidate->state = PET_SLOT_ACTIVE;
    state->active_slot = state->candidate_slot;
    state->candidate_slot = -1;
    strcpy(state->binding_revision, binding_revision);
    strcpy(state->relationship_id, relationship_id);
    strcpy(state->config_version, config_version);
    state->phase = PET_INSTALL_IDLE;
    return true;
}

bool pet_install_cancel(pet_install_t *state)
{
    if (!pet_install_valid(state) || state->phase == PET_INSTALL_IDLE ||
        state->phase == PET_INSTALL_ACTIVATING) return false;
    memset(&state->slots[state->candidate_slot], 0, sizeof(state->slots[0]));
    state->candidate_slot = -1;
    state->phase = PET_INSTALL_IDLE;
    return true;
}

bool pet_install_binding_matches(const pet_install_t *state, const char *revision,
                                  const char *relationship_id, const char *build_id,
                                  const char *sha256, const char *config_version)
{
    if (!pet_install_valid(state) || state->active_slot == -1 || state->phase == PET_INSTALL_ACTIVATING ||
        !bounded(revision, 81) || !uuid(relationship_id) || !uuid(build_id) || !hash(sha256) ||
        !bounded(config_version, 81)) return false;
    const pet_install_slot_t *active = &state->slots[state->active_slot];
    return !strcmp(state->binding_revision, revision) && !strcmp(state->relationship_id, relationship_id) &&
        !strcmp(active->build_id, build_id) && !strcmp(active->sha256, sha256) &&
        !strcmp(state->config_version, config_version);
}
