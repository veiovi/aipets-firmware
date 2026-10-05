/*
 * Move packs (FP_FEATURE_MOVE) in the portable C validator, natively under
 * ASan/UBSan. The committed move seed validates and no player binds it, and
 * every byte edit of the frame-pack compiler's move-pack tests gets the same
 * verdict here.
 * usage: frame_player_move_test <v2-move> <v2-features>
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_player.h"

static uint8_t *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); exit(2); }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) { perror(path); exit(2); }
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc & 1u) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xffffffffu;
}

/* Header offsets: length 16, CRCs 20 and 24, flags 28, frames 44, clip
 * directory 48, roles 52, emotions 56 and 60, actions 62 and 68, talk 64,
 * idle 72. */
static void restamp(uint8_t *pack, size_t size)
{
    wr32(pack + 16, (uint32_t)size);
    wr32(pack + 20, crc32(pack + 128, size - 128));
    wr32(pack + 24, 0u);
    wr32(pack + 24, crc32(pack, 128));
}

static void *workspace;

static fp_error_t validate(const uint8_t *pack, size_t size, fp_pack_info_t *info)
{
    return fp_validate_with_workspace(pack, (uint32_t)size, info, workspace, fp_validation_workspace_size());
}

enum {
    SEED, LONGEST, UNAPPROVED, LIFECYCLE, SPEAKING, FRAMES_65, ROLES, EMOTION, EMOTIONS_OFFSET, TALK, IDLE,
    LOOPING, HOLDING, SHORT, LONG, EFFECT_FRAME, STATE_PLANE, MOVE_CLIP, EXIT_CLIP, LOOPING_EFFECT, SHORT_EFFECT,
    NO_EFFECT, BIG, EDITS,
};
static const struct { const char *name; fp_error_t code; } expected[EDITS] = {
    [SEED] = { "the committed seed: 30 ticks", FP_OK },
    [LONGEST] = { "the longest move: 364 ticks", FP_OK },
    [UNAPPROVED] = { "unapproved", FP_OK },
    [LIFECYCLE] = { "another feature (action lifecycle)", FP_ERR_BAD_MOVE },
    [SPEAKING] = { "speaking poses", FP_ERR_BAD_MOVE },
    [FRAMES_65] = { "65 frames", FP_ERR_BAD_MOVE },
    [ROLES] = { "a roles table", FP_ERR_BAD_MOVE },
    [EMOTION] = { "an emotion", FP_ERR_BAD_MOVE },
    [EMOTIONS_OFFSET] = { "an emotions offset", FP_ERR_BAD_MOVE },
    [TALK] = { "a talk table", FP_ERR_BAD_MOVE },
    [IDLE] = { "an idle section", FP_ERR_BAD_MOVE },
    [LOOPING] = { "a looping move", FP_ERR_BAD_MOVE },
    [HOLDING] = { "a move that holds its last frame", FP_ERR_BAD_MOVE },
    [SHORT] = { "29 ticks", FP_ERR_BAD_MOVE },
    [LONG] = { "365 ticks", FP_ERR_BAD_MOVE },
    [EFFECT_FRAME] = { "an effect frame in the move", FP_ERR_BAD_MOVE },
    [STATE_PLANE] = { "the effect on the state plane", FP_ERR_BAD_MOVE },
    [MOVE_CLIP] = { "the effect on the move's clip", FP_ERR_BAD_OVERLAY },
    [EXIT_CLIP] = { "the effect with an exit clip", FP_ERR_BAD_MOVE },
    [LOOPING_EFFECT] = { "a looping effect", FP_ERR_BAD_MOVE },
    [SHORT_EFFECT] = { "an effect shorter than the move", FP_ERR_BAD_MOVE },
    [NO_EFFECT] = { "a second clip without an effect", FP_ERR_BAD_MOVE },
    [BIG] = { "300,001 bytes", FP_ERR_BAD_MOVE },
};

/* A copy of the seed with one edit and its CRCs restamped. The seed's move clip
 * is four steps of 5, 10, 10 and 5 ticks; its effect clip two of 15. */
static uint8_t *edit(const uint8_t *seed, size_t seed_size, int which, size_t *size)
{
    *size = which == BIG ? 300001u : seed_size;
    uint8_t *p = calloc(1, *size);
    assert(p);
    memcpy(p, seed, seed_size);
    uint32_t clips = rd32(p + 48), move_steps = rd32(p + clips + 8), effect_steps = rd32(p + clips + 16u + 8u);
    uint32_t action = rd32(p + 68), flags = rd16(p + 28);
    switch (which) {
    case LONGEST: wr16(p + move_steps + 6, 344); wr16(p + effect_steps + 2, 349); break;
    case UNAPPROVED: wr16(p + 28, flags & ~1u); break;
    case LIFECYCLE: wr16(p + 28, flags | 8u); break;
    case SPEAKING: wr16(p + 28, flags | 32u); break;
    case FRAMES_65: wr16(p + 44, 65); break;
    case ROLES: wr32(p + 52, 128); break;
    case EMOTION: wr16(p + 60, 1); break;
    case EMOTIONS_OFFSET: wr32(p + 56, 128); break;
    case TALK: wr32(p + 64, 128); break;
    case IDLE: wr32(p + 72, 128); break;
    case LOOPING: p[clips + 6] = 1; break;
    case HOLDING: p[clips + 6] = 2; break;
    case SHORT: wr16(p + move_steps + 6, 9); wr16(p + effect_steps + 2, 14); break;
    case LONG: wr16(p + move_steps + 6, 345); wr16(p + effect_steps + 2, 350); break;
    case EFFECT_FRAME: wr16(p + move_steps + 4, 3); break;
    case STATE_PLANE: p[action + 1] = 2; break;
    case MOVE_CLIP: wr16(p + action + 4, 0); break;
    case EXIT_CLIP: wr16(p + action + 6, 1); break;
    case LOOPING_EFFECT: p[clips + 16 + 6] = 1; break;
    case SHORT_EFFECT: wr16(p + effect_steps + 2, 14); break;
    case NO_EFFECT: wr16(p + 62, 0); wr16(p + 28, flags & ~2u); break;
    default: break;
    }
    restamp(p, *size);
    return p;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <v2-move> <v2-features>\n", argv[0]);
        return 2;
    }
    size_t move_size, core_size;
    uint8_t *move = load(argv[1], &move_size), *core = load(argv[2], &core_size);
    workspace = aligned_alloc(16, fp_validation_workspace_size());
    assert(workspace);

    fp_pack_info_t info = {0};
    assert(validate(move, move_size, &info) == FP_OK && info.move == 1u && info.approved && info.clip_count == 2u);
    assert(validate(core, core_size, &info) == FP_OK && info.move == 0u);

    /* No player binds a move: it has no roles to direct. */
    uint32_t arena_bytes = fp_arena_size();
    void *arena = aligned_alloc(16, arena_bytes);
    assert(arena);
    fp_error_t error = FP_OK;
    assert(!fp_player_init(arena, arena_bytes, move, (uint32_t)move_size, 1u, &error) && error == FP_ERR_ARGUMENT);
    assert(!fp_player_init_prevalidated(arena, arena_bytes, move, (uint32_t)move_size, 1u, &error) && error == FP_ERR_ARGUMENT);

    for (int which = 0; which < EDITS; which++) {
        size_t size;
        uint8_t *pack = edit(move, move_size, which, &size);
        fp_error_t got = validate(pack, size, NULL);
        if (got != expected[which].code) {
            fprintf(stderr, "%s: %s, expected %s\n", expected[which].name, fp_error_string(got), fp_error_string(expected[which].code));
            return 1;
        }
        free(pack);
    }
    uint8_t *flagged = malloc(core_size);
    assert(flagged);
    memcpy(flagged, core, core_size);
    wr16(flagged + 28, rd16(flagged + 28) | 64u);
    restamp(flagged, core_size);
    assert(validate(flagged, core_size, NULL) == FP_ERR_BAD_MOVE);

    printf("frame_player move packs: %d edits and the player's refusal checked\n", EDITS + 1);
    free(flagged); free(arena); free(workspace); free(move); free(core);
    return 0;
}
