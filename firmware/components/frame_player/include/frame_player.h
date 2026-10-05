#pragma once

/*
 * frame_player — the full-frame animation system.
 *
 * A complete, independent runtime for `.aipetframes` packs: banks of opaque
 * 120x120 or 240x240 palette-indexed frames on explicit tick timelines (clips),
 * with an internal director (state machine, idle scheduler, PRNG, talk
 * ladder). It is the sole supported character-pack runtime and shares no pack
 * format or runtime state with the procedural emergency renderer.
 *
 * Freestanding: no heap, no floats, no libc; state lives in a caller-provided
 * arena. The pack is read in place (flash mmap or rodata) byte-wise, so no
 * alignment is required. After fp_init() succeeds, fp_tick() cannot fail.
 *
 * The binary format and the director's behavior are specified by the
 * frame-pack compiler's TypeScript reference; the parity tests
 * assert per-tick framebuffer CRC equality between this C implementation and
 * the TypeScript reference.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FP_CANVAS_WIDTH = 120,
    FP_CANVAS_HEIGHT = 120,
    FP_PIXEL_COUNT = FP_CANVAS_WIDTH * FP_CANVAS_HEIGHT,
    FP_MAX_CANVAS = 240,
    FP_MAX_PIXELS = FP_MAX_CANVAS * FP_MAX_CANVAS,
    FP_TICK_MS = 33,
    FP_PACK_VERSION = 1,
    FP_PACK_VERSION_240 = 2,
    FP_PACK_MAX_BYTES_240 = 0xbc0000 - 0x2000,
    /* Hard ceiling: one V1 asset slot, so slot/OTA delivery stays possible. */
    FP_PACK_MAX_BYTES = 0x2a0000,
};

typedef enum {
    FP_OK = 0,
    FP_ERR_ARGUMENT,
    FP_ERR_BAD_MAGIC,
    FP_ERR_UNSUPPORTED_VERSION,
    FP_ERR_BAD_HEADER,
    FP_ERR_BAD_CHECKSUM,
    FP_ERR_BAD_BOUNDS,
    FP_ERR_BAD_FRAME,
    FP_ERR_BAD_CLIP,
    FP_ERR_BAD_ROLE,
    FP_ERR_BAD_EMOTION,
    FP_ERR_BAD_TALK,
    FP_ERR_BAD_IDLE,
    FP_ERR_BAD_OVERLAY,
    FP_ERR_BAD_FACIAL,
    FP_ERR_TRUNCATED,
    FP_ERR_ARENA,
    FP_ERR_BAD_SPEAKING,
    FP_ERR_BAD_MOVE,
} fp_error_t;

/* Mirrors pet_face_state_t ordinals exactly (firmware/main/pet_face.h). */
typedef enum {
    FP_SYS_BOOTING = 0,
    FP_SYS_PROVISIONING,
    FP_SYS_CONNECTING,
    FP_SYS_IDLE,
    FP_SYS_LISTENING,
    FP_SYS_THINKING,
    FP_SYS_SPEAKING,
    FP_SYS_OFFLINE,
    FP_SYS_ERROR,
} fp_sys_state_t;

/*
 * Stable pack emotion ids. 0 clears back to neutral; a pack may implement any
 * subset — requests for ids a pack lacks are ignored.
 */
typedef enum {
    FP_EMO_NEUTRAL = 0,
    FP_EMO_ANGRY = 1,
    FP_EMO_CONFUSED = 2,
    FP_EMO_DETERMINED = 3,
    FP_EMO_DISGUST = 4,
    FP_EMO_EMBARRASSED = 5,
    FP_EMO_FEAR = 6,
    FP_EMO_JOY = 7,
    FP_EMO_SAD = 8,
    FP_EMO_SLEEPY = 9,
    FP_EMO_SURPRISED = 10,
    FP_EMO_CURIOUS = 11,
    FP_EMO_CONCERNED = 12,
    FP_EMO_EXCITED = 13,
    FP_EMO_SHY = 14,
    FP_EMO_COUNT,
} fp_emotion_t;

typedef enum {
    FP_PROFILE_FULL = 0,
    FP_PROFILE_BALANCED = 1,
    FP_PROFILE_REDUCED = 2,
} fp_animation_profile_t;

typedef enum {
    FP_GESTURE_NONE = 0,
    FP_GESTURE_NOD,
    FP_GESTURE_FOCUS,
    FP_GESTURE_HEARTBEAT,
    FP_GESTURE_BOUNCE,
    FP_GESTURE_WOBBLE,
    FP_GESTURE_POP,
} fp_gesture_t;

typedef enum {
    FP_PLANE_BASE = 0,
    FP_PLANE_CHARACTER_FX,
    FP_PLANE_STATE_FX,
    FP_PLANE_GESTURE_FX,
    FP_PLANE_TOUCH_FX,
    FP_PLANE_COUNT,
} fp_plane_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
} fp_dirty_rect_t;

typedef struct {
    uint8_t overlay_actions;
    uint8_t parallax_safe;
    uint8_t max_parallax_px;
    uint8_t action_count;
    uint32_t semantic_mask;
    uint8_t facial_regions;
    uint8_t facial_group_count;
    uint8_t speaking_poses;
    uint8_t speaking_gesture_count;
} fp_motion_capabilities_t;

typedef struct {
    const uint8_t *id;
    uint16_t id_len;
    const uint8_t *name;
    uint16_t name_len;
    const uint8_t *version;
    uint16_t version_len;
    uint16_t frame_count;
    uint16_t clip_count;
    uint8_t emotion_count;
    uint8_t gender; /* same encoding as V1 packs */
    uint8_t approved;
    uint16_t palette_count;
    uint32_t payload_crc32;
    uint32_t pack_bytes;
    uint16_t format_version;
    uint16_t width;
    uint16_t height;
    uint8_t codecs; /* Exact mask of resource encodings, after full validation. */
    uint8_t move; /* A move pack: it plays beside its core pack; no player binds one. */
} fp_pack_info_t;

typedef struct fp_player fp_player_t;

/* Legacy allocation-free raw/RLE validation. Compressed resources return
 * FP_ERR_ARENA: use the explicit workspace API for all v2 release validation. */
fp_error_t fp_validate(const void *pack, uint32_t pack_bytes, fp_pack_info_t *info);
uint32_t fp_validation_workspace_size(void);
/* Workspace must be 8-byte aligned, exclusively owned for this call, and
 * allocated off the firmware task stack. `info` may be NULL. */
fp_error_t fp_validate_with_workspace(const void *pack, uint32_t pack_bytes,
    fp_pack_info_t *info, void *workspace, uint32_t workspace_bytes);

/* Changes whenever fp_validate_with_workspace accepts or rejects differently.
 * An installed pack records the revision that proved it; firmware with a newer
 * validator checks that pack in full once more before showing it. */
#define FP_VALIDATOR_REVISION 3u

/* Arena bytes required by fp_init (compile-time constant). */
uint32_t fp_arena_size(void);

/*
 * Bind a player to a validated pack. `seed` feeds the idle PRNG: fixed seeds
 * give reproducible tick sequences (tests/emulator), esp_random() gives
 * variety across boots. Seed 0 is remapped to a constant internally. A move
 * pack has no roles to direct: binding one fails with FP_ERR_ARGUMENT.
 */
fp_player_t *fp_player_init(void *arena, uint32_t arena_bytes, const void *pack,
                            uint32_t pack_bytes, uint32_t seed, fp_error_t *error);
/*
 * Bind a player to bytes that fp_validate_with_workspace already accepted in
 * full, for example an installed pack whose SHA-256 still equals the bytes
 * that were validated. Structure and uncompressed frames are checked again;
 * the payload CRC is not, and no compressed frame is inflated, so binding a
 * 3 MB pack takes milliseconds instead of seconds. Given bytes that would
 * fail full validation, the player stays memory-safe but may render garbage.
 */
fp_player_t *fp_player_init_prevalidated(void *arena, uint32_t arena_bytes, const void *pack,
                                         uint32_t pack_bytes, uint32_t seed, fp_error_t *error);
/* The same structure-only check without binding, to read `info` (identity,
 * approval, size) of bytes already validated in full. Needs no workspace. */
fp_error_t fp_inspect_prevalidated(const void *pack, uint32_t pack_bytes, fp_pack_info_t *info);

void fp_set_sys_state(fp_player_t *player, fp_sys_state_t state);
void fp_set_emotion(fp_player_t *player, fp_emotion_t emotion);
void fp_set_audio_level(fp_player_t *player, uint8_t level_0_100);
void fp_set_animation_profile(fp_player_t *player, fp_animation_profile_t profile);
/* Pause the automatic idle emotion/performance director while retaining the
 * neutral idle frame and directly triggered semantic actions. Normal players
 * start with the idle showcase enabled. Same as fp_set_idle_mode with
 * FP_IDLE_SHOWCASE (1) or FP_IDLE_HOLD (0). */
void fp_set_idle_showcase_enabled(fp_player_t *player, uint8_t enabled);
/* How the pet idles when nothing else happens:
 * - FP_IDLE_SHOWCASE, the default: every emotion and performance in turn, the
 *   art-review loop;
 * - FP_IDLE_HOLD: the neutral idle, while direct semantic actions are reviewed;
 * - FP_IDLE_NATURAL: a companion's idle. The pack's micro performances play at
 *   its authored gaps. After FP_CALM_MIN..MAX ticks of calm (about 4 to 10
 *   minutes at 33 ms), one of its manual actions plays, such as a signature
 *   move. Calm is idle with no input, emotion or other action. An ambient loop
 *   that an action or a state interrupts resumes where it stopped, so a long
 *   idle loop plays every moment; the reduced profile, another idle mode or a
 *   new binding starts it over.
 * Setting the current mode again changes nothing. */
typedef enum { FP_IDLE_HOLD = 0, FP_IDLE_SHOWCASE = 1, FP_IDLE_NATURAL = 2 } fp_idle_mode_t;
void fp_set_idle_mode(fp_player_t *player, fp_idle_mode_t mode);
void fp_set_tilt(fp_player_t *player, int8_t screen_x, int8_t screen_y);
/* Subtle speaking movement, the owner's setting (off by default): while a pack
 * with speaking poses talks, the whole character drifts by up to 2px. */
void fp_set_speaking_drift(fp_player_t *player, uint8_t enabled);
void fp_notify_shake(fp_player_t *player);
void fp_notify_gesture(fp_player_t *player, fp_gesture_t gesture);
void fp_notify_touch(fp_player_t *player);
void fp_notify_tap(fp_player_t *player);
/* Explicitly select a v2 manual base action by its signed-release mapping.
 * Returns 0 for unavailable/context-incompatible actions. -1 cancels. A state
 * change, competing action or completion cancels the request; no stale replay. */
uint8_t fp_request_action(fp_player_t *player, int16_t action);
void fp_report_health(fp_player_t *player, uint8_t audio_healthy, uint16_t presentation_ms);

/* Advance exactly one 33 ms director tick. The framebuffer is refreshed only
 * when the visible frame or whole-frame offset changes. */
void fp_tick(fp_player_t *player);

/* Validated pack-sized RGB565 (native endian), valid until the next fp_tick. */
const uint16_t *fp_framebuffer(const fp_player_t *player);
uint16_t fp_canvas_width(const fp_player_t *player);
uint16_t fp_canvas_height(const fp_player_t *player);
uint32_t fp_frame_crc32(const fp_player_t *player);
uint32_t fp_debug_base_decode_count(const fp_player_t *player);
/* Monotonic counter incremented only when the visible frame or whole-frame
 * offset changes. Adapters can avoid invalidating an unchanged canvas. */
uint32_t fp_visual_revision(const fp_player_t *player);
fp_dirty_rect_t fp_dirty_rect(const fp_player_t *player);
int16_t fp_debug_dirty_x(const fp_player_t *player);
int16_t fp_debug_dirty_y(const fp_player_t *player);
uint16_t fp_debug_dirty_width(const fp_player_t *player);
uint16_t fp_debug_dirty_height(const fp_player_t *player);
int16_t fp_active_action(const fp_player_t *player, fp_plane_t plane);
fp_motion_capabilities_t fp_motion_capabilities(const fp_player_t *player);
const fp_pack_info_t *fp_pack_info(const fp_player_t *player);
const char *fp_error_string(fp_error_t error);

/* Pack emotion id at table index [0, emotion_count); 0 if out of range.
 * Lets callers enumerate emotions without assuming dense ids. */
uint8_t fp_emotion_id_at(const fp_player_t *player, uint8_t index);

/* Introspection for tests and the emulator HUD. Offsets are the values
 * actually applied to the rendered frame, including registered-face bob. */
uint8_t fp_debug_phase(const fp_player_t *player);
uint16_t fp_debug_frame(const fp_player_t *player);
int8_t fp_debug_offset_y(const fp_player_t *player);
int8_t fp_debug_offset_x(const fp_player_t *player);
int8_t fp_debug_facial_group(const fp_player_t *player);
uint8_t fp_debug_mouth_stage(const fp_player_t *player);
uint8_t fp_debug_blink_stage(const fp_player_t *player);
/* Speaking pose and gesture while talking; 0 and -1 otherwise. */
uint8_t fp_debug_speaking_pose(const fp_player_t *player);
int8_t fp_debug_speaking_gesture(const fp_player_t *player);
void fp_debug_compose_facial(fp_player_t *player, uint8_t group, uint8_t mouth, uint8_t blink, uint8_t bob);

#ifdef __cplusplus
}
#endif
