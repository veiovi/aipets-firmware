#include "pet_replace.h"

#include <string.h>

static bool text(const char *s, size_t capacity)
{ return s && s[0] && memchr(s, 0, capacity); }
static bool uuid(const char *s)
{
    if (!text(s, 37) || strlen(s) != 36) return false;
    for (unsigned i = 0; i < 36; ++i)
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!strchr("0123456789abcdef", s[i])) return false;
    return true;
}
static bool hash(const char *s)
{ return text(s, 65) && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64; }
static bool empty_pack(const pet_replace_pack_t *p)
{ return !p->build_id[0] && !p->sha256[0] && !p->bytes; }
static bool pack_valid(const pet_replace_pack_t *p, uint32_t capacity)
{ return p && uuid(p->build_id) && hash(p->sha256) && p->bytes && p->bytes <= capacity; }
static bool same_pack(const pet_replace_pack_t *a, const pet_replace_pack_t *b, uint32_t capacity)
{
    return pack_valid(a, capacity) && pack_valid(b, capacity) && a->bytes == b->bytes &&
        !strcmp(a->build_id, b->build_id) && !strcmp(a->sha256, b->sha256);
}
static void clear_active(pet_replace_t *s)
{
    memset(&s->active, 0, sizeof(s->active));
    s->binding_revision[0] = s->relationship_id[0] = s->config_version[0] = 0;
}
static void clear_operation(pet_replace_t *s)
{
    memset(&s->target, 0, sizeof(s->target));
    s->request_id[0] = s->operation_id[0] = s->fence_id[0] = s->expected_revision[0] = 0;
    s->downloaded_bytes = 0; s->prefix_sha256[0] = 0;
}
bool pet_replace_capacity_valid(uint32_t bytes)
{
    /* The four single-pet layouts, and one slot of the three-pet store. */
    return bytes == 0xbbe000u || bytes == 0xabe000u || bytes == 0x9be000u || bytes == 0x7be000u ||
        bytes == 0x2dd000u;
}
bool pet_replace_empty(pet_replace_t *s, uint32_t capacity)
{
    if (!s || !pet_replace_capacity_valid(capacity)) return false;
    memset(s, 0, sizeof(*s)); s->capacity_bytes = capacity; return true;
}
bool pet_replace_valid(const pet_replace_t *s)
{
    if (!s || !pet_replace_capacity_valid(s->capacity_bytes) ||
        s->phase < PET_REPLACE_EMPTY || s->phase > PET_REPLACE_RECOVERY) return false;
    if (!memchr(s->request_id, 0, sizeof(s->request_id)) ||
        !memchr(s->operation_id, 0, sizeof(s->operation_id)) ||
        !memchr(s->fence_id, 0, sizeof(s->fence_id)) ||
        !memchr(s->expected_revision, 0, sizeof(s->expected_revision)) ||
        !memchr(s->binding_revision, 0, sizeof(s->binding_revision)) ||
        !memchr(s->relationship_id, 0, sizeof(s->relationship_id)) ||
        !memchr(s->config_version, 0, sizeof(s->config_version)) ||
        !memchr(s->prefix_sha256, 0, sizeof(s->prefix_sha256)) ||
        !memchr(s->active.build_id, 0, sizeof(s->active.build_id)) ||
        !memchr(s->active.sha256, 0, sizeof(s->active.sha256)) ||
        !memchr(s->target.build_id, 0, sizeof(s->target.build_id)) ||
        !memchr(s->target.sha256, 0, sizeof(s->target.sha256))) return false;
    bool active = !empty_pack(&s->active);
    if (active) {
        if (!pack_valid(&s->active, s->capacity_bytes) || !uuid(s->relationship_id) ||
            !text(s->binding_revision, sizeof(s->binding_revision)) ||
            !text(s->config_version, sizeof(s->config_version)) ||
            (s->phase != PET_REPLACE_ACTIVE && s->phase != PET_REPLACE_REQUESTED &&
             s->phase != PET_REPLACE_FENCED)) return false;
    } else if (s->binding_revision[0] || s->relationship_id[0] || s->config_version[0]) return false;
    if (s->phase == PET_REPLACE_EMPTY || s->phase == PET_REPLACE_ACTIVE)
        return active == (s->phase == PET_REPLACE_ACTIVE) && empty_pack(&s->target) &&
            !s->request_id[0] && !s->operation_id[0] && !s->fence_id[0] &&
            !s->expected_revision[0] && !s->downloaded_bytes && !s->prefix_sha256[0];
    if (!pack_valid(&s->target, s->capacity_bytes) || !uuid(s->request_id) ||
        !text(s->expected_revision, sizeof(s->expected_revision))) return false;
    if (s->phase == PET_REPLACE_REQUESTED)
        return !s->operation_id[0] && !s->fence_id[0] && !s->downloaded_bytes && !s->prefix_sha256[0];
    if (!uuid(s->operation_id) || !uuid(s->fence_id)) return false;
    if (s->phase == PET_REPLACE_FENCED || s->phase == PET_REPLACE_INVALIDATED)
        return !s->downloaded_bytes && !s->prefix_sha256[0];
    if (s->downloaded_bytes > s->target.bytes ||
        (s->downloaded_bytes != s->target.bytes && s->downloaded_bytes % PET_REPLACE_CHECKPOINT_BYTES) ||
        (s->downloaded_bytes ? !hash(s->prefix_sha256) : !!s->prefix_sha256[0])) return false;
    if (s->phase == PET_REPLACE_VERIFIED || s->phase == PET_REPLACE_ACTIVATING)
        return s->downloaded_bytes == s->target.bytes && !strcmp(s->prefix_sha256, s->target.sha256);
    return true;
}
bool pet_replace_request(pet_replace_t *s, const char *id, const pet_replace_pack_t *target,
                         const char *revision)
{
    if (!pet_replace_valid(s) || (s->phase != PET_REPLACE_EMPTY && s->phase != PET_REPLACE_ACTIVE) ||
        !uuid(id) || !pack_valid(target, s->capacity_bytes) ||
        !text(revision, sizeof(s->expected_revision))) return false;
    /* Permit arguments borrowed from the current state without clearing them. */
    pet_replace_pack_t selected = *target; char request[37], expected[81];
    strcpy(request, id); strcpy(expected, revision); clear_operation(s);
    s->target = selected; strcpy(s->request_id, request); strcpy(s->expected_revision, expected);
    s->phase = PET_REPLACE_REQUESTED; return true;
}
bool pet_replace_fenced(pet_replace_t *s, const char *operation, const char *fence,
                        const pet_replace_pack_t *target)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_REQUESTED || !uuid(operation) || !uuid(fence) ||
        !same_pack(&s->target, target, s->capacity_bytes)) return false;
    strcpy(s->operation_id, operation); strcpy(s->fence_id, fence);
    s->phase = PET_REPLACE_FENCED; return true;
}
bool pet_replace_invalidate(pet_replace_t *s)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_FENCED) return false;
    clear_active(s); s->phase = PET_REPLACE_INVALIDATED; return true;
}
bool pet_replace_begin_download(pet_replace_t *s)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_INVALIDATED) return false;
    s->phase = PET_REPLACE_DOWNLOADING; return true;
}
bool pet_replace_progress(pet_replace_t *s, uint32_t bytes, const char *prefix)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_DOWNLOADING || !hash(prefix) ||
        bytes <= s->downloaded_bytes || bytes > s->target.bytes ||
        bytes - s->downloaded_bytes > PET_REPLACE_CHECKPOINT_BYTES ||
        (bytes != s->target.bytes && bytes % PET_REPLACE_CHECKPOINT_BYTES)) return false;
    s->downloaded_bytes = bytes; strcpy(s->prefix_sha256, prefix); return true;
}
bool pet_replace_resume_matches(const pet_replace_t *s, const pet_replace_pack_t *target,
                                uint32_t bytes, const char *prefix)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_DOWNLOADING ||
        !same_pack(&s->target, target, s->capacity_bytes) || bytes != s->downloaded_bytes) return false;
    return bytes ? hash(prefix) && !strcmp(prefix, s->prefix_sha256) : prefix && !prefix[0];
}
bool pet_replace_restart(pet_replace_t *s, const pet_replace_pack_t *target)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_DOWNLOADING ||
        !same_pack(&s->target, target, s->capacity_bytes)) return false;
    s->downloaded_bytes = 0; s->prefix_sha256[0] = 0;
    s->phase = PET_REPLACE_INVALIDATED; return true;
}
bool pet_replace_verified(pet_replace_t *s, const char *full_hash)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_DOWNLOADING ||
        s->downloaded_bytes != s->target.bytes || !hash(full_hash) ||
        strcmp(full_hash, s->target.sha256) || strcmp(full_hash, s->prefix_sha256)) return false;
    s->phase = PET_REPLACE_VERIFIED; return true;
}
bool pet_replace_activate(pet_replace_t *s)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_VERIFIED) return false;
    s->phase = PET_REPLACE_ACTIVATING; return true;
}
bool pet_replace_commit(pet_replace_t *s, const char *operation, const char *fence,
                        const pet_replace_pack_t *target, const char *revision,
                        const char *relationship, const char *config)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_ACTIVATING ||
        !uuid(operation) || !uuid(fence) || strcmp(operation, s->operation_id) || strcmp(fence, s->fence_id) ||
        !same_pack(&s->target, target, s->capacity_bytes) || !text(revision, sizeof(s->binding_revision)) ||
        !uuid(relationship) || !text(config, sizeof(s->config_version))) return false;
    s->active = s->target; strcpy(s->binding_revision, revision);
    strcpy(s->relationship_id, relationship); strcpy(s->config_version, config);
    clear_operation(s); s->phase = PET_REPLACE_ACTIVE; return true;
}
bool pet_replace_cancel(pet_replace_t *s)
{
    if (!pet_replace_valid(s) || s->phase == PET_REPLACE_EMPTY || s->phase == PET_REPLACE_ACTIVE ||
        s->phase == PET_REPLACE_ACTIVATING || s->phase == PET_REPLACE_RECOVERY) return false;
    if (s->phase == PET_REPLACE_REQUESTED) {
        clear_operation(s); s->phase = empty_pack(&s->active) ? PET_REPLACE_EMPTY : PET_REPLACE_ACTIVE;
    } else {
        /* Keep operation/fence/target for recovery reconciliation. Even before
         * erase, a committed fence must never resurrect old conversation. */
        clear_active(s); s->phase = PET_REPLACE_RECOVERY;
    }
    return true;
}
bool pet_replace_rebind(pet_replace_t *s, const pet_replace_pack_t *pack, const char *revision,
                        const char *relationship, const char *config)
{
    if (!pet_replace_valid(s) || (s->phase != PET_REPLACE_EMPTY && s->phase != PET_REPLACE_ACTIVE) || !pack ||
        !pack_valid(pack, s->capacity_bytes) || !text(revision, sizeof(s->binding_revision)) ||
        !uuid(relationship) || !text(config, sizeof(s->config_version))) return false;
    clear_operation(s); s->active = *pack; strcpy(s->binding_revision, revision);
    strcpy(s->relationship_id, relationship); strcpy(s->config_version, config);
    s->phase = PET_REPLACE_ACTIVE; return true;
}
bool pet_replace_has_active(const pet_replace_t *s)
{
    return pet_replace_valid(s) && (s->phase == PET_REPLACE_ACTIVE || s->phase == PET_REPLACE_REQUESTED) &&
        !empty_pack(&s->active);
}
bool pet_replace_retry(pet_replace_t *s, const char *operation, const char *fence,
                       const pet_replace_pack_t *target)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_RECOVERY ||
        !uuid(operation) || !uuid(fence) || strcmp(operation, s->operation_id) || strcmp(fence, s->fence_id) ||
        !same_pack(&s->target, target, s->capacity_bytes)) return false;
    s->downloaded_bytes = 0; s->prefix_sha256[0] = 0;
    s->phase = PET_REPLACE_INVALIDATED; return true;
}
bool pet_replace_supersede(pet_replace_t *s, const char *operation, const char *fence,
                           const pet_replace_t *replacement)
{
    if (!pet_replace_valid(s) || s->phase != PET_REPLACE_RECOVERY ||
        !uuid(operation) || !uuid(fence) || strcmp(operation, s->operation_id) || strcmp(fence, s->fence_id) ||
        !pet_replace_valid(replacement) || replacement->phase != PET_REPLACE_FENCED ||
        !empty_pack(&replacement->active) || replacement->capacity_bytes != s->capacity_bytes ||
        !strcmp(replacement->operation_id, operation) || !strcmp(replacement->fence_id, fence)) return false;
    *s = *replacement; return true;
}
bool pet_replace_can_erase(const pet_replace_t *s)
{ return pet_replace_valid(s) && s->phase == PET_REPLACE_DOWNLOADING && empty_pack(&s->active); }
