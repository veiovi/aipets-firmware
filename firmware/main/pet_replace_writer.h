#pragma once
#include "pet_replace_journal.h"
#include "mbedtls/sha256.h"

typedef struct {
    bool (*read)(void *context, uint32_t offset, void *data, size_t bytes);
    bool (*erase)(void *context, uint32_t offset, size_t bytes);
    bool (*write)(void *context, uint32_t offset, const void *data, size_t bytes);
    void (*freeze)(void *context); /* Stop conversation/audio; idempotent. */
    bool (*detach)(void *context); /* Release renderer references AND mappings. */
    void *context;
    uint32_t partition_bytes; /* Validated physical single-pet partition. */
} pet_replace_io_t;
typedef struct {
    pet_replace_journal_t *store;
    pet_replace_io_t io;
    mbedtls_sha256_context prefix;
    uint32_t checked_bytes;
    char checked_operation[37], checked_fence[37];
    pet_replace_pack_t checked_target;
    bool initialized, detached, resume_checked;
} pet_replace_writer_t;
/* One serialized flash worker owns the writer. Firmware OTA must reserve the
 * same runtime gate. No concurrent renderers may bypass the detach callback. */
bool pet_replace_writer_init(pet_replace_writer_t *writer, pet_replace_journal_t *store,
                              const pet_replace_io_t *io);
void pet_replace_writer_close(pet_replace_writer_t *writer);
/* FENCED -> durable INVALIDATED -> detach -> durable DOWNLOADING, no erases.
 * Reopens may also continue INVALIDATED/DOWNLOADING after verifying progress. */
bool pet_replace_writer_resume(pet_replace_writer_t *writer);
/* Each 64 KiB checkpoint is erased, written, read back, hashed, then journaled.
 * A failed write invalidates the in-memory hash; resume must revalidate it. */
bool pet_replace_writer_block(pet_replace_writer_t *writer, const void *data, size_t bytes);
/* The signed envelope is opaque to this I/O layer; the control worker must
 * verify its signature, exact target and requirements BEFORE obtaining a fence.
 * This is allowed only before the first block. read-back is mandatory. */
bool pet_replace_writer_manifest(pet_replace_writer_t *writer, const void *data, size_t bytes);
