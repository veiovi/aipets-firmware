#pragma once

#include "frame_player.h"

/* One caller-owned buffer holds bookkeeping, resident tables/resources, two
 * 256 KiB clip windows, then an unused tail for story events. No heap or flash
 * mappings. The reader runs only during open or worker step, never fp_tick. */
#define FP_STREAM_WINDOW_BYTES 262144u

typedef struct fp_stream fp_stream_t;
typedef int (*fp_stream_read_t)(void *context, uint32_t offset, void *out, uint32_t bytes);

/* Open after the pack's full SHA-256 has been verified. prove requests full
 * codec/CRC validation; otherwise the same immutable bytes were proved by the
 * current validator revision. Runtime metadata must precede/follow the frame
 * payload bank as emitted by the canonical compiler. Authoring JSON stays in
 * flash and is covered by the hash and CRC.
 *
 * The caller detaches the old player and drains its worker before reusing the
 * buffer. On failure no player may retain any part of that buffer. */
fp_error_t fp_stream_open(void *buffer, uint32_t capacity, uint32_t pack_bytes,
    fp_stream_read_t read, void *context, int prove, uint32_t minimum_tail, fp_stream_t **out);
const fp_pack_source_t *fp_stream_source(const fp_stream_t *stream);
const fp_pack_info_t *fp_stream_info(const fp_stream_t *stream);
void *fp_stream_tail(fp_stream_t *stream, uint32_t *bytes);
uint32_t fp_stream_resident_bytes(const fp_stream_t *stream);

/* One storage worker; caller serializes step against open/buffer reuse.
 * 1 loaded a window, 0 idle/cancelled, -1 read failure (latched until reopen).
 * cancel can interrupt between read slices but does not drain an in-flight
 * read. The owner must drain it before erasing flash or reusing the buffer. */
int fp_stream_step(fp_stream_t *stream);
void fp_stream_cancel(fp_stream_t *stream);
int fp_stream_failed(const fp_stream_t *stream);
