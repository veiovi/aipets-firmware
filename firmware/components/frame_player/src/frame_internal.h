#pragma once

/*
 * Shared internals of the frame_player component. The binary layout mirrors
 * the frame-pack compiler's TypeScript constants field for field; the
 * director state mirrors its TypeScript player.
 */

#include "frame_player.h"
#include "frame_codec.h"

#define FP_HEADER_BYTES 128u
#define FP_MAX_PALETTE 256u
#define FP_MAX_FRAMES 2048u
#define FP_MAX_CLIPS 256u
#define FP_MAX_CLIP_STEPS 256u
#define FP_MAX_EMOTIONS 32u
/* Expression masks are 16 bits: bit 0 is neutral, bits 1..15 are emotion ids. */
#define FP_MAX_EMOTION_ID 15u
#define FP_MAX_MICRO 16u
#define FP_MAX_TALK_STAGES 16u
/* A facial group carries seven mouth replacements, indexed by talk stage. */
#define FP_FACIAL_MOUTH_STAGES 7u
#define FP_MAX_ACTIONS 72u
#define FP_TALK_LUT_BYTES 101u

#define FP_FRAME_DIR_ENTRY_BYTES 12u
#define FP_CLIP_DIR_ENTRY_BYTES 16u
#define FP_CLIP_STEP_BYTES 4u
#define FP_EMOTION_ENTRY_BYTES 20u
#define FP_IDLE_PARAMS_BYTES 16u
#define FP_MICRO_ENTRY_BYTES 4u
#define FP_ACTION_ENTRY_BYTES 24u

#define FP_CODEC_INDEX8_FULL 0u
#define FP_CODEC_INDEX8_MASKED_RECT 1u
#define FP_CODEC_INDEX8_DENSE_RECT 2u
#define FP_CODEC_INDEX8_FULL_RLE 3u
#define FP_CODEC_INDEX8_MASKED_RECT_RLE 4u
#define FP_CODEC_INDEX8_FULL_ZLIB 5u
#define FP_CODEC_INDEX8_MASKED_RECT_ZLIB 6u
#define FP_CODEC_INDEX8_DENSE_RECT_ZLIB 7u

static inline int fp_full_codec(uint8_t c) { return c == 0u || c == 3u || c == 5u; }
static inline int fp_masked_codec(uint8_t c) { return c == 1u || c == 4u || c == 6u; }
static inline int fp_dense_codec(uint8_t c) { return c == 2u || c == 7u; }
static inline int fp_zlib_codec(uint8_t c) { return c >= 5u && c <= 7u; }

#define FP_FLAG_APPROVED 1u
#define FP_FEATURE_OVERLAY_ACTIONS 2u
#define FP_FEATURE_FACIAL_REGIONS 4u
#define FP_FEATURE_ACTION_LIFECYCLE 8u
#define FP_FEATURE_PERFORMANCE_ACTIONS 16u
#define FP_FEATURE_SPEAKING_POSES 32u
/* A move pack — see FP_FEATURE_MOVE in constants.ts. */
#define FP_FEATURE_MOVE 64u
#define FP_MOVE_MIN_TICKS 30u
#define FP_MOVE_MAX_TICKS 364u
#define FP_MOVE_MAX_FRAMES 64u
#define FP_MOVE_MAX_BYTES 300000u
#define FP_ACTION_FLAG_CENTER_ONLY 16u
#define FP_FACIAL_HEADER_BYTES 56u
#define FP_FACIAL_GROUP_BYTES 36u
#define FP_MAX_FACIAL_GROUPS 5u
#define FP_ACTION_FLAG_ESSENTIAL 1u
#define FP_ACTION_PROFILE_MASK 14u
#define FP_ACTION_NO_CLIP 0xffffu

#define FP_ACTIVATION_SEMANTIC 0u
#define FP_ACTIVATION_AMBIENT_LOOP 1u
#define FP_ACTIVATION_AMBIENT_EVENT 2u
#define FP_ACTIVATION_MANUAL 3u

#define FP_SEM_TOUCH 0u
#define FP_SEM_LISTENING 1u
#define FP_SEM_THINKING 2u
#define FP_SEM_SPEAKING 3u
#define FP_SEM_SHAKE 4u
#define FP_SEM_TILT_LEFT 5u
#define FP_SEM_TILT_RIGHT 6u
#define FP_SEM_TILT_UP 7u
#define FP_SEM_TILT_DOWN 8u
#define FP_SEM_SIGNATURE 9u
#define FP_SEM_BOOTING 10u
#define FP_SEM_PROVISIONING 11u
#define FP_SEM_CONNECTING 12u
#define FP_SEM_OFFLINE 13u
#define FP_SEM_ERROR 14u
#define FP_SEM_GESTURE_NOD 15u
#define FP_SEM_GESTURE_FOCUS 16u
#define FP_SEM_GESTURE_HEARTBEAT 17u
#define FP_SEM_GESTURE_BOUNCE 18u
#define FP_SEM_GESTURE_WOBBLE 19u
#define FP_SEM_GESTURE_POP 20u

#define FP_LOOP_ONCE 0u
#define FP_LOOP_LOOP 1u
#define FP_LOOP_HOLD_LAST 2u

/* Touch, shake and gesture pulses and ambient events play once per trigger.
 * Nothing else ends them, so their main clip plays once whatever its loop
 * mode (player.ts isTriggeredOneShot). */
static inline int fp_triggered_one_shot(uint8_t semantic, uint8_t activation)
{
    return activation == FP_ACTIVATION_AMBIENT_EVENT ||
           (activation == FP_ACTIVATION_SEMANTIC &&
            (semantic == FP_SEM_TOUCH || semantic == FP_SEM_SHAKE ||
             (semantic >= FP_SEM_GESTURE_NOD && semantic <= FP_SEM_GESTURE_POP)));
}

#define FP_ROLE_COUNT 8u
#define FP_ROLE_ABSENT 0xffffu
#define FP_ROLE_IDLE_NEUTRAL 0u
#define FP_ROLE_IDLE_BLINK 1u
#define FP_ROLE_BOOTING 2u
#define FP_ROLE_OFFLINE 3u
#define FP_ROLE_ERROR 4u
#define FP_ROLE_LISTENING_BASE 5u
#define FP_ROLE_THINKING_BASE 6u

/* Header field offsets (bytes) — see constants.ts. */
#define FP_HDR_MAGIC 0u
#define FP_HDR_VERSION 8u
#define FP_HDR_HEADER_BYTES 10u
#define FP_HDR_CANVAS_W 12u
#define FP_HDR_CANVAS_H 14u
#define FP_HDR_FILE_LENGTH 16u
#define FP_HDR_PAYLOAD_CRC32 20u
#define FP_HDR_HEADER_CRC32 24u
#define FP_HDR_FLAGS 28u
#define FP_HDR_TICK_MS 30u
#define FP_HDR_PALETTE_OFFSET 32u
#define FP_HDR_PALETTE_COUNT 36u
#define FP_HDR_BG_PALETTE_INDEX 38u
#define FP_HDR_GENDER 39u
#define FP_HDR_FRAME_DIR_OFFSET 40u
#define FP_HDR_FRAME_COUNT 44u
#define FP_HDR_CLIP_COUNT 46u
#define FP_HDR_CLIP_DIR_OFFSET 48u
#define FP_HDR_ROLES_OFFSET 52u
#define FP_HDR_EMOTIONS_OFFSET 56u
#define FP_HDR_EMOTION_COUNT 60u
#define FP_HDR_OVERLAY_COUNT 62u
#define FP_HDR_TALK_OFFSET 64u
#define FP_HDR_OVERLAY_DIR_OFFSET 68u
#define FP_HDR_IDLE_OFFSET 72u
#define FP_HDR_STRINGS_OFFSET 76u
#define FP_HDR_STRINGS_LENGTH 80u
#define FP_HDR_META_OFFSET 84u
#define FP_HDR_META_LENGTH 88u
#define FP_HDR_ID_STR_OFFSET 92u
#define FP_HDR_NAME_STR_OFFSET 96u
#define FP_HDR_VERSION_STR_OFFSET 100u
#define FP_HDR_ID_STR_LEN 104u
#define FP_HDR_NAME_STR_LEN 106u
#define FP_HDR_VERSION_STR_LEN 108u
#define FP_HDR_FACIAL_OFFSET 110u
#define FP_HDR_FACIAL_LENGTH 114u
#define FP_HDR_SPEAKING_OFFSET 118u /* u32 */
#define FP_HDR_SPEAKING_LENGTH 122u /* u16 */
/* bytes 124..127 reserved, must be zero */

/* Speaking-poses section — see constants.ts. */
#define FP_SPEAKING_SECTION_VERSION 1u
#define FP_SPEAKING_HEADER_BYTES 12u
#define FP_SPEAKING_GESTURE_BYTES 20u
#define FP_SPEAKING_GESTURE_KINDS 8u
#define FP_SPEAKING_MIN_POSES 2u
#define FP_SPEAKING_MAX_POSES 16u
#define FP_SPEAKING_MAX_GESTURES 8u
#define FP_SPEAKING_MIN_GESTURE_STEPS 2u
#define FP_SPEAKING_MAX_GESTURE_STEPS 8u
#define FP_SPEAKING_MAX_BREAK_TICKS 900u
#define FP_SPEAKING_DRIFT_PX 2
#define FP_SPEAKING_DRIFT_MIN_TICKS 90
#define FP_SPEAKING_DRIFT_MAX_TICKS 180

/* Director phases — mirrors player.ts Phase. */
typedef enum {
    FP_PHASE_SYS_STATIC = 0,
    FP_PHASE_IDLE = 1,
    FP_PHASE_MICRO = 2,
    FP_PHASE_EMO_ENTER = 3,
    FP_PHASE_EMO_HOLD = 4,
    FP_PHASE_EMO_EXIT = 5,
    FP_PHASE_TALK = 6,
    FP_PHASE_ATTEND = 7,
} fp_phase_t;

/* Director tuning — mirrors player.ts. */
#define FP_SETTLE_TICKS 30
#define FP_EMOTION_COOLDOWN_TICKS 18
#define FP_TALK_STAGE_MIN_DWELL_TICKS 2
#define FP_TALK_LOWER_STREAK_TICKS 2
#define FP_SHOWCASE_GAP_TICKS 15
/* Natural idle: ticks of calm before a manual action (about 4 to 10 minutes). */
#define FP_CALM_MIN_TICKS 7200
#define FP_CALM_MAX_TICKS 18000
#define FP_SHOWCASE_MIN_HOLD_MS 3000
#define FP_SHOWCASE_MAX_HOLD_MS 6000
#define FP_EMOTION_REQUEST_NONE (-1)

typedef struct {
    uint8_t id;
    uint16_t enter_clip;
    uint16_t hold_clip;
    uint16_t exit_clip;
    uint16_t min_hold_ms;
    uint16_t max_hold_ms;
} fp_emotion_entry_t;

typedef struct {
    uint16_t clip;
    uint16_t weight;
} fp_micro_entry_t;

typedef struct {
    uint8_t semantic;
    uint8_t plane;
    uint8_t activation;
    uint8_t priority;
    uint16_t clip;
    uint16_t exit_clip;
    uint16_t flags;
    uint16_t state_mask;
    uint16_t expression_mask;
    uint16_t interval_min_ticks;
    uint16_t interval_max_ticks;
    uint16_t cooldown_ticks;
    uint16_t enter_clip;
    uint16_t performance_mask;
} fp_action_entry_t;

typedef struct {
    int16_t action;
    uint16_t clip;
    uint16_t step;
    int32_t ticks_left;
    uint8_t entering;
    uint8_t exiting;
    uint8_t complete;
} fp_action_cursor_t;
typedef struct { uint8_t role; uint16_t base_frame; uint8_t mouth[4], eyes[4]; uint16_t mouth_frames[7], eye_frames[5]; } fp_facial_group_t;
typedef struct {
    uint8_t kind;
    uint8_t step_count;
    uint8_t pose[FP_SPEAKING_MAX_GESTURE_STEPS];
    uint8_t ticks[FP_SPEAKING_MAX_GESTURE_STEPS];
} fp_speaking_gesture_t;

/* Parsed pack view: small caches; frames/clips are read from the pack. */
typedef struct {
    uint16_t palette[FP_MAX_PALETTE];
    uint16_t palette_count;
    uint8_t bg_index;
    uint32_t frame_dir_offset;
    uint16_t frame_count;
    uint32_t clip_dir_offset;
    uint16_t clip_count;
    uint16_t roles[FP_ROLE_COUNT];
    fp_emotion_entry_t emotions[FP_MAX_EMOTIONS];
    uint8_t emotion_count;
    uint16_t talk_stage_frames[FP_MAX_TALK_STAGES];
    uint8_t talk_stage_count;
    uint32_t talk_lut_offset;
    uint16_t bob_amplitude_px;
    uint16_t bob_period_ticks;
    uint16_t micro_gap_min_ticks;
    uint16_t micro_gap_max_ticks;
    fp_micro_entry_t micro[FP_MAX_MICRO];
    uint8_t micro_count;
    uint16_t features;
    uint8_t fallbacks_enabled;
    uint8_t parallax_safe;
    uint8_t max_parallax_px;
    fp_action_entry_t actions[FP_MAX_ACTIONS];
    uint8_t action_count;
    uint8_t facial_group_count, facial_center;
    uint8_t facial_independent_blink;
    uint16_t facial_blink_state_mask, facial_pose_state_mask, facial_bob_state_mask;
    uint16_t facial_blink_gaps[6], facial_pose_timings[8], facial_bob_period;
    uint8_t facial_blink_ticks[5], facial_bob_full, facial_bob_balanced;
    fp_facial_group_t facial_groups[FP_MAX_FACIAL_GROUPS];
    uint8_t speaking_pose_count, speaking_gesture_count, speaking_auto_mask;
    uint16_t speaking_break_min, speaking_break_max;
    /* [pose * talk_stage_count + stage]; pose 0 is the talk table. */
    uint16_t speaking_frames[FP_SPEAKING_MAX_POSES * FP_MAX_TALK_STAGES];
    fp_speaking_gesture_t speaking_gestures[FP_SPEAKING_MAX_GESTURES];
} fp_pack_view_t;

struct fp_player {
    uint32_t magic;
    const uint8_t *pack;
    uint32_t pack_bytes;
    fp_pack_info_t info;
    fp_pack_view_t view;

    /* Latched inputs. */
    int32_t sys_state;
    int32_t requested_emotion; /* -1 none, 0 clear, else emotion id */
    int32_t audio_level;
    int32_t tap_pending;
    int32_t shake_pending;
    int32_t gesture_pending;
    int32_t tilt_x;
    int32_t tilt_y;
    int32_t animation_profile;
    int32_t idle_showcase_enabled; /* fp_idle_mode_t */
    int32_t optional_suppression_ticks;

    /* Director state — field-for-field mirror of player.ts. */
    int32_t phase;
    int32_t clip_index;
    int32_t clip_step;
    int32_t clip_ticks_left;
    int32_t current_emotion;
    int32_t pending_emotion;
    int32_t clear_requested;
    int32_t hold_min_ticks_left;
    int32_t hold_max_ticks_left;
    int32_t cooldown_emotion;
    int32_t cooldown_ticks_left;
    int32_t settle_ticks_left;
    int32_t micro_countdown;
    int32_t showcase_index;
    int32_t showcase_countdown;
    int32_t calm_countdown; /* -1 until calm begins in FP_IDLE_NATURAL */
    int32_t talk_stage;
    int32_t talk_dwell_ticks_left;
    int32_t talk_lower_streak;
    int32_t bob_tick;
    int32_t shown_sys_state; /* sys state the current SYS/ATTEND clip was chosen for */
    uint32_t prng_state;

    fp_action_cursor_t action_cursors[FP_PLANE_COUNT];
    int16_t requested_action;
    int32_t action_event_countdown[FP_MAX_ACTIONS];
    int32_t action_cooldown_ticks[FP_MAX_ACTIONS];
    uint8_t action_was_eligible[FP_MAX_ACTIONS];
    /* FP_IDLE_NATURAL: the step and ticks each interrupted ambient loop would
     * have shown next, where it resumes; 0 ticks when there is none. */
    uint16_t loop_resume_step[FP_MAX_ACTIONS];
    int32_t loop_resume_ticks[FP_MAX_ACTIONS];
    int32_t fallback_touch_ticks;
    int32_t fallback_shake_ticks;
    int32_t facial_group, facial_blink_stage, facial_blink_ticks_left, facial_blink_countdown;
    int32_t facial_pose_phase, facial_pose_side, facial_pose_ticks;
    int32_t facial_forced_bob;
    /* Speaking poses and drift — mirrors player.ts. */
    int32_t speaking_drift_enabled;
    int32_t speaking_pose, speaking_gesture, speaking_step, speaking_step_ticks;
    int32_t speaking_neutral_ticks, speaking_break_ticks;
    uint8_t speaking_bag[FP_SPEAKING_MAX_GESTURES];
    int32_t speaking_bag_count, speaking_last_gesture;
    int32_t drift_started, drift_x, drift_y, drift_from_x, drift_from_y;
    int32_t drift_to_x, drift_to_y, drift_elapsed, drift_duration;

    /* Presentation identity. The director still advances every 33 ms, but
     * identical authored visuals do not need another framebuffer expansion
     * or LVGL transformed-canvas invalidation. */
    uint32_t visual_revision;
    int32_t rendered_frame;
    int32_t rendered_offset_y;
    int32_t rendered_offset_x;
    int32_t render_initialized;
    fp_dirty_rect_t dirty_rect;
    /* The framebuffer holds exactly this base frame, drawn at this offset with
     * nothing over it (no facial patch, effect or fallback), so drawing the
     * same again would change no pixel. `plain_under`: the edges that the
     * offset exposes show the unshifted frame rather than the background. */
    int32_t plain_valid, plain_frame, plain_offset_x, plain_offset_y, plain_under;

    uint16_t framebuffer[FP_MAX_PIXELS];
    uint16_t scratch[FP_MAX_PIXELS];
    /* Caller-owned bounded decode state. The base survives facial/effect
     * scratch decodes, so a 4 fps movement is not inflated at the 33 ms tick. */
    _Alignas(8) fp_codec_workspace_t codec_workspace;
    int32_t decoded_base_frame;
    uint32_t base_decode_count;
    uint8_t decoded_base[FP_MAX_PIXELS];
};

uint16_t fp_rd16(const uint8_t *p);
uint32_t fp_rd32(const uint8_t *p);

typedef struct {
    uint16_t frame;
    uint16_t duration_ticks;
} fp_clip_step_t;

uint8_t fp_clip_loop_mode(const fp_player_t *player, uint32_t clip);
uint16_t fp_clip_step_count(const fp_player_t *player, uint32_t clip);
fp_clip_step_t fp_clip_step_at(const fp_player_t *player, uint32_t clip, uint32_t step);

void fp_render(fp_player_t *player);
void fp_actions_tick(fp_player_t *player);
void fp_actions_calm(fp_player_t *player);
uint8_t fp_actions_request(fp_player_t *player, int16_t action);
int32_t fp_actions_frame(const fp_player_t *player);
int32_t fp_actions_offset_x(const fp_player_t *player);
int32_t fp_actions_offset_y(const fp_player_t *player);
void fp_actions_composite(fp_player_t *player, uint16_t *target);
uint32_t fp_next_random(fp_player_t *player);
void fp_director_reset(fp_player_t *player);
void fp_director_tick(fp_player_t *player);
int32_t fp_director_frame(const fp_player_t *player);
int32_t fp_director_offset_y(const fp_player_t *player);
/* A speaking-poses pack is talking and no base action owns the frame. */
int fp_speaking_motion(const fp_player_t *player);
