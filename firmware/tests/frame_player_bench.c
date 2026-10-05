/*
 * Host benchmark and byte-identity probe for the frame player's decode and
 * present path, built from the same sources as the firmware:
 *
 *   cc -std=c11 -O2 -I firmware/components/frame_player/include \
 *     -I firmware/components/frame_player/src firmware/tests/frame_player_bench.c \
 *     firmware/components/frame_player/src/(all .c files) \
 *     firmware/components/frame_player/vendor/miniz/miniz_tinfl.c -o frame_player_bench
 *   ./frame_player_bench [-r reps] [-t ticks] [-offsets] <.aipetframes packs>
 *
 * Per pack:
 *   A  inflate of every compressed full frame: the validator's path (zlib
 *      wrapper and Adler-32) and the renderer's in-place path;
 *   B  every step of every base clip, in playback order, through fp_render
 *      with the decode cache cleared (inflate and compose), then again from the
 *      same framebuffer with the frame already decoded (compose only), and the
 *      360x360 LCD expansion that firmware/main/pet_face_pack.c presents;
 *   C  the director for a scripted input sequence (showcase and natural idle,
 *      speech with drift, listening, thinking, touches, gestures, tilts,
 *      actions, profiles), presented like pet_face_pack.c.
 * -offsets gives the bound view an authored bob and 1 px parallax, so every
 * render path sees whole-frame offsets. Every framebuffer, dirty rectangle and
 * expanded canvas is hashed: two builds render identically when their
 * "identity" lines match. Times are host medians, not device times.
 * Build with -DFP_BENCH_BASELINE against sources older than the in-place path.
 */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 on Linux */

#include "frame_internal.h"
#include "frame_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void hash_bytes(uint64_t *hash, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { *hash ^= p[i]; *hash *= 0x100000001b3ull; }
}

static void hash_dirty(uint64_t *hash, fp_dirty_rect_t d, uint32_t revision)
{
    uint32_t words[3] = { (uint32_t)(uint16_t)d.x | ((uint32_t)(uint16_t)d.y << 16),
                          (uint32_t)d.width | ((uint32_t)d.height << 16), revision };
    hash_bytes(hash, words, sizeof words);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Sorts v. */
static double pct_us(uint64_t *v, size_t n, double p)
{
    if (!n) return 0.0;
    qsort(v, n, sizeof *v, cmp_u64);
    return (double)v[(size_t)(p * (double)(n - 1) + 0.5)] / 1e3;
}

static void print_dist(const char *label, uint64_t *v, size_t n)
{
    printf("   %-44s n=%-4zu p50=%7.1fus p95=%7.1fus max=%7.1fus\n", label, n,
           pct_us(v, n, 0.5), pct_us(v, n, 0.95), pct_us(v, n, 1.0));
}

typedef struct { uint32_t frame, bytes; uint8_t codec; uint64_t ns; } frame_cost_t;

static int cmp_cost(const void *a, const void *b)
{
    uint64_t x = ((const frame_cost_t *)a)->ns, y = ((const frame_cost_t *)b)->ns;
    return x > y ? -1 : x < y;
}

static const char *codec_name(uint8_t c)
{
    static const char *names[] = { "full", "masked", "dense", "full_rle", "masked_rle", "full_zlib", "masked_zlib", "dense_zlib" };
    return c < 8u ? names[c] : "?";
}

static void print_heaviest(const char *label, frame_cost_t *costs, size_t n)
{
    qsort(costs, n, sizeof *costs, cmp_cost);
    printf("   heaviest %s:", label);
    uint32_t shown[5];
    size_t count = 0;
    for (size_t i = 0; i < n && count < 5u; i++) {
        int seen = 0;
        for (size_t k = 0; k < count; k++) seen |= shown[k] == costs[i].frame;
        if (seen) continue;
        shown[count++] = costs[i].frame;
        printf(" #%u %s %uB %.1fus;", costs[i].frame, codec_name(costs[i].codec), costs[i].bytes, (double)costs[i].ns / 1e3);
    }
    printf("\n");
}

/* The adapter's presentation: union of dirty rectangles since the last
 * present; more than 60% of the canvas becomes a full redraw. */
typedef struct { fp_dirty_rect_t pending; int valid; } present_t;

static uint16_t display[360u * 360u];

static void pend(present_t *s, fp_dirty_rect_t next)
{
    if (!next.width || !next.height) return;
    if (!s->valid) { s->pending = next; s->valid = 1; return; }
    int x0 = s->pending.x < next.x ? s->pending.x : next.x, y0 = s->pending.y < next.y ? s->pending.y : next.y;
    int x1 = s->pending.x + s->pending.width, y1 = s->pending.y + s->pending.height;
    if (next.x + next.width > x1) x1 = next.x + next.width;
    if (next.y + next.height > y1) y1 = next.y + next.height;
    s->pending = (fp_dirty_rect_t){ (int16_t)x0, (int16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0) };
}

static uint64_t present(const fp_player_t *p, present_t *s, uint64_t *hash)
{
    uint16_t size = fp_canvas_width(p);
    fp_dirty_rect_t full = { 0, 0, size, size }, d = s->valid ? s->pending : full;
    if ((uint32_t)d.width * d.height > (uint32_t)size * size * 3u / 5u) d = full;
    uint64_t t0 = now_ns();
    fp_display_expand(fp_framebuffer(p), size, size, display, 360, d);
    uint64_t t1 = now_ns();
    s->valid = 0;
    if (hash) hash_bytes(hash, display, sizeof display);
    return t1 - t0;
}

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = length > 0 ? aligned_alloc(64, ((size_t)length + 63u) & ~(size_t)63u) : NULL;
    if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) { free(bytes); bytes = NULL; }
    fclose(file);
    *size = (uint32_t)length;
    return bytes;
}

static void script(fp_player_t *p, int t, int ticks, uint32_t actions)
{
    int seg = t % 6000;
    fp_set_idle_mode(p, t < ticks / 2 ? FP_IDLE_SHOWCASE : FP_IDLE_NATURAL);
    fp_set_animation_profile(p, (t / 2500) % 3 == 2 ? FP_PROFILE_FULL : FP_PROFILE_BALANCED);
    fp_sys_state_t state = FP_SYS_IDLE;
    if (seg >= 3000 && seg < 3900) state = FP_SYS_SPEAKING;
    else if (seg >= 3900 && seg < 4200) state = FP_SYS_LISTENING;
    else if (seg >= 4200 && seg < 4500) state = FP_SYS_THINKING;
    else if (seg >= 5900) state = FP_SYS_CONNECTING;
    fp_set_sys_state(p, state);
    fp_set_speaking_drift(p, (uint8_t)((t / 6000) & 1));
    fp_set_audio_level(p, (uint8_t)(state == FP_SYS_SPEAKING ? (t * 37) % 101 : 0));
    if (t % 480 == 100) fp_set_emotion(p, (fp_emotion_t)(1 + (t / 480) % 14));
    if (t % 480 == 400) fp_set_emotion(p, FP_EMO_NEUTRAL);
    if (t % 211 == 7) fp_notify_touch(p);
    if (t % 331 == 11) fp_notify_gesture(p, (fp_gesture_t)(1 + (t / 331) % 6));
    if (t % 997 == 13) fp_notify_shake(p);
    if (t % 700 < 60) fp_set_tilt(p, (int8_t)((t / 700) & 1 ? 80 : -80), (int8_t)((t / 1400) & 1 ? 60 : 0));
    else fp_set_tilt(p, 0, 0);
    if (t % 1500 == 50) fp_request_action(p, (int16_t)((uint32_t)(t / 1500) % (actions ? actions : 1u)));
}

static void offsets(fp_player_t *p, int enabled)
{
    if (!enabled) return;
    p->view.bob_amplitude_px = 2; p->view.bob_period_ticks = 6;
    p->view.parallax_safe = 1; p->view.max_parallax_px = 1;
}

int main(int argc, char **argv)
{
    int reps = 9, ticks = 12000, offset = 0, first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (!strcmp(argv[first], "-r") && first + 1 < argc) reps = atoi(argv[++first]);
        else if (!strcmp(argv[first], "-t") && first + 1 < argc) ticks = atoi(argv[++first]);
        else if (!strcmp(argv[first], "-offsets")) offset = 1;
        else break;
    }
    if (first >= argc || reps < 1 || ticks < 1) {
        fprintf(stderr, "usage: %s [-r reps] [-t ticks] [-offsets] pack.aipetframes...\n", argv[0]);
        return 2;
    }
    uint32_t arena_bytes = fp_arena_size(), work_bytes = fp_validation_workspace_size();
    void *arena = aligned_alloc(64, ((size_t)arena_bytes + 63u) & ~(size_t)63u);
    fp_codec_workspace_t *work = aligned_alloc(64, ((size_t)work_bytes + 63u) & ~(size_t)63u);
    uint8_t *indices = malloc(FP_MAX_PIXELS);
    uint16_t *before = malloc(FP_MAX_PIXELS * 2u), *after = malloc(FP_MAX_PIXELS * 2u);
    uint64_t *samples = calloc((size_t)reps, sizeof *samples);
    if (!arena || !work || !indices || !before || !after || !samples) return 1;

    for (int a = first; a < argc; a++) {
        uint32_t size = 0;
        uint8_t *pack = load(argv[a], &size);
        fp_pack_info_t info;
        fp_error_t error = pack ? fp_validate_with_workspace(pack, size, &info, work, work_bytes) : FP_ERR_ARGUMENT;
        fp_player_t *p = error == FP_OK ? fp_player_init_prevalidated(arena, arena_bytes, pack, size, 1u, &error) : NULL;
        if (!p) { fprintf(stderr, "%s: %s\n", argv[a], fp_error_string(error)); return 1; }
        const fp_pack_view_t *v = &p->view;
        const uint32_t canvas = info.width, pixels = canvas * canvas;
        printf("== %s: %.*s %.*s, %u bytes, %u frames, %u px, codecs 0x%02x\n", argv[a], info.id_len,
               (const char *)info.id, info.version_len, (const char *)info.version, size, info.frame_count, canvas, info.codecs);

        /* A. Inflate. */
        uint64_t codec_hash = 0xcbf29ce484222325ull;
        frame_cost_t *costs = calloc(info.frame_count, sizeof *costs);
        uint64_t *validate_ns = calloc(info.frame_count, sizeof *validate_ns), *render_inflate_ns = calloc(info.frame_count, sizeof *render_inflate_ns);
        size_t full_zlib = 0;
        for (uint32_t f = 0; f < info.frame_count; f++) {
            const uint8_t *entry = pack + v->frame_dir_offset + f * FP_FRAME_DIR_ENTRY_BYTES;
            uint32_t length = fp_rd32(entry + 4u);
            if (!fp_zlib_codec(entry[8])) continue;
            const uint8_t *decoded = NULL;
            uint32_t decoded_length = 0;
            for (int r = 0; r < reps; r++) {
                uint64_t t0 = now_ns();
                decoded = fp_decode_resource(pack + fp_rd32(entry), length, entry[8], canvas, work, &decoded_length);
                samples[r] = now_ns() - t0;
            }
            if (!decoded) { fprintf(stderr, "frame %u does not decode\n", f); return 1; }
            hash_bytes(&codec_hash, decoded, decoded_length);
            if (entry[8] != FP_CODEC_INDEX8_FULL_ZLIB) continue;
            validate_ns[full_zlib] = (uint64_t)(pct_us(samples, (size_t)reps, 0.5) * 1e3);
            costs[full_zlib] = (frame_cost_t){ f, length, entry[8], validate_ns[full_zlib] };
#ifndef FP_BENCH_BASELINE
            for (int r = 0; r < reps; r++) {
                uint64_t t0 = now_ns();
                int ok = fp_inflate_full_frame(pack + fp_rd32(entry), length, canvas, work, indices);
                samples[r] = now_ns() - t0;
                if (!ok) { fprintf(stderr, "frame %u does not inflate in place\n", f); return 1; }
            }
            decoded = fp_decode_resource(pack + fp_rd32(entry), length, entry[8], canvas, work, &decoded_length);
            if (decoded_length != pixels || memcmp(decoded, indices, pixels)) { fprintf(stderr, "frame %u inflates differently\n", f); return 1; }
            render_inflate_ns[full_zlib] = (uint64_t)(pct_us(samples, (size_t)reps, 0.5) * 1e3);
#endif
            full_zlib++;
        }
        if (full_zlib) {
            print_dist("A inflate, validator (zlib + Adler-32)", validate_ns, full_zlib);
#ifndef FP_BENCH_BASELINE
            print_dist("A inflate, renderer (in place)", render_inflate_ns, full_zlib);
#endif
            print_heaviest("inflate", costs, full_zlib);
        }

        /* B. Every base-clip step in playback order. */
        uint64_t frames_hash = 0xcbf29ce484222325ull;
        uint32_t steps = 0;
        for (uint32_t c = 0; c < v->clip_count; c++) steps += fp_clip_step_count(p, c);
        frame_cost_t *renders = calloc(steps, sizeof *renders);
        uint64_t *render_ns = calloc((size_t)steps * (size_t)reps, sizeof *render_ns);
        uint64_t *compose_ns = calloc((size_t)steps * (size_t)reps, sizeof *compose_ns);
        uint64_t *expand_ns = calloc((size_t)steps * (size_t)reps, sizeof *expand_ns);
        uint32_t n_steps = 0;
        for (int r = 0; r < reps; r++) {
            present_t shown = { { 0, 0, 0, 0 }, 0 };
            n_steps = 0;
            for (uint32_t c = 0; c < v->clip_count; c++) {
                uint32_t count = fp_clip_step_count(p, c);
                int base = 1;
                for (uint32_t s = 0; s < count; s++)
                    base &= fp_full_codec(pack[v->frame_dir_offset + (uint32_t)fp_clip_step_at(p, c, s).frame * FP_FRAME_DIR_ENTRY_BYTES + 8u]);
                for (uint32_t s = 0; base && s < count; s++, n_steps++) {
                    size_t at = (size_t)n_steps * (size_t)reps + (size_t)r;
                    p->phase = FP_PHASE_SYS_STATIC;
                    p->clip_index = (int32_t)c;
                    p->clip_step = (int32_t)s;
                    p->decoded_base_frame = -1;
                    p->render_initialized = 0;
                    uint32_t revision = fp_visual_revision(p);
                    memcpy(before, p->framebuffer, pixels * 2u);
                    uint64_t t0 = now_ns();
                    fp_render(p);
                    render_ns[at] = now_ns() - t0;
                    fp_dirty_rect_t d = fp_dirty_rect(p);
                    if (fp_visual_revision(p) != revision) {
                        pend(&shown, d);
                        expand_ns[at] = present(p, &shown, r == 0 ? &frames_hash : NULL);
                    }
                    if (r == 0) {
                        uint16_t frame = fp_clip_step_at(p, c, s).frame;
                        const uint8_t *entry = pack + v->frame_dir_offset + (uint32_t)frame * FP_FRAME_DIR_ENTRY_BYTES;
                        renders[n_steps] = (frame_cost_t){ frame, fp_rd32(entry + 4u), entry[8], 0 };
                        hash_bytes(&frames_hash, p->framebuffer, pixels * 2u);
                        hash_dirty(&frames_hash, d, fp_visual_revision(p) - revision);
                    }
                    /* Again from the same framebuffer, the frame already decoded. */
                    memcpy(after, p->framebuffer, pixels * 2u);
                    memcpy(p->framebuffer, before, pixels * 2u);
#ifndef FP_BENCH_BASELINE
                    p->plain_valid = 0;
#endif
                    t0 = now_ns();
                    fp_render(p);
                    compose_ns[at] = now_ns() - t0;
                    if (memcmp(after, p->framebuffer, pixels * 2u)) { fprintf(stderr, "compose-only render differs\n"); return 1; }
                }
            }
        }
        if (n_steps) {
            uint64_t *rn = calloc(n_steps, sizeof *rn), *cn = calloc(n_steps, sizeof *cn), *en = calloc(n_steps, sizeof *en);
            size_t n_expand = 0;
            for (uint32_t i = 0; i < n_steps; i++) {
                rn[i] = renders[i].ns = (uint64_t)(pct_us(&render_ns[(size_t)i * (size_t)reps], (size_t)reps, 0.5) * 1e3);
                cn[i] = (uint64_t)(pct_us(&compose_ns[(size_t)i * (size_t)reps], (size_t)reps, 0.5) * 1e3);
                uint64_t e = (uint64_t)(pct_us(&expand_ns[(size_t)i * (size_t)reps], (size_t)reps, 0.5) * 1e3);
                if (e) en[n_expand++] = e;
            }
            print_dist("B render: inflate + compose", rn, n_steps);
            print_dist("B render: compose only", cn, n_steps);
            print_dist("B present: 360x360 expansion", en, n_expand);
            print_heaviest("render", renders, n_steps);
            free(rn); free(cn); free(en);
        }

        /* C. The director under scripted input. */
        uint64_t ticks_hash = 0xcbf29ce484222325ull;
        p = fp_player_init_prevalidated(arena, arena_bytes, pack, size, 12345u, &error);
        offsets(p, offset);
        present_t shown = { { 0, 0, (uint16_t)canvas, (uint16_t)canvas }, 1 };
        memset(display, 0, sizeof display);
        present(p, &shown, &ticks_hash);
        uint32_t presented = fp_visual_revision(p), decodes = fp_debug_base_decode_count(p);
        uint64_t *tick_ns = calloc((size_t)ticks, sizeof *tick_ns), *present_ns = calloc((size_t)ticks, sizeof *present_ns);
        size_t n_present = 0;
        for (int t = 0; t < ticks; t++) {
            script(p, t, ticks, v->action_count);
            uint64_t t0 = now_ns();
            fp_tick(p);
            tick_ns[t] = now_ns() - t0;
            uint32_t revision = fp_visual_revision(p);
            hash_bytes(&ticks_hash, p->framebuffer, pixels * 2u);
            hash_dirty(&ticks_hash, fp_dirty_rect(p), revision);
            if (revision == presented) continue;
            pend(&shown, fp_dirty_rect(p));
            present_ns[n_present++] = present(p, &shown, &ticks_hash);
            presented = revision;
        }
        printf("   C %d ticks, %u inflates, %zu presents\n", ticks, fp_debug_base_decode_count(p) - decodes, n_present);
        print_dist("C tick (fp_tick)", tick_ns, (size_t)ticks);
        print_dist("C present", present_ns, n_present);
        printf("   identity codec=%016llx frames=%016llx ticks=%016llx\n", (unsigned long long)codec_hash,
               (unsigned long long)frames_hash, (unsigned long long)ticks_hash);
        free(costs); free(validate_ns); free(render_inflate_ns); free(renders); free(render_ns); free(compose_ns);
        free(expand_ns); free(tick_ns); free(present_ns); free(pack);
    }
    free(samples); free(after); free(before); free(indices); free(work); free(arena);
    return 0;
}
