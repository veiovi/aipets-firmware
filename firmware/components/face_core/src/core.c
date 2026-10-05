/* Lifecycle, sticky inputs, dwell, and the tick. */
#include "fc_internal.h"

#define FC_DWELL_TICKS 8u        /* 264 ms minimum expression dwell */
#define FC_SEED_FALLBACK 0xA5F0C3D1u
#define FC_FIRST_BLINK_MS 1500u  /* lab: state.nextBlink = 1500 */
#define FC_GESTURE_RNG_SALT_SEED 0x67657374u /* "gest" */

FC_EXPORT(fc_arena_size)
uint32_t fc_arena_size(void)
{
    return ((uint32_t)sizeof(fc_core_t) + 15u) & ~15u;
}

static void fc_reset_dynamics(fc_core_t *c, uint32_t seed)
{
    c->rng = (seed == 0u) ? FC_SEED_FALLBACK : seed;
    c->tick_count = 0;
    c->time_ms = 0;
    c->next_blink_ms = FC_FIRST_BLINK_MS;
    c->blink_start_ms = 0;
    c->blink_env = 0;
    c->blinking = 0;
    c->next_saccade_ms = 0; /* lab: nextAt 0 — first saccade fires on tick 1 */
    c->sacc_x = 0;
    c->sacc_y = 0;
    c->breath_phase = 0;
    c->sway_phase = 0;
    c->gesture = FC_GESTURE_NONE;
    c->gesture_tick = 0;
    /* dedicated stream: shake jitter must not move the blink/saccade RNG */
    c->gesture_rng = c->rng ^ FC_GESTURE_RNG_SALT_SEED;
}

FC_EXPORT(fc_init)
fc_core_t *fc_init(void *arena, uint32_t arena_bytes)
{
    if (arena == NULL) return NULL;
    if (((uintptr_t)arena & 7u) != 0u) return NULL;
    if (arena_bytes < fc_arena_size()) return NULL;

    fc_core_t *c = (fc_core_t *)arena;
    fc_memset(c, 0, sizeof(*c));
    c->magic = FC_MAGIC;
    c->state = FC_STATE_IDLE;
    c->expr = FC_EXPR_NEUTRAL;
    c->intensity = 256;
    c->expr_valid = 0;
    c->has_first_expr = 0;
    fc_reset_dynamics(c, FC_SEED_FALLBACK);

    /* Preload the live pose to the composed neutral-idle base so the first
     * ticks ease from a sensible face instead of from zero. */
    fc_compose_target(c);
    fc_memcpy(c->current, c->target, sizeof(c->current));
    return c;
}

FC_EXPORT(fc_seed)
void fc_seed(fc_core_t *c, uint32_t seed)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    fc_reset_dynamics(c, seed);
}

FC_EXPORT(fc_set_presentation)
void fc_set_presentation(fc_core_t *c, uint8_t state, uint8_t expression,
                         fc_q88_t intensity)
{
    fc_set_state(c, state);
    fc_set_expression(c, expression, intensity);
}

FC_EXPORT(fc_set_state)
void fc_set_state(fc_core_t *c, uint8_t state)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    if (state >= FC_STATE_COUNT) state = FC_STATE_IDLE;
    c->in_state = state;
    c->state_dirty = 1;
}

FC_EXPORT(fc_set_expression)
void fc_set_expression(fc_core_t *c, uint8_t expression, fc_q88_t intensity)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    if (expression >= FC_EXPR_COUNT) expression = FC_EXPR_NEUTRAL;
    if (intensity < 0) intensity = 0;
    if (intensity > 256) intensity = 256;
    c->in_expr = expression;
    c->in_intensity = intensity;
    c->expr_valid = 1;
}

FC_EXPORT(fc_set_flags)
void fc_set_flags(fc_core_t *c, uint32_t flags)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    c->flags = flags;
}

FC_EXPORT(fc_set_speech)
void fc_set_speech(fc_core_t *c, fc_q88_t level)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    if (level < 0) level = 0;
    if (level > 256) level = 256;
    c->speech = level;
}

FC_EXPORT(fc_set_touch)
void fc_set_touch(fc_core_t *c, fc_q88_t x, fc_q88_t y, fc_q88_t pressure)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    if (x < -256) x = -256;
    if (x > 256) x = 256;
    if (y < -256) y = -256;
    if (y > 256) y = 256;
    if (pressure < 0) pressure = 0;
    if (pressure > 256) pressure = 256;
    c->touch_x = x;
    c->touch_y = y;
    c->touch_pressure = pressure;
}

FC_EXPORT(fc_trigger_tap)
void fc_trigger_tap(fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    c->tap_ticks = 6;
}

FC_EXPORT(fc_set_tilt)
void fc_set_tilt(fc_core_t *c, fc_q88_t x, fc_q88_t y)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    if (x < -256) x = -256;
    if (x > 256) x = 256;
    if (y < -256) y = -256;
    if (y > 256) y = 256;
    c->tilt_x = x;
    c->tilt_y = y;
}

static void fc_apply_pending_expression(fc_core_t *c)
{
    if (!c->expr_valid) return;

    int same = (c->in_expr == c->expr) && (c->in_intensity == c->intensity);
    if (same) {
        c->expr_valid = 0; /* same-target set never resets the dwell */
        return;
    }
    if (!c->has_first_expr || c->expr_dwell_left == 0u) {
        c->expr = c->in_expr;
        c->intensity = c->in_intensity;
        c->has_first_expr = 1;
        c->expr_dwell_left = FC_DWELL_TICKS;
        c->expr_valid = 0;
    }
    /* else: stays pending, latest wins, applied when the dwell expires */
}

FC_EXPORT(fc_tick)
void fc_tick(fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);

    c->time_ms += FC_TICK_MS;
    c->tick_count++;

    if (c->state_dirty) {
        c->state = c->in_state;
        c->state_dirty = 0;
    }
    fc_apply_pending_expression(c);
    if (c->expr_dwell_left > 0u) c->expr_dwell_left--;

    /* oscillator phases advance every tick regardless of flags (deterministic,
     * no RNG involved) */
    c->breath_phase = (uint16_t)(c->breath_phase + 723u); /* 2pi / 2992 ms */
    c->sway_phase = (uint16_t)(c->sway_phase + 413u);     /* 2pi / 5236 ms */

    if (!(c->flags & FC_FLAG_REDUCED_MOTION)) {
        fc_blink_tick(c);
        fc_saccade_tick(c);
    }

    fc_compose_target(c);
    fc_ease_all(c);
    if (c->tap_ticks > 0u) c->tap_ticks--;
    fc_gesture_step(c); /* after compose: tick N composes with envelope pos N */
}

FC_EXPORT(fc_pose)
const int16_t *fc_pose(const fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    return c->current;
}

FC_EXPORT(fc_active_state)
uint8_t fc_active_state(const fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    return c->state;
}

FC_EXPORT(fc_active_expression)
uint8_t fc_active_expression(const fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    return c->expr;
}

FC_EXPORT(fc_active_expression_intensity)
fc_q88_t fc_active_expression_intensity(const fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    return c->intensity;
}

FC_EXPORT(fc_tick_count)
uint32_t fc_tick_count(const fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    return c->tick_count;
}

#if defined(FC_TESTING)
/* Test-only observability: the RNG stream position, for asserting that
 * reduced motion consumes zero draws. */
FC_EXPORT(fc_test_rng_state)
uint32_t fc_test_rng_state(const fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    return c->rng;
}
#endif
