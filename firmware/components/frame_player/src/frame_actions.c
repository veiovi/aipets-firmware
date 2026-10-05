/* Data-driven full-frame actions and hard-alpha effect planes. */

#include "frame_internal.h"

static uint16_t fp_expression_bit(const fp_player_t *p)
{
    return p->current_emotion == 0 ? 1u : (uint16_t)(1u << p->current_emotion);
}

static uint16_t fp_profile_bit(const fp_player_t *p)
{
    return (uint16_t)(1u << (p->animation_profile + 1));
}

static uint16_t fp_performance_bit(const fp_player_t *p)
{
    if (p->phase != FP_PHASE_MICRO) return 0u;
    for (uint32_t i = 0; i < p->view.micro_count; i++) {
        if ((int32_t)p->view.micro[i].clip == p->clip_index) return (uint16_t)(1u << i);
    }
    return 0u;
}

static int32_t fp_tilt_semantic(const fp_player_t *p)
{
    int32_t ax = p->tilt_x < 0 ? -p->tilt_x : p->tilt_x;
    int32_t ay = p->tilt_y < 0 ? -p->tilt_y : p->tilt_y;
    if ((ax > ay ? ax : ay) < 50) return -1;
    if (ax >= ay) return p->tilt_x < 0 ? FP_SEM_TILT_LEFT : FP_SEM_TILT_RIGHT;
    return p->tilt_y < 0 ? FP_SEM_TILT_UP : FP_SEM_TILT_DOWN;
}

static int fp_base_allowed(const fp_player_t *p, uint32_t index)
{
    const fp_action_entry_t *a = &p->view.actions[index];
    if (a->plane != FP_PLANE_BASE) return 1;
    if (p->sys_state == FP_SYS_BOOTING) return a->semantic == FP_SEM_BOOTING;
    if (p->sys_state == FP_SYS_PROVISIONING) return a->semantic == FP_SEM_PROVISIONING;
    if (p->sys_state == FP_SYS_CONNECTING) return a->semantic == FP_SEM_CONNECTING;
    if (p->sys_state == FP_SYS_OFFLINE) return a->semantic == FP_SEM_OFFLINE;
    if (p->sys_state == FP_SYS_ERROR) return a->semantic == FP_SEM_ERROR;
    if (p->sys_state == FP_SYS_SPEAKING) return 0;
    if (p->sys_state == FP_SYS_LISTENING) return a->semantic == FP_SEM_LISTENING;
    if (p->sys_state == FP_SYS_THINKING) return a->semantic == FP_SEM_THINKING;
    return 1;
}

static int32_t fp_gesture_semantic(const fp_player_t *p)
{
    switch (p->gesture_pending) {
    case FP_GESTURE_NOD: return FP_SEM_GESTURE_NOD;
    case FP_GESTURE_FOCUS: return FP_SEM_GESTURE_FOCUS;
    case FP_GESTURE_HEARTBEAT: return FP_SEM_GESTURE_HEARTBEAT;
    case FP_GESTURE_BOUNCE: return FP_SEM_GESTURE_BOUNCE;
    case FP_GESTURE_WOBBLE: return FP_SEM_GESTURE_WOBBLE;
    case FP_GESTURE_POP: return FP_SEM_GESTURE_POP;
    default: return -1;
    }
}

static int fp_action_context(const fp_player_t *p, uint32_t index, int include_activation)
{
    const fp_action_entry_t *a = &p->view.actions[index];
    if ((a->flags & fp_profile_bit(p)) == 0u) return 0;
    if ((a->state_mask & (uint16_t)(1u << p->sys_state)) == 0u) return 0;
    if ((a->expression_mask & fp_expression_bit(p)) == 0u) return 0;
    if (a->performance_mask != 0u && (a->performance_mask & fp_performance_bit(p)) == 0u) return 0;
    if (p->optional_suppression_ticks > 0 && (a->flags & FP_ACTION_FLAG_ESSENTIAL) == 0u &&
        a->semantic != FP_SEM_TOUCH && a->semantic != FP_SEM_SHAKE &&
        (a->semantic < FP_SEM_GESTURE_NOD || a->semantic > FP_SEM_GESTURE_POP)) return 0;
    if (!include_activation) return 1;
    if (a->activation == FP_ACTIVATION_MANUAL) return p->requested_action==(int16_t)index;
    if (a->activation == FP_ACTIVATION_AMBIENT_LOOP) return p->animation_profile != FP_PROFILE_REDUCED;
    if (a->activation == FP_ACTIVATION_AMBIENT_EVENT) {
        return p->animation_profile != FP_PROFILE_REDUCED && p->action_event_countdown[index] == 0;
    }
    switch (a->semantic) {
    case FP_SEM_TOUCH: return p->tap_pending != 0;
    case FP_SEM_SHAKE: return p->shake_pending != 0;
    case FP_SEM_LISTENING: return p->sys_state == FP_SYS_LISTENING;
    case FP_SEM_THINKING: return p->sys_state == FP_SYS_THINKING;
    case FP_SEM_SPEAKING: return p->sys_state == FP_SYS_SPEAKING;
    case FP_SEM_BOOTING: return p->sys_state == FP_SYS_BOOTING;
    case FP_SEM_PROVISIONING: return p->sys_state == FP_SYS_PROVISIONING;
    case FP_SEM_CONNECTING: return p->sys_state == FP_SYS_CONNECTING;
    case FP_SEM_OFFLINE: return p->sys_state == FP_SYS_OFFLINE;
    case FP_SEM_ERROR: return p->sys_state == FP_SYS_ERROR;
    case FP_SEM_GESTURE_NOD:
    case FP_SEM_GESTURE_FOCUS:
    case FP_SEM_GESTURE_HEARTBEAT:
    case FP_SEM_GESTURE_BOUNCE:
    case FP_SEM_GESTURE_WOBBLE:
    case FP_SEM_GESTURE_POP:
        return fp_gesture_semantic(p) == a->semantic;
    case FP_SEM_TILT_LEFT:
    case FP_SEM_TILT_RIGHT:
    case FP_SEM_TILT_UP:
    case FP_SEM_TILT_DOWN:
        return p->animation_profile != FP_PROFILE_REDUCED && fp_tilt_semantic(p) == a->semantic;
    default: return 0;
    }
}

static void fp_draw_action_countdown(fp_player_t *p, uint32_t index)
{
    const fp_action_entry_t *a = &p->view.actions[index];
    uint32_t span = (uint32_t)a->interval_max_ticks - a->interval_min_ticks + 1u;
    p->action_event_countdown[index] = (int32_t)(a->interval_min_ticks + (fp_next_random(p) % (span > 0u ? span : 1u)));
}

static int fp_action_schedule_context(const fp_player_t *p, uint32_t index)
{
    const fp_action_entry_t *a = &p->view.actions[index];
    if ((a->flags & fp_profile_bit(p)) == 0u) return 0;
    if ((a->state_mask & (uint16_t)(1u << p->sys_state)) == 0u) return 0;
    if (p->optional_suppression_ticks > 0 && (a->flags & FP_ACTION_FLAG_ESSENTIAL) == 0u) return 0;
    return fp_base_allowed(p, index);
}

static void fp_update_schedules(fp_player_t *p)
{
    for (uint32_t i = 0; i < p->view.action_count; i++) {
        const fp_action_entry_t *a = &p->view.actions[i];
        if (a->activation != FP_ACTIVATION_AMBIENT_EVENT) continue;
        /* The interval belongs to the action's state/profile context, not to a
         * fleeting facial expression. Selection waits at zero until the
         * allowed expression returns. */
        int eligible = fp_action_schedule_context(p, i);
        if (!eligible) {
            p->action_was_eligible[i] = 0u;
            p->action_event_countdown[i] = -1;
        } else if (!p->action_was_eligible[i]) {
            p->action_was_eligible[i] = 1u;
            fp_draw_action_countdown(p, i);
        } else if (p->action_event_countdown[i] > 0) {
            p->action_event_countdown[i]--;
        }
    }
}

/* FP_IDLE_NATURAL: an ambient loop that another action or a state interrupts
 * keeps the step and ticks it would have shown next, and resumes there when it
 * returns, so a long idle loop plays every moment. A loop bound to an idle
 * performance starts with its performance. Mirrors player.ts keepLoop(). */
static void fp_keep_loop(fp_player_t *p, const fp_action_cursor_t *c)
{
    const fp_action_entry_t *a = &p->view.actions[c->action];
    if (p->idle_showcase_enabled != FP_IDLE_NATURAL || a->activation != FP_ACTIVATION_AMBIENT_LOOP ||
        a->performance_mask != 0u || c->entering) return;
    p->loop_resume_step[c->action] = c->step;
    p->loop_resume_ticks[c->action] = c->ticks_left;
}

/* The main clip continues where fp_keep_loop left it, once. */
static void fp_resume_loop(fp_player_t *p, fp_action_cursor_t *c)
{
    int32_t ticks = p->loop_resume_ticks[c->action];
    if (ticks <= 0) return;
    c->step = p->loop_resume_step[c->action];
    c->ticks_left = ticks;
    p->loop_resume_ticks[c->action] = 0;
}

static fp_action_cursor_t fp_start_action(fp_player_t *p, uint32_t index)
{
    fp_action_cursor_t c;
    const fp_action_entry_t *a = &p->view.actions[index];
    c.action = (int16_t)index;
    c.clip = a->enter_clip != FP_ACTION_NO_CLIP ? a->enter_clip : a->clip;
    c.step = 0u;
    c.ticks_left = (int32_t)fp_clip_step_at(p, c.clip, 0u).duration_ticks;
    c.entering = a->enter_clip != FP_ACTION_NO_CLIP;
    c.exiting = 0u;
    c.complete = 0u;
    if (a->activation == FP_ACTIVATION_AMBIENT_EVENT) p->action_event_countdown[index] = -2;
    p->action_cooldown_ticks[index] = a->cooldown_ticks;
    if (!c.entering) fp_resume_loop(p, &c);
    return c;
}

static void fp_advance_action(fp_player_t *p, fp_action_cursor_t *c)
{
    if (c->complete) return;
    c->ticks_left--;
    if (c->ticks_left > 0) return;
    uint16_t count = fp_clip_step_count(p, c->clip);
    if ((uint32_t)(c->step + 1u) < count) {
        c->step++;
        c->ticks_left = (int32_t)fp_clip_step_at(p, c->clip, c->step).duration_ticks;
        return;
    }
    const fp_action_entry_t *a = &p->view.actions[c->action];
    uint8_t loop = fp_clip_loop_mode(p, c->clip);
    if (!c->entering && !c->exiting && fp_triggered_one_shot(a->semantic, a->activation)) loop = FP_LOOP_ONCE;
    if (loop == FP_LOOP_LOOP) {
        c->step = 0u;
        c->ticks_left = (int32_t)fp_clip_step_at(p, c->clip, 0u).duration_ticks;
    } else if (loop == FP_LOOP_HOLD_LAST) {
        c->ticks_left = (int32_t)fp_clip_step_at(p, c->clip, c->step).duration_ticks;
    } else if (c->entering) {
        c->clip = a->clip;
        c->step = 0u;
        c->ticks_left = (int32_t)fp_clip_step_at(p, c->clip, 0u).duration_ticks;
        c->entering = 0u;
        c->complete = 0u;
        fp_resume_loop(p, c);
    } else {
        c->complete = 1u;
    }
}

static int fp_begin_action_exit(fp_player_t *p, fp_action_cursor_t *c)
{
    const fp_action_entry_t *a = &p->view.actions[c->action];
    if (!c->exiting && a->exit_clip != FP_ACTION_NO_CLIP) {
        c->clip = a->exit_clip;
        c->step = 0u;
        c->ticks_left = (int32_t)fp_clip_step_at(p, c->clip, 0u).duration_ticks;
        c->exiting = 1u;
        c->entering = 0u;
        c->complete = 0u;
        return 1;
    }
    return 0;
}

static int fp_action_continuation(const fp_player_t *p, uint32_t index)
{
    const fp_action_entry_t *a = &p->view.actions[index];
    if(a->activation==FP_ACTIVATION_MANUAL)return fp_base_allowed(p,index)&&fp_action_context(p,index,1);
    int one_shot =
        a->activation == FP_ACTIVATION_AMBIENT_EVENT || a->semantic == FP_SEM_TOUCH ||
        a->semantic == FP_SEM_SHAKE ||
        (a->semantic >= FP_SEM_GESTURE_NOD && a->semantic <= FP_SEM_GESTURE_POP);
    return fp_base_allowed(p, index) && fp_action_context(p, index, one_shot ? 0 : 1);
}

static int32_t fp_select_action(const fp_player_t *p, uint32_t plane)
{
    int32_t best = -1;
    int32_t priority = -1;
    for (uint32_t i = 0; i < p->view.action_count; i++) {
        const fp_action_entry_t *a = &p->view.actions[i];
        if (a->plane != plane || p->action_cooldown_ticks[i] > 0 || !fp_base_allowed(p, i) || !fp_action_context(p, i, 1)) continue;
        if ((int32_t)a->priority > priority) {
            best = (int32_t)i;
            priority = a->priority;
        }
    }
    /* A requested manual action outranks an ambient loop: the loop is what the
     * pet does when nothing is requested. Its own cooldown, set when it
     * started, does not count while it runs. This reads the cursor as updated
     * this tick, where player.ts reads it as of the start of the tick; they
     * agree because an action that ends always clears its own request first. */
    int32_t r = p->requested_action;
    if (r >= 0 && best >= 0 && best != r && p->view.actions[best].activation == FP_ACTIVATION_AMBIENT_LOOP &&
        p->view.actions[r].plane == plane && (p->action_cooldown_ticks[r] <= 0 || p->action_cursors[plane].action == r) &&
        fp_base_allowed(p, (uint32_t)r) && fp_action_context(p, (uint32_t)r, 1)) best = r;
    return best;
}

uint8_t fp_actions_request(fp_player_t *p,int16_t action)
{
    if(action==-1){p->requested_action=-1;return 1u;}
    if(action<0||action>=p->view.action_count||p->view.actions[action].activation!=FP_ACTIVATION_MANUAL||
        p->action_cooldown_ticks[action]>0||!fp_base_allowed(p,(uint32_t)action)||!fp_action_context(p,(uint32_t)action,0))return 0u;
    p->requested_action=action;
    return 1u;
}

/* A manual action fp_actions_request would accept now. */
static int fp_manual_ready(const fp_player_t *p, uint32_t index)
{
    return p->view.actions[index].activation == FP_ACTIVATION_MANUAL && p->action_cooldown_ticks[index] <= 0 &&
        fp_base_allowed(p, index) && fp_action_context(p, index, 0);
}

/* One that calm may start: ready now, and over when its clip ends. */
static int fp_calm_candidate(const fp_player_t *p, uint32_t index)
{
    return fp_manual_ready(p, index) && fp_clip_loop_mode(p, p->view.actions[index].clip) == FP_LOOP_ONCE;
}

/* FP_IDLE_NATURAL: a stretch of calm ends with one of the pack's manual actions
 * that plays once. Calm is the idle family outside the reduced profile, with no
 * input, emotion or request, and nothing but an ambient loop on the base plane.
 * While none is ready (a cooldown, a moment of degraded rendering) the calm
 * pauses. The countdown is drawn when calm begins with one ready, so no other
 * mode or pack draws from the PRNG here. Mirrors player.ts tickCalm(). */
void fp_actions_calm(fp_player_t *p)
{
    const fp_action_cursor_t *base = &p->action_cursors[FP_PLANE_BASE];
    int calm = p->idle_showcase_enabled == FP_IDLE_NATURAL && p->sys_state == FP_SYS_IDLE &&
        p->animation_profile != FP_PROFILE_REDUCED &&
        (p->phase == FP_PHASE_IDLE || p->phase == FP_PHASE_MICRO) && !p->tap_pending && !p->shake_pending &&
        p->gesture_pending == FP_GESTURE_NONE && p->pending_emotion == 0 && p->requested_action < 0 &&
        (base->action < 0 || p->view.actions[base->action].activation == FP_ACTIVATION_AMBIENT_LOOP);
    if (!calm) {
        p->calm_countdown = -1;
        return;
    }
    uint32_t ready = 0;
    for (uint32_t i = 0; i < p->view.action_count; i++) ready += (uint32_t)fp_calm_candidate(p, i);
    if (!ready) return;
    if (p->calm_countdown < 0) {
        p->calm_countdown = (int32_t)(FP_CALM_MIN_TICKS + fp_next_random(p) % (uint32_t)(FP_CALM_MAX_TICKS - FP_CALM_MIN_TICKS + 1));
        return;
    }
    if (--p->calm_countdown > 0) return;
    uint32_t pick = fp_next_random(p) % ready;
    for (uint32_t i = 0; i < p->view.action_count; i++) {
        if (fp_calm_candidate(p, i) && pick-- == 0u) {
            p->requested_action = (int16_t)i;
            break;
        }
    }
    p->calm_countdown = -1;
}

void fp_actions_tick(fp_player_t *p)
{
    if(p->requested_action>=0&&(!fp_base_allowed(p,(uint32_t)p->requested_action)||!fp_action_context(p,(uint32_t)p->requested_action,0)))p->requested_action=-1;
    if (p->optional_suppression_ticks > 0) p->optional_suppression_ticks--;
    if (p->tap_pending) p->fallback_touch_ticks = 8;
    else if (p->fallback_touch_ticks > 0) p->fallback_touch_ticks--;
    if (p->shake_pending) p->fallback_shake_ticks = 12;
    else if (p->fallback_shake_ticks > 0) p->fallback_shake_ticks--;
    fp_update_schedules(p);
    for (uint32_t i = 0; i < p->view.action_count; i++) {
        if (p->action_cooldown_ticks[i] > 0) p->action_cooldown_ticks[i]--;
    }
    for (uint32_t plane = 0; plane < FP_PLANE_COUNT; plane++) {
        fp_action_cursor_t *c = &p->action_cursors[plane];
        if (c->action >= 0) {
            fp_advance_action(p, c);
            const fp_action_entry_t *a = &p->view.actions[c->action];
            if (c->complete) {
                if(p->requested_action==c->action)p->requested_action=-1;
                if (a->activation == FP_ACTIVATION_AMBIENT_EVENT) fp_draw_action_countdown(p, (uint32_t)c->action);
                c->action = -1;
                c->exiting = 0u;
                c->entering = 0u;
            } else if (!c->exiting && !fp_action_continuation(p, (uint32_t)c->action)) {
                fp_keep_loop(p, c);
                if (!fp_begin_action_exit(p, c)) c->action = -1;
            }
        }
        int32_t selected = fp_select_action(p, plane);
        if (!c->exiting && selected >= 0 && (c->action < 0 || selected != c->action)) {
            int32_t current_priority = c->action >= 0 ? p->view.actions[c->action].priority : -1;
            if (c->action < 0 || (int32_t)p->view.actions[selected].priority >= current_priority ||
                (selected == p->requested_action && p->view.actions[c->action].activation == FP_ACTIVATION_AMBIENT_LOOP)) {
                if (c->action >= 0 && p->view.actions[c->action].activation == FP_ACTIVATION_AMBIENT_EVENT) {
                    fp_draw_action_countdown(p, (uint32_t)c->action);
                }
                if(c->action>=0&&p->requested_action==c->action)p->requested_action=-1;
                if (c->action >= 0) fp_keep_loop(p, c);
                *c = fp_start_action(p, (uint32_t)selected);
            }
        }
        if(plane==FP_PLANE_BASE&&p->requested_action>=0&&c->action!=p->requested_action)p->requested_action=-1;
    }
    /* A tick whose profile turns a loop off also forgets where it stopped. */
    for (uint32_t i = 0; i < p->view.action_count; i++) {
        if ((p->view.actions[i].flags & fp_profile_bit(p)) == 0u || p->animation_profile == FP_PROFILE_REDUCED) p->loop_resume_ticks[i] = 0;
    }
    p->tap_pending = 0;
    p->shake_pending = 0;
    p->gesture_pending = FP_GESTURE_NONE;
}

int32_t fp_actions_frame(const fp_player_t *p)
{
    const fp_action_cursor_t *c = &p->action_cursors[FP_PLANE_BASE];
    if (c->action >= 0) return (int32_t)fp_clip_step_at(p, c->clip, c->step).frame;
    return fp_director_frame(p);
}

int32_t fp_actions_offset_x(const fp_player_t *p)
{
    if (fp_speaking_motion(p)) return p->drift_x;
    if (!p->view.parallax_safe || p->animation_profile == FP_PROFILE_REDUCED || p->view.max_parallax_px == 0u) return 0;
    int32_t ax = p->tilt_x < 0 ? -p->tilt_x : p->tilt_x;
    if (ax < 15) return 0;
    return p->tilt_x < 0 ? -(int32_t)p->view.max_parallax_px : (int32_t)p->view.max_parallax_px;
}

int32_t fp_actions_offset_y(const fp_player_t *p)
{
    if (fp_speaking_motion(p)) return p->drift_y;
    int32_t parallax = 0;
    if (p->view.parallax_safe && p->animation_profile != FP_PROFILE_REDUCED && p->view.max_parallax_px > 0u) {
        int32_t ay = p->tilt_y < 0 ? -p->tilt_y : p->tilt_y;
        if (ay >= 15) parallax = p->tilt_y < 0 ? -(int32_t)p->view.max_parallax_px : (int32_t)p->view.max_parallax_px;
    }
    if (p->action_cursors[FP_PLANE_BASE].action >= 0) return parallax;
    return parallax + fp_director_offset_y(p);
}

static void fp_apply_masked(fp_player_t *p, uint16_t *target, uint32_t frame_index)
{
    const uint8_t *dir = p->pack + p->view.frame_dir_offset + frame_index * FP_FRAME_DIR_ENTRY_BYTES;
    uint32_t length = fp_rd32(dir + 4u);
    const uint8_t *data = fp_decode_resource(p->pack + fp_rd32(dir), length, dir[8], p->info.width, &p->codec_workspace, &length);
    if (!data) return;
    uint32_t x0 = data[0], y0 = data[1], w = data[2], h = data[3];
    uint32_t mask_bytes = (w * h + 7u) / 8u;
    uint32_t color_at = 4u + mask_bytes;
    uint8_t codec = dir[8];
    uint32_t run_left = 0u;
    uint8_t run_color = 0u;
    for (uint32_t pixel = 0; pixel < w * h; pixel++) {
        if (((data[4u + (pixel >> 3)] >> (pixel & 7u)) & 1u) == 0u) continue;
        uint32_t x = x0 + (pixel % w);
        uint32_t y = y0 + (pixel / w);
        if (codec == FP_CODEC_INDEX8_MASKED_RECT_RLE) {
            if (run_left == 0u) { run_left = data[color_at++]; run_color = data[color_at++]; }
            target[y * p->info.width + x] = p->view.palette[run_color];
            run_left--;
        } else {
            target[y * p->info.width + x] = p->view.palette[data[color_at++]];
        }
    }
}

static int fp_active_semantic(const fp_player_t *p, uint8_t semantic)
{
    for (uint32_t plane = 0; plane < FP_PLANE_COUNT; plane++) {
        int32_t action = p->action_cursors[plane].action;
        if (action >= 0 && p->view.actions[action].semantic == semantic) return 1;
    }
    return 0;
}

static void fp_pixel(const fp_player_t *p, uint16_t *target, int32_t x, int32_t y, uint16_t color)
{
    if (x < 0 || x >= 120 || y < 0 || y >= 120) return;
    int32_t scale = p->info.width / 120;
    for (int32_t dy = 0; dy < scale; dy++) for (int32_t dx = 0; dx < scale; dx++)
        target[(y * scale + dy) * p->info.width + x * scale + dx] = color;
}

static void fp_star(const fp_player_t *p, uint16_t *target, int32_t x, int32_t y, uint16_t color)
{
    fp_pixel(p, target, x, y, color); fp_pixel(p, target, x - 1, y, color); fp_pixel(p, target, x + 1, y, color);
    fp_pixel(p, target, x, y - 1, color); fp_pixel(p, target, x, y + 1, color);
    fp_pixel(p, target, x - 2, y, 0xffffu); fp_pixel(p, target, x + 2, y, 0xffffu);
    fp_pixel(p, target, x, y - 2, 0xffffu); fp_pixel(p, target, x, y + 2, 0xffffu);
}

static void fp_draw_fallbacks(const fp_player_t *p, uint16_t *target)
{
    if (!p->view.fallbacks_enabled) return;
    uint32_t phase = (uint32_t)p->bob_tick & 3u;
    if (p->sys_state == FP_SYS_LISTENING && !fp_active_semantic(p, FP_SEM_LISTENING)) {
        int32_t r = phase < 2u ? 3 : 4;
        for (int32_t x = 60 - r; x <= 60 + r; x++) { fp_pixel(p, target, x, 8 - r, 0x07ffu); fp_pixel(p, target, x, 8 + r, 0x07ffu); }
        for (int32_t y = 8 - r; y <= 8 + r; y++) { fp_pixel(p, target, 60 - r, y, 0x07ffu); fp_pixel(p, target, 60 + r, y, 0x07ffu); }
    }
    if (p->sys_state == FP_SYS_THINKING && !fp_active_semantic(p, FP_SEM_THINKING)) {
        uint32_t active = (uint32_t)p->bob_tick % 3u;
        for (uint32_t i = 0; i < 3u; i++) {
            uint16_t color = i == active ? 0xffffu : 0x8410u;
            fp_pixel(p, target, 54 + (int32_t)i * 6, 9, color); fp_pixel(p, target, 55 + (int32_t)i * 6, 9, color);
            fp_pixel(p, target, 54 + (int32_t)i * 6, 10, color); fp_pixel(p, target, 55 + (int32_t)i * 6, 10, color);
        }
    }
    if (p->fallback_touch_ticks > 0 && !fp_active_semantic(p, FP_SEM_TOUCH)) {
        int32_t spread = 1 + ((8 - p->fallback_touch_ticks) >> 1);
        fp_star(p, target, 27 - spread, 42 - spread, 0xffe0u); fp_star(p, target, 93 + spread, 39 - spread, 0xffe0u);
        fp_star(p, target, 82 + spread, 75 + spread, 0xf81fu);
    }
    if (p->fallback_shake_ticks > 0 && !fp_active_semantic(p, FP_SEM_SHAKE)) {
        int32_t dx = (p->fallback_shake_ticks & 1) == 0 ? 2 : -2;
        fp_star(p, target, 23 + dx, 32, 0xffe0u); fp_star(p, target, 97 - dx, 32, 0x07ffu);
        fp_pixel(p, target, 19 - dx, 56, 0xf81fu); fp_pixel(p, target, 101 + dx, 56, 0xf81fu);
    }
}

void fp_actions_composite(fp_player_t *p, uint16_t *target)
{
    for (uint32_t plane = FP_PLANE_CHARACTER_FX; plane < FP_PLANE_COUNT; plane++) {
        const fp_action_cursor_t *c = &p->action_cursors[plane];
        if (c->action < 0) continue;
        fp_apply_masked(p, target, fp_clip_step_at(p, c->clip, c->step).frame);
    }
    fp_draw_fallbacks(p, target);
}
