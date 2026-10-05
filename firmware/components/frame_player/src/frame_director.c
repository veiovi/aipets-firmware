/*
 * The animation director: phases, idle scheduler, emotion graph, talk ladder.
 *
 * This is a statement-for-statement mirror of the TypeScript reference
 * player of the frame-pack compiler. Any behavioral change MUST land in both
 * files together; the parity tests hold the two to per-tick framebuffer CRC
 * equality across scenario tapes.
 */

#include "frame_internal.h"

/* round(ms / tick) without floats; exact for all integer ms (tick is odd). */
static int32_t fp_ms_to_ticks(int32_t ms, int32_t tick_ms)
{
    int32_t ticks = (2 * ms + tick_ms) / (2 * tick_ms);
    return ticks < 1 ? 1 : ticks;
}

uint32_t fp_next_random(fp_player_t *p)
{
    uint32_t x = p->prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p->prng_state = x;
    return x;
}

static void fp_draw_micro_countdown(fp_player_t *p)
{
    if (p->view.micro_count == 0u) {
        p->micro_countdown = -1;
        return;
    }
    uint32_t span = (uint32_t)(p->view.micro_gap_max_ticks - p->view.micro_gap_min_ticks + 1);
    p->micro_countdown = (int32_t)(p->view.micro_gap_min_ticks + (fp_next_random(p) % span));
}

static int32_t fp_draw_micro_clip(fp_player_t *p)
{
    /* A pack without idle micro clips keeps the countdown disabled, so this
     * helper is not normally reached. Keep the boundary defensive anyway:
     * corrupted director state must fall back to neutral instead of
     * underflowing micro_count and indexing before the fixed arena. */
    if (p->view.micro_count == 0u) {
        return (int32_t)p->view.roles[FP_ROLE_IDLE_NEUTRAL];
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < p->view.micro_count; i++) total += p->view.micro[i].weight;
    uint32_t r = fp_next_random(p) % total;
    for (uint32_t i = 0; i < p->view.micro_count; i++) {
        if (r < p->view.micro[i].weight) return (int32_t)p->view.micro[i].clip;
        r -= p->view.micro[i].weight;
    }
    return (int32_t)p->view.micro[p->view.micro_count - 1u].clip;
}

/* ---- Clip cursor ------------------------------------------------------- */

static void fp_start_clip(fp_player_t *p, int32_t clip_index)
{
    p->clip_index = clip_index;
    p->clip_step = 0;
    p->clip_ticks_left = (int32_t)fp_clip_step_at(p, (uint32_t)clip_index, 0).duration_ticks;
}

/* Advance one tick; returns 1 when a ONCE clip has finished. */
static int fp_advance_clip(fp_player_t *p)
{
    uint32_t clip = (uint32_t)p->clip_index;
    p->clip_ticks_left--;
    if (p->clip_ticks_left > 0) return 0;
    uint16_t step_count = fp_clip_step_count(p, clip);
    if ((uint32_t)(p->clip_step + 1) < step_count) {
        p->clip_step++;
        p->clip_ticks_left = (int32_t)fp_clip_step_at(p, clip, (uint32_t)p->clip_step).duration_ticks;
        return 0;
    }
    switch (fp_clip_loop_mode(p, clip)) {
    case FP_LOOP_LOOP:
        p->clip_step = 0;
        p->clip_ticks_left = (int32_t)fp_clip_step_at(p, clip, 0).duration_ticks;
        return 0;
    case FP_LOOP_HOLD_LAST:
        p->clip_ticks_left = (int32_t)fp_clip_step_at(p, clip, (uint32_t)p->clip_step).duration_ticks;
        return 0;
    default:
        /* FP_LOOP_ONCE: freeze on the last step; the director moves on. */
        p->clip_ticks_left = 0;
        return 1;
    }
}

/* ---- Role helpers ------------------------------------------------------ */

static int32_t fp_role_clip_or_neutral(const fp_player_t *p, uint32_t role)
{
    uint16_t clip = p->view.roles[role];
    return clip == FP_ROLE_ABSENT ? (int32_t)p->view.roles[FP_ROLE_IDLE_NEUTRAL] : (int32_t)clip;
}

static int32_t fp_sys_role_clip(const fp_player_t *p)
{
    switch (p->sys_state) {
    case FP_SYS_OFFLINE: return fp_role_clip_or_neutral(p, FP_ROLE_OFFLINE);
    case FP_SYS_ERROR: return fp_role_clip_or_neutral(p, FP_ROLE_ERROR);
    default: return fp_role_clip_or_neutral(p, FP_ROLE_BOOTING);
    }
}

static int32_t fp_attend_role_clip(const fp_player_t *p)
{
    return p->sys_state == FP_SYS_THINKING ? fp_role_clip_or_neutral(p, FP_ROLE_THINKING_BASE)
                                           : fp_role_clip_or_neutral(p, FP_ROLE_LISTENING_BASE);
}

static int32_t fp_find_emotion(const fp_player_t *p, int32_t id)
{
    for (uint32_t i = 0; i < p->view.emotion_count; i++) {
        if ((int32_t)p->view.emotions[i].id == id) return (int32_t)i;
    }
    return -1;
}

/* ---- Phase entries ----------------------------------------------------- */

static void fp_enter_sys_static(fp_player_t *p)
{
    p->phase = FP_PHASE_SYS_STATIC;
    p->current_emotion = 0;
    p->clear_requested = 0;
    p->shown_sys_state = p->sys_state;
    fp_start_clip(p, fp_sys_role_clip(p));
}

static void fp_enter_idle(fp_player_t *p, int with_settle)
{
    p->phase = FP_PHASE_IDLE;
    p->current_emotion = 0;
    p->clear_requested = 0;
    p->settle_ticks_left = with_settle ? FP_SETTLE_TICKS : 0;
    fp_start_clip(p, (int32_t)p->view.roles[FP_ROLE_IDLE_NEUTRAL]);
    fp_draw_micro_countdown(p);
    p->showcase_countdown = FP_SHOWCASE_GAP_TICKS;
}

static void fp_enter_attend(fp_player_t *p)
{
    p->phase = FP_PHASE_ATTEND;
    p->current_emotion = 0;
    p->clear_requested = 0;
    p->shown_sys_state = p->sys_state;
    fp_start_clip(p, fp_attend_role_clip(p));
}

/* ---- Speaking poses (mirrors player.ts) --------------------------------- */

static int32_t fp_draw_speaking_break(fp_player_t *p)
{
    uint32_t span = (uint32_t)(p->view.speaking_break_max - p->view.speaking_break_min + 1u);
    return (int32_t)(p->view.speaking_break_min + fp_next_random(p) % span);
}

/* Shuffled rotation of the automatic gestures, never the same one twice in a row. */
static int32_t fp_choose_speaking_gesture(fp_player_t *p)
{
    uint8_t *bag = p->speaking_bag;
    if (p->speaking_bag_count == 0) {
        for (uint32_t g = 0; g < p->view.speaking_gesture_count; g++) {
            if ((p->view.speaking_auto_mask >> g) & 1u) bag[p->speaking_bag_count++] = (uint8_t)g;
        }
        for (int32_t i = p->speaking_bag_count - 1; i > 0; i--) {
            uint32_t j = fp_next_random(p) % (uint32_t)(i + 1);
            uint8_t swap = bag[i];
            bag[i] = bag[j];
            bag[j] = swap;
        }
        int32_t last = p->speaking_bag_count - 1;
        if (last > 0 && bag[last] == p->speaking_last_gesture) {
            bag[last] = bag[0];
            bag[0] = (uint8_t)p->speaking_last_gesture;
        }
    }
    int32_t gesture = bag[--p->speaking_bag_count];
    p->speaking_last_gesture = gesture;
    return gesture;
}

/* Gestures start only while speech is audible, after a full neutral break. */
static void fp_tick_speaking_gesture(fp_player_t *p, int32_t raw)
{
    if (p->animation_profile == FP_PROFILE_REDUCED) {
        if (p->speaking_gesture >= 0) {
            p->speaking_gesture = -1;
            p->speaking_pose = 0;
            p->speaking_neutral_ticks = 0;
        }
        return;
    }
    if (p->speaking_gesture >= 0) {
        if (--p->speaking_step_ticks > 0) return;
        const fp_speaking_gesture_t *gesture = &p->view.speaking_gestures[p->speaking_gesture];
        if (++p->speaking_step >= (int32_t)gesture->step_count) {
            p->speaking_gesture = -1;
            p->speaking_pose = 0;
            p->speaking_neutral_ticks = 0;
            p->speaking_break_ticks = fp_draw_speaking_break(p);
            return;
        }
        p->speaking_pose = gesture->pose[p->speaking_step];
        p->speaking_step_ticks = gesture->ticks[p->speaking_step];
        return;
    }
    if (p->speaking_neutral_ticks < p->speaking_break_ticks) p->speaking_neutral_ticks++;
    if (p->speaking_neutral_ticks < p->speaking_break_ticks || raw == 0) return;
    int32_t index = fp_choose_speaking_gesture(p);
    const fp_speaking_gesture_t *gesture = &p->view.speaking_gestures[index];
    p->speaking_gesture = index;
    p->speaking_step = 0;
    p->speaking_pose = gesture->pose[0];
    p->speaking_step_ticks = gesture->ticks[0];
    p->speaking_neutral_ticks = 0;
}

static void fp_pick_drift_target(fp_player_t *p)
{
    p->drift_from_x = p->drift_x;
    p->drift_from_y = p->drift_y;
    uint32_t span = FP_SPEAKING_DRIFT_PX * 2u + 1u;
    int32_t to_x = (int32_t)(fp_next_random(p) % span) - FP_SPEAKING_DRIFT_PX;
    int32_t to_y = (int32_t)(fp_next_random(p) % span) - FP_SPEAKING_DRIFT_PX;
    if (to_x == p->drift_from_x && to_y == p->drift_from_y) to_x = to_x == FP_SPEAKING_DRIFT_PX ? to_x - 1 : to_x + 1;
    p->drift_to_x = to_x;
    p->drift_to_y = to_y;
    p->drift_duration = FP_SPEAKING_DRIFT_MIN_TICKS +
        (int32_t)(fp_next_random(p) % (uint32_t)(FP_SPEAKING_DRIFT_MAX_TICKS - FP_SPEAKING_DRIFT_MIN_TICKS + 1));
    p->drift_elapsed = 0;
}

/* round(q / 1024) for q in [-4096, 4096], without a negative division. */
static int32_t fp_round_q10(int32_t q) { return (q + 512 + 8192) / 1024 - 8; }

/* Whole-pixel smoothstep toward a new target every 3-6 s. */
static void fp_tick_drift(fp_player_t *p, int held)
{
    if (!p->speaking_drift_enabled || p->animation_profile == FP_PROFILE_REDUCED || held) {
        p->drift_started = 0;
        p->drift_x = 0;
        p->drift_y = 0;
        return;
    }
    if (!p->drift_started) {
        p->drift_started = 1;
        fp_pick_drift_target(p);
    }
    p->drift_elapsed++;
    uint32_t t = (uint32_t)p->drift_elapsed * 1024u / (uint32_t)p->drift_duration; /* Q10, 0..1024 */
    int32_t ease = (int32_t)((t * t * (3072u - 2u * t)) >> 20); /* t^2 (3 - 2t), Q10 */
    p->drift_x = fp_round_q10(p->drift_from_x * 1024 + (p->drift_to_x - p->drift_from_x) * ease);
    p->drift_y = fp_round_q10(p->drift_from_y * 1024 + (p->drift_to_y - p->drift_from_y) * ease);
    if (p->drift_elapsed >= p->drift_duration) fp_pick_drift_target(p);
}

int fp_speaking_motion(const fp_player_t *p)
{
    return p->view.speaking_pose_count > 0u && p->phase == FP_PHASE_TALK && p->action_cursors[FP_PLANE_BASE].action < 0;
}

static void fp_enter_talk(fp_player_t *p)
{
    p->phase = FP_PHASE_TALK;
    p->current_emotion = 0;
    p->clear_requested = 0;
    p->talk_stage = (int32_t)p->pack[p->view.talk_lut_offset + (uint32_t)p->audio_level];
    p->talk_dwell_ticks_left = FP_TALK_STAGE_MIN_DWELL_TICKS;
    p->talk_lower_streak = 0;
    if (p->view.speaking_pose_count > 0u) {
        p->speaking_gesture = -1;
        p->speaking_pose = 0;
        p->speaking_step = 0;
        p->speaking_step_ticks = 0;
        p->speaking_neutral_ticks = 0;
        p->speaking_break_ticks = fp_draw_speaking_break(p);
        p->drift_started = 0;
        p->drift_x = 0;
        p->drift_y = 0;
    }
}

static void fp_enter_emotion(fp_player_t *p, int32_t id)
{
    const fp_emotion_entry_t *emotion = &p->view.emotions[fp_find_emotion(p, id)];
    p->phase = FP_PHASE_EMO_ENTER;
    p->current_emotion = id;
    p->clear_requested = 0;
    p->hold_min_ticks_left = fp_ms_to_ticks((int32_t)emotion->min_hold_ms, FP_TICK_MS);
    int32_t max_ticks = fp_ms_to_ticks((int32_t)emotion->max_hold_ms, FP_TICK_MS);
    p->hold_max_ticks_left = max_ticks < p->hold_min_ticks_left ? p->hold_min_ticks_left : max_ticks;
    fp_start_clip(p, (int32_t)emotion->enter_clip);
}

static void fp_enter_showcase_emotion(fp_player_t *p, int32_t id)
{
    fp_enter_emotion(p, id);
    uint32_t span = (uint32_t)(FP_SHOWCASE_MAX_HOLD_MS - FP_SHOWCASE_MIN_HOLD_MS + 1);
    int32_t hold_ms = FP_SHOWCASE_MIN_HOLD_MS + (int32_t)(fp_next_random(p) % span);
    int32_t hold_ticks = fp_ms_to_ticks(hold_ms, FP_TICK_MS);
    /* The showcase is intentionally an art-review loop: each complete full
     * emotion remains visible for a real 3–6 seconds before its authored exit. */
    p->hold_min_ticks_left = hold_ticks;
    p->hold_max_ticks_left = hold_ticks;
}

static void fp_begin_emotion_exit(fp_player_t *p)
{
    const fp_emotion_entry_t *emotion = &p->view.emotions[fp_find_emotion(p, p->current_emotion)];
    p->phase = FP_PHASE_EMO_EXIT;
    fp_start_clip(p, (int32_t)emotion->exit_clip);
}

static void fp_finish_emotion_exit(fp_player_t *p)
{
    p->cooldown_emotion = p->current_emotion;
    p->cooldown_ticks_left = FP_EMOTION_COOLDOWN_TICKS;
    int32_t pending = p->pending_emotion;
    p->pending_emotion = 0;
    if (pending != 0 && pending != p->cooldown_emotion && fp_find_emotion(p, pending) >= 0) {
        fp_enter_emotion(p, pending);
    } else {
        fp_enter_idle(p, 0);
    }
}

/* ---- Input intake ------------------------------------------------------ */

static void fp_consume_emotion_request(fp_player_t *p)
{
    int32_t request = p->requested_emotion;
    if (request == FP_EMOTION_REQUEST_NONE) return;
    p->requested_emotion = FP_EMOTION_REQUEST_NONE;
    if (request == 0) {
        p->pending_emotion = 0;
        if (p->phase == FP_PHASE_EMO_ENTER || p->phase == FP_PHASE_EMO_HOLD) p->clear_requested = 1;
        return;
    }
    if (fp_find_emotion(p, request) < 0) {
        /* A release may omit this expression. Do not strand the previous pose. */
        p->pending_emotion = 0;
        if (p->phase == FP_PHASE_EMO_ENTER || p->phase == FP_PHASE_EMO_HOLD) p->clear_requested = 1;
        return;
    }
    switch (p->phase) {
    case FP_PHASE_IDLE:
    case FP_PHASE_MICRO:
        if (request == p->cooldown_emotion && p->cooldown_ticks_left > 0) return;
        p->pending_emotion = request;
        break;
    case FP_PHASE_EMO_ENTER:
    case FP_PHASE_EMO_HOLD:
        if (request == p->current_emotion) {
            const fp_emotion_entry_t *emotion = &p->view.emotions[fp_find_emotion(p, request)];
            int32_t refreshed = fp_ms_to_ticks((int32_t)emotion->max_hold_ms, FP_TICK_MS);
            if (refreshed > p->hold_max_ticks_left) p->hold_max_ticks_left = refreshed;
            p->clear_requested = 0;
        } else {
            p->pending_emotion = request;
        }
        break;
    default:
        /* EMO_EXIT, TALK, ATTEND, SYS_STATIC */
        p->pending_emotion = request;
        break;
    }
}

/* ---- Sub-ticks --------------------------------------------------------- */

static void fp_tick_idle(fp_player_t *p)
{
    if (p->settle_ticks_left > 0) {
        p->settle_ticks_left--;
        fp_advance_clip(p);
        return;
    }
    if (p->pending_emotion != 0) {
        int32_t pending = p->pending_emotion;
        p->pending_emotion = 0;
        fp_enter_emotion(p, pending);
        return;
    }
    uint32_t emotion_count = p->view.emotion_count;
    uint32_t performance_count = p->view.micro_count;
    uint32_t showcase_count = emotion_count + performance_count;
    if (showcase_count > 0u && p->idle_showcase_enabled == FP_IDLE_SHOWCASE) {
        if (p->showcase_countdown > 0) p->showcase_countdown--;
        if (p->showcase_countdown == 0) {
            uint32_t slot = (uint32_t)p->showcase_index % showcase_count;
            uint32_t paired = emotion_count < performance_count ? emotion_count : performance_count;
            paired *= 2u;
            int use_emotion;
            uint32_t index;
            if (slot < paired) {
                use_emotion = (slot & 1u) == 0u;
                index = slot / 2u;
            } else {
                uint32_t tail = slot - paired;
                use_emotion = emotion_count > performance_count;
                index = (paired / 2u) + tail;
            }
            p->showcase_index = (int32_t)((slot + 1u) % showcase_count);
            if (use_emotion) {
                fp_enter_showcase_emotion(p, (int32_t)p->view.emotions[index].id);
            } else {
                p->phase = FP_PHASE_MICRO;
                fp_start_clip(p, (int32_t)p->view.micro[index].clip);
            }
            return;
        }
        fp_advance_clip(p);
        return;
    }
    if (p->micro_countdown > 0) {
        p->micro_countdown--;
        if (p->micro_countdown == 0) {
            p->phase = FP_PHASE_MICRO;
            fp_start_clip(p, fp_draw_micro_clip(p));
            return;
        }
    }
    fp_advance_clip(p);
}

static void fp_tick_emotion_hold(fp_player_t *p)
{
    if (p->hold_min_ticks_left > 0) p->hold_min_ticks_left--;
    if (p->hold_max_ticks_left > 0) p->hold_max_ticks_left--;
    int wants_out = p->clear_requested || p->pending_emotion != 0 || p->hold_max_ticks_left == 0;
    if (wants_out && p->hold_min_ticks_left == 0) {
        fp_begin_emotion_exit(p);
        return;
    }
    fp_advance_clip(p);
}

static void fp_tick_talk(fp_player_t *p)
{
    int32_t raw = (int32_t)p->pack[p->view.talk_lut_offset + (uint32_t)p->audio_level];
    if (p->talk_dwell_ticks_left > 0) p->talk_dwell_ticks_left--;
    if (raw > p->talk_stage) {
        p->talk_stage = raw;
        p->talk_dwell_ticks_left = FP_TALK_STAGE_MIN_DWELL_TICKS;
        p->talk_lower_streak = 0;
    } else if (raw < p->talk_stage) {
        p->talk_lower_streak++;
        if (p->talk_dwell_ticks_left == 0 && p->talk_lower_streak >= FP_TALK_LOWER_STREAK_TICKS) {
            p->talk_stage = raw;
            p->talk_dwell_ticks_left = FP_TALK_STAGE_MIN_DWELL_TICKS;
            p->talk_lower_streak = 0;
        }
    } else {
        p->talk_lower_streak = 0;
    }
    if (p->view.speaking_pose_count > 0u) {
        /* A base action owns the frame; the speaking gestures wait for it. */
        int held = p->action_cursors[FP_PLANE_BASE].action >= 0;
        if (!held) fp_tick_speaking_gesture(p, raw);
        fp_tick_drift(p, held);
    }
}

/* ---- Frame + offset queries ------------------------------------------- */

int32_t fp_director_frame(const fp_player_t *p)
{
    if (p->phase == FP_PHASE_TALK) {
        if (p->view.speaking_pose_count > 0u) {
            return (int32_t)p->view.speaking_frames[(uint32_t)p->speaking_pose * p->view.talk_stage_count + (uint32_t)p->talk_stage];
        }
        return (int32_t)p->view.talk_stage_frames[p->talk_stage];
    }
    return (int32_t)fp_clip_step_at(p, (uint32_t)p->clip_index, (uint32_t)p->clip_step).frame;
}

int32_t fp_director_offset_y(const fp_player_t *p)
{
    uint16_t amplitude = p->view.bob_amplitude_px;
    uint16_t period = p->view.bob_period_ticks;
    if (amplitude == 0u || period == 0u) return 0;
    /* An authored bob continues while the pet talks: the gentle float that
     * made the original Luna look alive in conversation. */
    if (p->phase != FP_PHASE_IDLE && p->phase != FP_PHASE_EMO_HOLD && p->phase != FP_PHASE_ATTEND &&
        p->phase != FP_PHASE_TALK) return 0;
    int32_t phase = p->bob_tick % (int32_t)period;
    return phase * 2 >= (int32_t)period ? (int32_t)amplitude : 0;
}

/* ---- Main entry points ------------------------------------------------- */

void fp_director_reset(fp_player_t *p)
{
    fp_enter_sys_static(p);
    fp_render(p);
}

void fp_director_tick(fp_player_t *p)
{
    p->bob_tick++;
    /* Wrap at the bob period (identical in the TS reference) so the counter
     * can never overflow int32; offset math only uses bob_tick % period. */
    if (p->view.bob_period_ticks > 0u && p->bob_tick >= (int32_t)p->view.bob_period_ticks) {
        p->bob_tick = 0;
    }
    if (p->cooldown_ticks_left > 0) p->cooldown_ticks_left--;
    fp_consume_emotion_request(p);
    fp_actions_calm(p);

    int wants_sys = p->sys_state == FP_SYS_BOOTING || p->sys_state == FP_SYS_PROVISIONING ||
                    p->sys_state == FP_SYS_CONNECTING || p->sys_state == FP_SYS_OFFLINE ||
                    p->sys_state == FP_SYS_ERROR;
    int wants_talk = p->sys_state == FP_SYS_SPEAKING;
    int wants_attend = p->sys_state == FP_SYS_LISTENING || p->sys_state == FP_SYS_THINKING;

    if (wants_sys) {
        if (p->phase != FP_PHASE_SYS_STATIC || p->shown_sys_state != p->sys_state) fp_enter_sys_static(p);
        else fp_advance_clip(p);
        fp_actions_tick(p);
        fp_render(p);
        return;
    }
    if (wants_talk) {
        if (p->phase != FP_PHASE_TALK) fp_enter_talk(p);
        else fp_tick_talk(p);
        fp_actions_tick(p);
        fp_render(p);
        return;
    }
    if (wants_attend) {
        if (p->phase != FP_PHASE_ATTEND || p->shown_sys_state != p->sys_state) fp_enter_attend(p);
        else fp_advance_clip(p);
        fp_actions_tick(p);
        fp_render(p);
        return;
    }

    if (p->idle_showcase_enabled == FP_IDLE_HOLD) {
        /* Exact-review scenarios can hold the neutral idle director while
         * exercising semantic actions one by one. Direct actions and effects
         * still tick with their normal arbitration and lifecycle behavior. */
        if (p->phase != FP_PHASE_IDLE) fp_enter_idle(p, 0);
        else fp_advance_clip(p);
        fp_actions_tick(p);
        fp_render(p);
        return;
    }

    /* sys_state == FP_SYS_IDLE: the idle/emotion family owns the frame. */
    switch (p->phase) {
    case FP_PHASE_SYS_STATIC:
    case FP_PHASE_TALK:
    case FP_PHASE_ATTEND:
        fp_enter_idle(p, p->phase == FP_PHASE_SYS_STATIC);
        break;
    case FP_PHASE_IDLE:
        fp_tick_idle(p);
        break;
    case FP_PHASE_MICRO:
        if (p->pending_emotion != 0) {
            int32_t pending = p->pending_emotion;
            p->pending_emotion = 0;
            fp_enter_emotion(p, pending);
        } else if (fp_advance_clip(p)) {
            fp_enter_idle(p, 0);
        }
        break;
    case FP_PHASE_EMO_ENTER:
        if (fp_advance_clip(p)) {
            p->phase = FP_PHASE_EMO_HOLD;
            fp_start_clip(p, (int32_t)p->view.emotions[fp_find_emotion(p, p->current_emotion)].hold_clip);
        }
        break;
    case FP_PHASE_EMO_HOLD:
        fp_tick_emotion_hold(p);
        break;
    case FP_PHASE_EMO_EXIT:
        if (fp_advance_clip(p)) fp_finish_emotion_exit(p);
        break;
    default:
        fp_enter_idle(p, 0);
        break;
    }
    fp_actions_tick(p);
    fp_render(p);
}
