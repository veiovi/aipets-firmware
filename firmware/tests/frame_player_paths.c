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

#include "frame_player.h"

#define TICKS 3600u

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

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <pack> <re-encoded pack>\n", argv[0]); return 2; }
    uint32_t arena_bytes = fp_arena_size(), changes = 0;
    fp_player_t *players[2];
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
    for (uint32_t tick = 0; tick < TICKS; tick++) {
        for (int i = 0; i < 2; i++) { drive(players[i], tick); fp_tick(players[i]); }
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
    return 0;
}
