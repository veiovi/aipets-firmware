#include "pet_replace_writer.h"
#include <string.h>

static bool ready(const pet_replace_writer_t *w)
{
    return w && w->initialized && w->store && w->store->ready && w->store->journal.loaded &&
        pet_replace_valid(&w->store->state) &&
        w->io.partition_bytes == w->store->state.capacity_bytes + PET_REPLACE_MANIFEST_BYTES;
}
static bool digest(const mbedtls_sha256_context *context, char result[65])
{
    mbedtls_sha256_context copy; uint8_t bytes[32]; mbedtls_sha256_init(&copy);
    mbedtls_sha256_clone(&copy, context); int status = mbedtls_sha256_finish(&copy, bytes);
    mbedtls_sha256_free(&copy); if (status) return false;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) { result[2*i] = hex[bytes[i] >> 4]; result[2*i+1] = hex[bytes[i] & 15]; }
    result[64] = 0; return true;
}
static bool identity_matches(const pet_replace_writer_t *w)
{
    const pet_replace_t *s = &w->store->state;
    return w->resume_checked && w->detached && w->checked_bytes == s->downloaded_bytes &&
        !strcmp(w->checked_operation, s->operation_id) && !strcmp(w->checked_fence, s->fence_id) &&
        w->checked_target.bytes == s->target.bytes &&
        !strcmp(w->checked_target.build_id, s->target.build_id) &&
        !strcmp(w->checked_target.sha256, s->target.sha256);
}
bool pet_replace_writer_init(pet_replace_writer_t *w, pet_replace_journal_t *s, const pet_replace_io_t *io)
{
    if (!w || !s || !s->ready || !io || !io->read || !io->erase || !io->write || !io->freeze || !io->detach ||
        !pet_replace_valid(&s->state) || io->partition_bytes != s->state.capacity_bytes + PET_REPLACE_MANIFEST_BYTES)
        return false;
    pet_replace_io_t callbacks = *io;
    memset(w, 0, sizeof(*w)); w->store = s; w->io = callbacks; w->initialized = true;
    mbedtls_sha256_init(&w->prefix); return true;
}
void pet_replace_writer_close(pet_replace_writer_t *w)
{
    if (!w) return;
    if (w->initialized) mbedtls_sha256_free(&w->prefix);
    memset(w, 0, sizeof(*w));
}
bool pet_replace_writer_resume(pet_replace_writer_t *w)
{
    if (!ready(w)) return false;
    w->resume_checked = false; w->detached = false;
    pet_replace_t next = w->store->state;
    if (next.phase != PET_REPLACE_FENCED && next.phase != PET_REPLACE_INVALIDATED &&
        next.phase != PET_REPLACE_DOWNLOADING) return false;
    w->io.freeze(w->io.context);
    if (next.phase == PET_REPLACE_FENCED) {
        if (!pet_replace_invalidate(&next) ||
            pet_replace_journal_commit(w->store, &next) != PET_JOURNAL_OK) return false;
    }
    /* Durable unavailable state already exists before any mapping is released. */
    if (!w->io.detach(w->io.context)) return false;
    w->detached = true;
    if (next.phase == PET_REPLACE_INVALIDATED) {
        if (!pet_replace_begin_download(&next) ||
            pet_replace_journal_commit(w->store, &next) != PET_JOURNAL_OK) return false;
    }
    if (mbedtls_sha256_starts(&w->prefix, 0)) return false;
    uint8_t buffer[1024];
    for (uint32_t at = 0; at < next.downloaded_bytes;) {
        size_t n = next.downloaded_bytes - at; if (n > sizeof(buffer)) n = sizeof(buffer);
        if (!w->io.read(w->io.context, at, buffer, n) || mbedtls_sha256_update(&w->prefix, buffer, n)) return false;
        at += (uint32_t)n;
    }
    char prefix[65] = {0};
    if (next.downloaded_bytes && !digest(&w->prefix, prefix)) return false;
    if (!pet_replace_resume_matches(&next, &next.target, next.downloaded_bytes, prefix)) {
        /* Saved progress does not match flash. Persist a same-target restart;
         * return failure so the worker reports recovery before trying again. */
        if (pet_replace_restart(&next, &next.target)) pet_replace_journal_commit(w->store, &next);
        return false;
    }
    w->checked_bytes = next.downloaded_bytes;
    strcpy(w->checked_operation, next.operation_id); strcpy(w->checked_fence, next.fence_id);
    w->checked_target = next.target; w->resume_checked = true; return true;
}
bool pet_replace_writer_block(pet_replace_writer_t *w, const void *data, size_t bytes)
{
    if (!ready(w) || !identity_matches(w) || !pet_replace_can_erase(&w->store->state) || !data ||
        !bytes || bytes > PET_REPLACE_CHECKPOINT_BYTES) return false;
    pet_replace_t next = w->store->state; uint32_t offset = next.downloaded_bytes;
    if (offset % PET_REPLACE_CHECKPOINT_BYTES || bytes > next.target.bytes - offset ||
        (bytes != PET_REPLACE_CHECKPOINT_BYTES && offset + bytes != next.target.bytes)) return false;
    size_t erase_bytes = (bytes + 4095u) & ~(size_t)4095u;
    if (erase_bytes > next.capacity_bytes - offset) return false;
    w->resume_checked = false; /* Any ambiguous failure requires a fresh read-back. */
    if (!w->io.erase(w->io.context, offset, erase_bytes) ||
        !w->io.write(w->io.context, offset, data, bytes)) return false;
    uint8_t buffer[1024];
    for (size_t at = 0; at < bytes;) {
        size_t n = bytes - at; if (n > sizeof(buffer)) n = sizeof(buffer);
        if (!w->io.read(w->io.context, offset + (uint32_t)at, buffer, n) ||
            memcmp(buffer, (const uint8_t *)data + at, n) ||
            mbedtls_sha256_update(&w->prefix, buffer, n)) return false;
        at += n;
    }
    char prefix[65];
    if (!digest(&w->prefix, prefix) || !pet_replace_progress(&next, offset + (uint32_t)bytes, prefix) ||
        pet_replace_journal_commit(w->store, &next) != PET_JOURNAL_OK) return false;
    w->checked_bytes = next.downloaded_bytes; w->resume_checked = true; return true;
}
bool pet_replace_writer_manifest(pet_replace_writer_t *w, const void *data, size_t bytes)
{
    if (!ready(w) || !identity_matches(w) || !pet_replace_can_erase(&w->store->state) ||
        w->store->state.downloaded_bytes || !data || bytes != PET_REPLACE_MANIFEST_BYTES) return false;
    uint32_t offset = w->store->state.capacity_bytes; w->resume_checked = false;
    if (!w->io.erase(w->io.context, offset, PET_REPLACE_MANIFEST_BYTES) ||
        !w->io.write(w->io.context, offset, data, bytes)) return false;
    uint8_t buffer[1024];
    for (size_t at = 0; at < bytes; at += sizeof(buffer))
        if (!w->io.read(w->io.context, offset + (uint32_t)at, buffer, sizeof(buffer)) ||
            memcmp(buffer, (const uint8_t *)data + at, sizeof(buffer))) return false;
    w->resume_checked = true; return true;
}
