/*
 * The player draws a full frame two ways: an RLE frame, or any frame with
 * something drawn over it, is composed on a scratch canvas and then copied
 * where it differs; a raw or zlib frame with nothing over it is compared with
 * the framebuffer in place. Given the same pack with its full frames in
 * different encodings, both must show the same pixels and dirty rectangles on
 * every tick, through idle bob, speech, speaking drift and its exposed edges.
 *
 *   frame_player_paths <pack.aipetframes> <same-pack-other-encoding.aipetframes>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "frame_player.h"
#include "frame_stream.h"

#define TICKS 3600u

typedef struct
{
    const uint8_t *pack;
    uint32_t bytes;
    int stalled;
    atomic_uint reads;
} source_t;

static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_changed = PTHREAD_COND_INITIALIZER;
static int block_read, entered_read, release_read;

static int read_source(void *context, uint32_t offset, void *out, uint32_t bytes)
{
    source_t *source = context;
    source->reads++;
    pthread_mutex_lock(&read_lock);
    if (block_read)
    {
        entered_read = 1;
        pthread_cond_broadcast(&read_changed);
        while (!release_read) pthread_cond_wait(&read_changed, &read_lock);
        block_read = 0;
    }
    pthread_mutex_unlock(&read_lock);
    assert(offset <= source->bytes && bytes <= source->bytes - offset);
    memcpy(out, source->pack + offset, bytes);
    return 1;
}

static const uint8_t *resource(void *context, uint16_t frame, uint32_t offset, uint32_t bytes)
{
    const source_t *source = context;
    (void)frame;
    assert(offset <= source->bytes && bytes <= source->bytes - offset);
    return source->stalled ? NULL : source->pack + offset;
}

static int begin(void *context, uint16_t clip, uint16_t step, uint16_t frame)
{
    (void)clip;
    (void)step;
    (void)frame;
    return !((source_t *)context)->stalled;
}

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = length > 0 ? malloc((size_t)length) : NULL;
    if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) { free(bytes); bytes = NULL; }
    fclose(file);
    *size = (uint32_t)length;
    return bytes;
}

static void drive(fp_player_t *player, uint32_t tick)
{
    uint32_t phase = tick % 1200u;
    fp_set_speaking_drift(player, (uint8_t)(tick >= 1200u));
    fp_set_sys_state(player, phase >= 300u && phase < 900u ? FP_SYS_SPEAKING : FP_SYS_IDLE);
    fp_set_audio_level(player, (uint8_t)((tick * 37u) % 101u));
    fp_set_tilt(player, (int8_t)(tick % 500u < 40u ? 90 : 0), 0);
    if (tick % 211u == 7u) fp_notify_touch(player);
}

static void *blocked_step(void *stream)
{
    return (void *)(intptr_t)fp_stream_step(stream);
}

/* A driver call that really blocks on another thread cannot stall a tick.
 * Resident speech cancels the pending optional clip without touching flash. */
static void stall_and_cancel(fp_stream_t *stream, fp_player_t *player, source_t *storage)
{
    pthread_mutex_lock(&read_lock);
    block_read = 1;
    entered_read = release_read = 0;
    pthread_mutex_unlock(&read_lock);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, blocked_step, stream));
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 5;
    pthread_mutex_lock(&read_lock);
    while (!entered_read) assert(!pthread_cond_timedwait(&read_changed, &read_lock, &deadline));
    pthread_mutex_unlock(&read_lock);
    unsigned reads = storage->reads;
    uint32_t crc = fp_frame_crc32(player), revision = fp_visual_revision(player);
    for (unsigned tick = 0; tick < 100u; tick++)
    {
        fp_tick(player);
        assert(!fp_tick_committed(player) && storage->reads == reads);
        assert(fp_frame_crc32(player) == crc && fp_visual_revision(player) == revision);
    }
    void *arena = aligned_alloc(16, fp_arena_size());
    fp_error_t error;
    fp_player_t *speech = fp_player_init_source(arena, fp_arena_size(), fp_stream_source(stream), 42, &error);
    assert(speech);
    fp_set_sys_state(speech, FP_SYS_SPEAKING);
    fp_set_audio_level(speech, 90);
    fp_tick(speech);
    assert(fp_tick_committed(speech) && storage->reads == reads);
    fp_stream_cancel(stream);
    pthread_mutex_lock(&read_lock);
    release_read = 1;
    pthread_cond_broadcast(&read_changed);
    pthread_mutex_unlock(&read_lock);
    void *result = NULL;
    assert(!pthread_join(worker, &result) && (intptr_t)result == 0);
    free(arena);
    fp_tick(player); /* The still-held director requests its clip again. */
    assert(!fp_tick_committed(player));
}

static void cache_paths(const char *path)
{
    source_t storage = {0};
    uint8_t *pack = load(path, &storage.bytes);
    storage.pack = pack;
    void *buffer = aligned_alloc(16, 4000000u);
    void *arenas[2] = {aligned_alloc(16, fp_arena_size()), aligned_alloc(16, fp_arena_size())};
    assert(pack && buffer && arenas[0] && arenas[1]);
    fp_stream_t *stream = NULL;
    uint32_t reserve = 256000u + fp_validation_workspace_size();
    fp_error_t error = fp_stream_open(buffer, 4000000u, storage.bytes, read_source, &storage, 1, reserve, &stream);
    if (error != FP_OK)
    {
        fprintf(stderr, "stream %s: %s\n", path, fp_error_string(error));
        abort();
    }
    uint32_t tail;
    assert(fp_stream_tail(stream, &tail) && tail >= reserve);
    fp_player_t *players[2] = {
        fp_player_init_prevalidated(arenas[0], fp_arena_size(), pack, storage.bytes, 42u, &error),
        fp_player_init_source(arenas[1], fp_arena_size(), fp_stream_source(stream), 42u, &error)};
    assert(players[0] && players[1]);
    uint32_t misses = 0, loaded = 0;
    for (uint32_t tick = 0; tick < 3600u; tick++)
    {
        for (uint32_t i = 0; i < 2u; i++)
        {
            drive(players[i], tick);
            if (tick % 300u == 20u) fp_request_action(players[i], 7);
        }
        uint32_t crc = fp_frame_crc32(players[1]), revision = fp_visual_revision(players[1]);
        unsigned reads = storage.reads;
        fp_tick(players[1]);
        assert(storage.reads == reads);
        if (!fp_tick_committed(players[1]))
        {
            misses++;
            if (misses == 1) stall_and_cancel(stream, players[1], &storage);
            reads = storage.reads;
            for (unsigned held = 0; held < 3u; held++)
            {
                assert(fp_frame_crc32(players[1]) == crc && fp_visual_revision(players[1]) == revision);
                fp_tick(players[1]);
                assert(!fp_tick_committed(players[1]) && storage.reads == reads);
            }
            assert(fp_stream_step(stream) == 1);
            loaded++;
            fp_tick(players[1]);
            assert(fp_tick_committed(players[1]));
        }
        fp_tick(players[0]);
        assert(fp_frame_crc32(players[0]) == fp_frame_crc32(players[1]));
        assert(fp_visual_revision(players[0]) == fp_visual_revision(players[1]));
        int work = misses ? fp_stream_step(stream) : 0;
        assert(work >= 0);
        loaded += work == 1;
    }
    assert(loaded && misses);
    printf("stream %s: resident %u, tail %u, windows %u, held %u; 3600 ticks identical\n", path,
           fp_stream_resident_bytes(stream), tail, loaded, misses);
    /* Hostile offsets never become pointers into a missing resident span. */
    uint8_t saved[4];
    memcpy(saved, pack + 40u, sizeof(saved));
    memset(pack + 40u, 255, 4u);
    assert(fp_stream_open(buffer, 4000000u, storage.bytes, read_source, &storage, 1, reserve, &stream) ==
           FP_ERR_BAD_BOUNDS);
    memcpy(pack + 40u, saved, sizeof(saved));
    pack[128] ^= 1u;
    assert(fp_stream_open(buffer, 4000000u, storage.bytes, read_source, &storage, 1, reserve, &stream) ==
           FP_ERR_BAD_CHECKSUM);
    pack[128] ^= 1u;
    assert(fp_stream_open(buffer, 4000000u, storage.bytes, read_source, &storage, 1,
                          4000000u - 2u * FP_STREAM_WINDOW_BYTES, &stream) == FP_ERR_ARENA);
    free(arenas[0]);
    free(arenas[1]);
    free(buffer);
    free(pack);
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <pack> <re-encoded pack> <stream pack>...\n", argv[0]);
        return 2;
    }
    uint32_t arena_bytes = fp_arena_size(), changes = 0;
    fp_player_t *players[3];
    uint8_t *packs[2] = {NULL, NULL};
    void *arenas[2] = {NULL, NULL};
    for (int i = 0; i < 2; i++) {
        uint32_t size = 0;
        uint8_t *pack = packs[i] = load(argv[1 + i], &size);
        void *arena = arenas[i] = aligned_alloc(16, ((size_t)arena_bytes + 15u) & ~(size_t)15u);
        fp_error_t error = FP_OK;
        players[i] = pack && arena ? fp_player_init(arena, arena_bytes, pack, size, 77u, &error) : NULL;
        if (!players[i]) { fprintf(stderr, "%s: %s\n", argv[1 + i], fp_error_string(error)); return 1; }
    }
    source_t storage = {.pack = packs[0], .bytes = fp_pack_info(players[0])->pack_bytes};
    fp_pack_source_t source = {.spans = {{storage.pack, 0, storage.bytes}},
                               .bytes = storage.bytes,
                               .context = &storage,
                               .read = read_source,
                               .frame = resource,
                               .begin = begin};
    void *stream_arena = aligned_alloc(16, arena_bytes);
    void *workspace = aligned_alloc(16, fp_validation_workspace_size());
    uint8_t scratch[257];
    assert(stream_arena && workspace);
    assert(fp_validate_source(&source, NULL, workspace, fp_validation_workspace_size(), scratch, sizeof(scratch)) ==
           FP_OK);
    free(workspace);
    fp_error_t error;
    fp_pack_source_t missing_tables = source;
    missing_tables.spans[0].length = 128u;
    assert(!fp_player_init_source(stream_arena, arena_bytes, &missing_tables, 77u, &error));
    assert(error == FP_ERR_BAD_BOUNDS);
    /* Adjacent spans have separate allocations. The palette starts in the
     * second span, not at the first allocation's one-past pointer. */
    uint8_t *header = malloc(128u);
    uint8_t *body = malloc(storage.bytes - 128u);
    assert(header && body);
    memcpy(header, storage.pack, 128u);
    memcpy(body, storage.pack + 128u, storage.bytes - 128u);
    source.spans[0] = (fp_pack_span_t){header, 0, 128u};
    source.spans[1] = (fp_pack_span_t){body, 128u, storage.bytes - 128u};
    players[2] = fp_player_init_source(stream_arena, arena_bytes, &source, 77u, &error);
    assert(players[2] && error == FP_OK);
    for (uint32_t tick = 0; tick < TICKS; tick++) {
        for (int i = 0; i < 2; i++) { drive(players[i], tick); fp_tick(players[i]); }
        drive(players[2], tick);
        if (tick % 17u == 3u)
        {
            uint32_t crc = fp_frame_crc32(players[2]), revision = fp_visual_revision(players[2]);
            storage.stalled = 1;
            for (int held = 0; held < 3; held++)
            {
                fp_tick(players[2]);
                assert(!fp_tick_committed(players[2]));
                assert(fp_frame_crc32(players[2]) == crc && fp_visual_revision(players[2]) == revision);
            }
            storage.stalled = 0;
        }
        fp_tick(players[2]);
        assert(fp_tick_committed(players[2]));
        assert(fp_frame_crc32(players[2]) == fp_frame_crc32(players[0]));
        assert(fp_visual_revision(players[2]) == fp_visual_revision(players[0]));
        fp_dirty_rect_t a = fp_dirty_rect(players[0]), b = fp_dirty_rect(players[1]);
        if (fp_frame_crc32(players[0]) != fp_frame_crc32(players[1]) ||
            fp_visual_revision(players[0]) != fp_visual_revision(players[1]) ||
            a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height) {
            fprintf(stderr, "tick %u: the encodings render differently\n", tick);
            return 1;
        }
        if (a.width && a.height) changes++;
    }
    printf("%u ticks, %u visible changes, both encodings identical\n", TICKS, changes);
    for (int i = 0; i < 2; i++) { free(arenas[i]); free(packs[i]); }
    free(stream_arena);
    free(header);
    free(body);
    for (int i = 3; i < argc; i++) cache_paths(argv[i]);
    return 0;
}
