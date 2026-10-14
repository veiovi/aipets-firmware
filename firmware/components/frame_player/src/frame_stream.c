#include "frame_stream.h"
#include "frame_internal.h"

#include <stdatomic.h>
#include <string.h>

enum
{
    EMPTY,
    LOADING,
    READY,
    PINNED
};

typedef struct
{
    atomic_uint state;
    uint16_t clip, first, end;
    uint32_t offsets[FP_MAX_FRAMES];
    uint8_t *bytes;
} window_t;

struct fp_stream
{
    fp_pack_source_t source;
    fp_pack_info_t info;
    fp_stream_read_t read;
    void *context;
    uint8_t *buffer, *scratch;
    uint32_t capacity, used, resident_bytes;
    uint32_t frames, clips;
    uint32_t resident[FP_MAX_FRAMES];
    uint16_t successor[FP_MAX_CLIPS];
    uint8_t resident_clip[FP_MAX_CLIPS];
    window_t windows[2];
    atomic_uint wanted;
    atomic_int active;
    atomic_int failed;
    int pinned;
};

static uint32_t aligned(uint32_t value)
{
    return (value + 15u) & ~15u;
}

static const uint8_t *frame_entry(const fp_stream_t *stream, uint16_t frame)
{
    return fp_source_at(&stream->source, fp_rd32(stream->source.spans[0].bytes + FP_HDR_FRAME_DIR_OFFSET) +
                                             (uint32_t)frame * FP_FRAME_DIR_ENTRY_BYTES);
}

static const uint8_t *clip_entry(const fp_stream_t *stream, uint16_t clip)
{
    return fp_source_at(&stream->source, fp_rd32(stream->source.spans[0].bytes + FP_HDR_CLIP_DIR_OFFSET) +
                                             (uint32_t)clip * FP_CLIP_DIR_ENTRY_BYTES);
}

static uint16_t clip_frame(const fp_stream_t *stream, uint16_t clip, uint16_t step)
{
    return fp_rd16(
        fp_source_at(&stream->source, fp_rd32(clip_entry(stream, clip) + 8u) + (uint32_t)step * FP_CLIP_STEP_BYTES));
}

static int read_source(void *context, uint32_t offset, void *out, uint32_t bytes)
{
    fp_stream_t *stream = context;
    return offset <= stream->source.bytes && bytes <= stream->source.bytes - offset &&
           stream->read(stream->context, offset, out, bytes);
}

static const uint8_t *read_resource(void *context, uint16_t frame, uint32_t offset, uint32_t length)
{
    fp_stream_t *stream = context;
    (void)frame;
    return length <= FP_STREAM_WINDOW_BYTES && read_source(stream, offset, stream->scratch, length) ? stream->scratch
                                                                                                    : NULL;
}

static const uint8_t *cached_resource(void *context, uint16_t frame, uint32_t offset, uint32_t length)
{
    fp_stream_t *stream = context;
    (void)offset;
    (void)length;
    if (frame >= stream->frames) return NULL;
    if (stream->resident[frame]) return stream->buffer + stream->resident[frame];
    if (stream->pinned < 0) return NULL;
    const window_t *window = &stream->windows[stream->pinned];
    return window->offsets[frame] ? window->bytes + window->offsets[frame] - 1u : NULL;
}

static unsigned request(uint16_t clip, uint16_t step)
{
    return (unsigned)clip + 1u + ((unsigned)step << 9u);
}

static void request_clip(fp_stream_t *stream, uint16_t clip, uint16_t step)
{
    atomic_store(&stream->wanted, clip < stream->clips && !stream->resident_clip[clip] ? request(clip, step) : 0u);
}

static int begin_frame(void *context, uint16_t clip, uint16_t step, uint16_t frame)
{
    fp_stream_t *stream = context;
    stream->pinned = -1;
    if (frame >= stream->frames) return 0;
    if (clip >= stream->clips || stream->resident_clip[clip])
    {
        request_clip(stream, clip < stream->clips ? stream->successor[clip] : FP_ACTION_NO_CLIP, 0);
        return stream->resident[frame] != 0;
    }
    for (int i = 0; i < 2; i++)
    {
        window_t *window = &stream->windows[i];
        unsigned expected = READY;
        if (!atomic_compare_exchange_strong(&window->state, &expected, PINNED)) continue;
        if (window->clip == clip && step >= window->first && step < window->end)
        {
            stream->pinned = i;
            atomic_store(&stream->active, i);
            uint16_t count = fp_rd16(clip_entry(stream, clip) + 12u);
            if (window->end < count) request_clip(stream, clip, window->end);
            else request_clip(stream, stream->successor[clip], 0);
            return stream->resident[frame] || window->offsets[frame];
        }
        atomic_store(&window->state, READY);
    }
    request_clip(stream, clip, step);
    /* A resident neutral at the start of a performance keeps playing while
     * its optional frames load. Only a missing resource holds the tick. */
    return stream->resident[frame] != 0;
}

static void end_frame(void *context)
{
    fp_stream_t *stream = context;
    if (stream->pinned >= 0) atomic_store(&stream->windows[stream->pinned].state, READY);
    stream->pinned = -1;
}

static void keep_clip(fp_stream_t *stream, uint16_t clip)
{
    if (clip == FP_ACTION_NO_CLIP) return;
    uint16_t count = fp_rd16(clip_entry(stream, clip) + 12u);
    for (uint16_t step = 0; step < count; step++) stream->resident[clip_frame(stream, clip, step)] = 1u;
}

static void choose_resident(fp_stream_t *stream, const fp_pack_view_t *view)
{
    for (uint32_t i = 0; i < FP_ROLE_COUNT; i++) keep_clip(stream, view->roles[i]);
    for (uint32_t i = 0; i < view->talk_stage_count; i++) stream->resident[view->talk_stage_frames[i]] = 1u;
    for (uint32_t i = 0; i < (uint32_t)view->speaking_pose_count * view->talk_stage_count; i++)
        stream->resident[view->speaking_frames[i]] = 1u;
    for (uint32_t i = 0; i < view->facial_group_count; i++)
    {
        const fp_facial_group_t *group = &view->facial_groups[i];
        stream->resident[group->base_frame] = 1u;
        for (uint32_t j = 0; j < 7u; j++) stream->resident[group->mouth_frames[j]] = 1u;
        for (uint32_t j = 0; j < 5u; j++) stream->resident[group->eye_frames[j]] = 1u;
    }
    for (uint32_t i = 0; i < view->action_count; i++)
    {
        const fp_action_entry_t *action = &view->actions[i];
        /* An interrupted optional action cannot postpone resident speech
         * while it loads an exit. Every overlay is resident as well. */
        keep_clip(stream, action->exit_clip);
        if (action->plane != FP_PLANE_BASE || action->activation == FP_ACTIVATION_SEMANTIC ||
            action->activation == FP_ACTIVATION_AMBIENT_LOOP)
        {
            keep_clip(stream, action->enter_clip);
            keep_clip(stream, action->clip);
        }
        if (action->enter_clip != FP_ACTION_NO_CLIP) stream->successor[action->enter_clip] = action->clip;
        stream->successor[action->clip] = action->exit_clip;
    }
    for (uint32_t i = 0; i < view->emotion_count; i++)
    {
        const fp_emotion_entry_t *emotion = &view->emotions[i];
        stream->successor[emotion->enter_clip] = emotion->hold_clip;
        stream->successor[emotion->hold_clip] = emotion->exit_clip;
    }
}

fp_error_t fp_stream_open(void *buffer, uint32_t capacity, uint32_t pack_bytes, fp_stream_read_t read, void *context,
                          int prove, uint32_t minimum_tail, fp_stream_t **out)
{
    if (out) *out = NULL;
    if (!buffer || ((uintptr_t)buffer & 15u) || !read || !out) return FP_ERR_ARGUMENT;
    uint32_t state_bytes = aligned(sizeof(fp_stream_t));
    if (capacity < state_bytes + FP_HEADER_BYTES + fp_arena_size() + FP_STREAM_WINDOW_BYTES ||
        minimum_tail > capacity - 2u * FP_STREAM_WINDOW_BYTES)
        return FP_ERR_ARENA;
    fp_stream_t *stream = buffer;
    memset(stream, 0, sizeof(*stream));
    stream->buffer = buffer;
    stream->capacity = capacity;
    stream->read = read;
    stream->context = context;
    stream->source.bytes = pack_bytes;
    stream->source.context = stream;
    stream->source.read = read_source;
    stream->source.frame = read_resource;
    stream->scratch = stream->buffer + capacity - FP_STREAM_WINDOW_BYTES;
    stream->pinned = -1;
    atomic_init(&stream->active, -1);
    atomic_init(&stream->wanted, 0);
    atomic_init(&stream->failed, 0);
    for (uint32_t i = 0; i < FP_MAX_CLIPS; i++) stream->successor[i] = FP_ACTION_NO_CLIP;
    for (uint32_t i = 0; i < 2u; i++) atomic_init(&stream->windows[i].state, EMPTY);

    uint8_t *header = stream->buffer + state_bytes;
    if (!read_source(stream, 0, header, FP_HEADER_BYTES)) return FP_ERR_TRUNCATED;
    uint32_t directory = fp_rd32(header + FP_HDR_FRAME_DIR_OFFSET);
    stream->frames = fp_rd16(header + FP_HDR_FRAME_COUNT);
    stream->clips = fp_rd16(header + FP_HDR_CLIP_COUNT);
    if (!stream->frames || stream->frames > FP_MAX_FRAMES || !stream->clips || stream->clips > FP_MAX_CLIPS ||
        directory < FP_HEADER_BYTES || directory > pack_bytes ||
        stream->frames * FP_FRAME_DIR_ENTRY_BYTES > pack_bytes - directory)
        return FP_ERR_BAD_BOUNDS;
    uint32_t prefix = directory + stream->frames * FP_FRAME_DIR_ENTRY_BYTES;
    uint32_t tail = fp_rd32(header + FP_HDR_CLIP_DIR_OFFSET);
    uint32_t meta = fp_rd32(header + FP_HDR_META_OFFSET), meta_bytes = fp_rd32(header + FP_HDR_META_LENGTH);
    uint32_t end = meta_bytes ? meta : pack_bytes;
    if (tail < prefix || tail > end || end > pack_bytes || meta_bytes > pack_bytes - end) return FP_ERR_BAD_BOUNDS;
    uint32_t available = capacity - 2u * FP_STREAM_WINDOW_BYTES - minimum_tail;
    if (prefix > available || end - tail > available - prefix ||
        state_bytes + aligned(prefix) + aligned(end - tail) > available)
        return FP_ERR_ARENA;
    stream->used = state_bytes + aligned(prefix) + aligned(end - tail);
    if (fp_arena_size() > capacity - FP_STREAM_WINDOW_BYTES - stream->used) return FP_ERR_ARENA;
    stream->source.spans[0] = (fp_pack_span_t){header, 0, prefix};
    stream->source.spans[1] = (fp_pack_span_t){header + aligned(prefix), tail, end - tail};
    if (!read_source(stream, 0, header, prefix) || !read_source(stream, tail, header + aligned(prefix), end - tail))
        return FP_ERR_TRUNCATED;
    for (uint32_t i = 0; i < stream->frames; i++)
    {
        const uint8_t *entry = frame_entry(stream, (uint16_t)i);
        uint32_t offset = fp_rd32(entry), length = fp_rd32(entry + 4u);
        if (offset < prefix || offset > tail || !length || length > tail - offset || length > FP_STREAM_WINDOW_BYTES)
            return FP_ERR_BAD_FRAME;
    }
    void *arena = stream->buffer + stream->used;
    fp_error_t error = FP_OK;
    if (prove)
    {
        error =
            fp_validate_source(&stream->source, NULL, arena, fp_arena_size(), stream->scratch, FP_STREAM_WINDOW_BYTES);
        if (error != FP_OK) return error;
    }
    fp_player_t *player = fp_player_init_source(arena, fp_arena_size(), &stream->source, 1u, &error);
    if (!player) return error;
    stream->info = player->info;
    choose_resident(stream, &player->view);
    /* No reference to the temporary player's arena survives this point. */
    for (uint32_t i = 0; i < stream->frames; i++)
    {
        if (!stream->resident[i]) continue;
        const uint8_t *entry = frame_entry(stream, (uint16_t)i);
        uint32_t length = fp_rd32(entry + 4u);
        if (aligned(length) > available - stream->used) return FP_ERR_ARENA;
        stream->resident[i] = stream->used;
        if (!read_source(stream, fp_rd32(entry), stream->buffer + stream->used, length)) return FP_ERR_TRUNCATED;
        stream->used += aligned(length);
    }
    for (uint16_t clip = 0; clip < stream->clips; clip++)
    {
        stream->resident_clip[clip] = 1u;
        uint16_t count = fp_rd16(clip_entry(stream, clip) + 12u);
        for (uint16_t step = 0; step < count; step++)
        {
            if (!stream->resident[clip_frame(stream, clip, step)]) stream->resident_clip[clip] = 0u;
        }
    }
    stream->resident_bytes = stream->used;
    for (uint32_t i = 0; i < 2u; i++)
    {
        stream->windows[i].bytes = stream->buffer + stream->used;
        stream->used += FP_STREAM_WINDOW_BYTES;
    }
    stream->source.frame = cached_resource;
    stream->source.begin = begin_frame;
    stream->source.end = end_frame;
    stream->scratch = NULL;
    *out = stream;
    return FP_OK;
}

int fp_stream_step(fp_stream_t *stream)
{
    if (!stream || atomic_load(&stream->failed)) return stream ? -1 : 0;
    unsigned wanted = atomic_load(&stream->wanted);
    if (!wanted) return 0;
    uint16_t clip = (uint16_t)((wanted & 511u) - 1u), first = (uint16_t)(wanted >> 9u);
    if (clip >= stream->clips) return 0;
    uint16_t count = fp_rd16(clip_entry(stream, clip) + 12u);
    if (first >= count) return 0;
    for (uint32_t i = 0; i < 2u; i++)
    {
        const window_t *window = &stream->windows[i];
        unsigned state = atomic_load(&window->state);
        if ((state == READY || state == PINNED) && window->clip == clip && first >= window->first &&
            first < window->end)
            return 0;
    }
    int active = atomic_load(&stream->active);
    window_t *window = &stream->windows[active == 0 ? 1 : 0];
    unsigned state = atomic_load(&window->state);
    if ((state != EMPTY && state != READY) || !atomic_compare_exchange_strong(&window->state, &state, LOADING))
        return 0;
    if (atomic_load(&stream->active) == (int)(window - stream->windows))
    {
        atomic_store(&window->state, state);
        return 0;
    }
    memset(window->offsets, 0, sizeof(window->offsets));
    uint32_t used = 0;
    uint16_t step = first;
    for (; step < count; step++)
    {
        uint16_t frame = clip_frame(stream, clip, step);
        if (stream->resident[frame] || window->offsets[frame]) continue;
        const uint8_t *entry = frame_entry(stream, frame);
        uint32_t offset = fp_rd32(entry), length = fp_rd32(entry + 4u);
        if (aligned(length) > FP_STREAM_WINDOW_BYTES - used) break;
        for (uint32_t copied = 0; copied < length;)
        {
            uint32_t chunk = length - copied;
            if (chunk > 8192u) chunk = 8192u;
            if (!read_source(stream, offset + copied, window->bytes + used + copied, chunk))
            {
                atomic_store(&stream->failed, 1);
                atomic_store(&window->state, EMPTY);
                return -1;
            }
            copied += chunk;
            if (atomic_load(&stream->wanted) != wanted)
            {
                atomic_store(&window->state, EMPTY);
                return 0;
            }
        }
        window->offsets[frame] = used + 1u;
        used += aligned(length);
    }
    window->clip = clip;
    window->first = first;
    window->end = step;
    atomic_store(&window->state, READY);
    return 1;
}

const fp_pack_source_t *fp_stream_source(const fp_stream_t *stream)
{
    return stream ? &stream->source : NULL;
}

const fp_pack_info_t *fp_stream_info(const fp_stream_t *stream)
{
    return stream ? &stream->info : NULL;
}

void *fp_stream_tail(fp_stream_t *stream, uint32_t *bytes)
{
    if (bytes) *bytes = stream ? stream->capacity - stream->used : 0u;
    return stream ? stream->buffer + stream->used : NULL;
}

uint32_t fp_stream_resident_bytes(const fp_stream_t *stream)
{
    return stream ? stream->resident_bytes : 0u;
}

void fp_stream_cancel(fp_stream_t *stream)
{
    if (stream) atomic_store(&stream->wanted, 0);
}

int fp_stream_failed(const fp_stream_t *stream)
{
    return stream && atomic_load(&stream->failed);
}
