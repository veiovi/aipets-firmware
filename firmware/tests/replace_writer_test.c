#include "pet_replace_writer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t journal[PET_JOURNAL_SECTORS][PET_JOURNAL_SECTOR_BYTES];
static uint8_t flash[PET_REPLACE_MAX_BYTES + PET_REPLACE_MANIFEST_BYTES];
static uint8_t content[0x10003];
static pet_replace_journal_t store;
static bool journal_failure, write_failure, corrupt_readback, detach_failure, detached, frozen;
static unsigned erases, writes;
static const char *id = "00000000-0000-4000-8000-000000000001";
static const char *op = "00000000-0000-4000-8000-000000000002";
static const char *fence = "00000000-0000-4000-8000-000000000003";
static bool journal_read(void *unused, unsigned sector, uint8_t *record)
{ (void)unused; memcpy(record, journal[sector], PET_JOURNAL_SECTOR_BYTES); return true; }
static bool journal_write(void *unused, unsigned sector, const uint8_t *record)
{
    (void)unused; memset(journal[sector], 0xff, PET_JOURNAL_SECTOR_BYTES);
    memcpy(journal[sector], record, journal_failure ? 24 : PET_JOURNAL_SECTOR_BYTES); return !journal_failure;
}
static const pet_journal_io_t journal_io = {.read=journal_read,.replace=journal_write};
static bool read_flash(void *unused, uint32_t offset, void *data, size_t bytes)
{
    (void)unused; assert(bytes <= sizeof(flash) - offset); memcpy(data, flash + offset, bytes);
    if (corrupt_readback && bytes) ((uint8_t *)data)[0] ^= 1;
    return true;
}
static bool erase_flash(void *unused, uint32_t offset, size_t bytes)
{
    (void)unused; assert(frozen && detached && pet_replace_can_erase(&store.state));
    assert(offset % 4096 == 0 && bytes % 4096 == 0 && bytes <= sizeof(flash) - offset);
    assert(offset < PET_REPLACE_MAX_BYTES ? offset + bytes <= PET_REPLACE_MAX_BYTES :
        offset == PET_REPLACE_MAX_BYTES && bytes == PET_REPLACE_MANIFEST_BYTES);
    ++erases; memset(flash + offset, 0xff, bytes); return true;
}
static bool write_flash(void *unused, uint32_t offset, const void *data, size_t bytes)
{
    (void)unused; assert(frozen && detached && bytes <= sizeof(flash) - offset); ++writes;
    const uint8_t *p = data; size_t n = write_failure ? bytes / 2 : bytes;
    for (size_t i = 0; i < n; ++i) { assert((flash[offset+i] & p[i]) == p[i]); flash[offset+i] &= p[i]; }
    return !write_failure;
}
static void freeze(void *unused) { (void)unused; frozen = true; }
static bool detach(void *unused)
{
    (void)unused; assert(frozen && !pet_replace_has_active(&store.state));
    assert(store.state.phase == PET_REPLACE_INVALIDATED || store.state.phase == PET_REPLACE_DOWNLOADING);
    if (detach_failure) return false; detached = true; return true;
}
static const pet_replace_io_t io = {.read=read_flash,.erase=erase_flash,.write=write_flash,
    .freeze=freeze,.detach=detach,.partition_bytes=sizeof(flash)};
static void hash(const void *data, size_t bytes, char out[65])
{
    uint8_t raw[32]; assert(!mbedtls_sha256(data, bytes, raw, 0));
    for (unsigned i = 0; i < 32; ++i) snprintf(out + 2*i, 3, "%02x", raw[i]);
}
static void reopen(pet_replace_writer_t *w)
{
    pet_replace_writer_close(w); detached = frozen = false;
    assert(pet_replace_journal_open(&store, &journal_io, PET_REPLACE_MAX_BYTES) == PET_JOURNAL_OK);
    assert(pet_replace_writer_init(w, &store, &io));
}
int main(void)
{
    memset(journal, 0xff, sizeof(journal)); memset(flash, 0xff, sizeof(flash));
    memset(content, 0x55, sizeof(content)); memcpy(content + 0x10000, "end", 3);
    pet_replace_pack_t target = {.bytes=sizeof(content)}; strcpy(target.build_id, id); hash(content, sizeof(content), target.sha256);
    assert(pet_replace_journal_open(&store, &journal_io, PET_REPLACE_MAX_BYTES) == PET_JOURNAL_OK);
    pet_replace_writer_t w = {0}; assert(pet_replace_writer_init(&w, &store, &io));
    assert(pet_replace_writer_init(&w, &store, &w.io));
    assert(!pet_replace_writer_resume(&w)); assert(!pet_replace_writer_block(&w, content, 0x10000));
    pet_replace_t next = store.state;
    assert(pet_replace_request(&next, id, &target, "r0"));
    assert(pet_replace_journal_commit(&store, &next) == PET_JOURNAL_OK);
    assert(!pet_replace_writer_resume(&w)); assert(!erases && !writes);
    assert(pet_replace_fenced(&next, op, fence, &target));
    assert(pet_replace_journal_commit(&store, &next) == PET_JOURNAL_OK);
    journal_failure = true; assert(!pet_replace_writer_resume(&w));
    assert(!detached && !erases && !store.ready);
    journal_failure = false; reopen(&w);
    detach_failure = true; assert(!pet_replace_writer_resume(&w));
    assert(store.state.phase == PET_REPLACE_INVALIDATED && !erases);
    detach_failure = false; assert(pet_replace_writer_resume(&w));
    assert(!erases && !writes);
    uint8_t manifest[PET_REPLACE_MANIFEST_BYTES]; memset(manifest, 0x42, sizeof(manifest));
    assert(!pet_replace_writer_manifest(&w, manifest, sizeof(manifest)-1));
    assert(pet_replace_writer_manifest(&w, manifest, sizeof(manifest)));
    assert(!memcmp(flash + PET_REPLACE_MAX_BYTES, manifest, sizeof(manifest)));
    assert(!pet_replace_writer_block(&w, content, 12));
    assert(!pet_replace_writer_block(&w, content, sizeof(content)));
    write_failure = true; assert(!pet_replace_writer_block(&w, content, 0x10000));
    assert(store.state.downloaded_bytes == 0 && !w.resume_checked);
    assert(!pet_replace_writer_block(&w, content, 0x10000));
    write_failure = false; reopen(&w); assert(pet_replace_writer_resume(&w));
    corrupt_readback = true; assert(!pet_replace_writer_block(&w, content, 0x10000));
    assert(store.state.downloaded_bytes == 0); corrupt_readback = false;
    reopen(&w); assert(pet_replace_writer_resume(&w));
    journal_failure = true; assert(!pet_replace_writer_block(&w, content, 0x10000));
    assert(!store.ready && !w.resume_checked); journal_failure = false;
    reopen(&w); assert(store.state.downloaded_bytes == 0); assert(pet_replace_writer_resume(&w));
    assert(pet_replace_writer_block(&w, content, 0x10000));
    assert(store.state.downloaded_bytes == 0x10000);
    assert(!pet_replace_writer_manifest(&w, manifest, sizeof(manifest)));
    reopen(&w); flash[19] ^= 1; unsigned before = erases;
    assert(!pet_replace_writer_resume(&w));
    assert(store.state.phase == PET_REPLACE_INVALIDATED && !store.state.downloaded_bytes && erases == before);
    assert(!strcmp(store.state.target.sha256, target.sha256) && !strcmp(store.state.fence_id, fence));
    assert(pet_replace_writer_resume(&w)); assert(pet_replace_writer_block(&w, content, 0x10000));
    assert(pet_replace_writer_block(&w, content + 0x10000, 3));
    assert(!memcmp(flash, content, sizeof(content)));
    assert(!strcmp(store.state.prefix_sha256, target.sha256));
    assert(!memcmp(flash + PET_REPLACE_MAX_BYTES, manifest, sizeof(manifest)));
    assert(!pet_replace_writer_block(&w, content, 1));
    reopen(&w); assert(pet_replace_writer_resume(&w));
    next = store.state; assert(pet_replace_verified(&next, target.sha256));
    assert(pet_replace_journal_commit(&store, &next) == PET_JOURNAL_OK);
    assert(!pet_replace_writer_block(&w, content, 1));
    assert(!pet_replace_writer_resume(&w));
    pet_replace_writer_close(&w);
    puts("single-slot writer: invalidation-before-detach, read-back SHA-256, torn writes, saved-prefix corruption and metadata boundary passed");
    return 0;
}
