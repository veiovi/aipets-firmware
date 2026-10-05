/* Internal state and kernels for face_core. Not installed; harness-only tests
 * reach the kernels through the FC_TESTING exports at the bottom. */
#pragma once

#include "face_core.h"

/* Freestanding helpers. support.c exposes standard libc backstops only to the
 * wasm build; firmware calls these prefixed helpers and never overrides libc. */
void *fc_memcpy(void *dest, const void *src, size_t n);
void *fc_memset(void *dest, int value, size_t n);
void *fc_memmove(void *dest, const void *src, size_t n);

#define FC_MAGIC 0xFACEC04Eu

#if defined(FC_TESTING)
#define FC_ASSERT(x) do { if (!(x)) { __builtin_trap(); } } while (0)
#else
#define FC_ASSERT(x) ((void)0)
#endif

/* Emotion pose targets, Q8.8, per-key MIX toward these by intensity.
 * v[] is indexed by channel 0..6 (EYE_OPEN..CHEEK); MOUTH_OPEN is special-cased
 * because its base is the speech-gated value (see compose_target). */
typedef struct {
    uint8_t mask;        /* bit i = channel i (0..6) participates */
    int16_t v[7];
    int16_t mouth_lerp;  /* -1 = none; else Q8.8 lerp target (surprised) */
    int16_t mouth_floor; /* 0 = none; else Q8.8 floor before intensity scale (excited) */
} fc_emotion_t;

extern const fc_emotion_t fc_emotions[FC_EXPR_COUNT];
extern const uint32_t fc_alpha_q16[FC_CHANNEL_COUNT]; /* per-channel easing alphas */
extern const int16_t fc_clamp_min[FC_CHANNEL_COUNT];
extern const int16_t fc_clamp_max[FC_CHANNEL_COUNT];

struct fc_core {
    uint32_t magic;
    uint32_t rng;
    uint32_t tick_count;
    uint32_t time_ms;     /* virtual, wraps; advanced FC_TICK_MS per tick */

    /* state is next-tick immediate; expression is latest-wins + dwell */
    uint8_t in_state;
    uint8_t state_dirty;
    uint8_t in_expr;
    uint8_t expr_valid;
    int16_t in_intensity;
    uint8_t has_first_expr;
    uint8_t expr_dwell_left;

    /* other sticky inputs */
    int16_t speech;
    int16_t touch_x, touch_y, touch_pressure;
    int16_t tilt_x, tilt_y;
    uint8_t tap_ticks;
    uint8_t _pad0;
    uint32_t flags;

    /* active presentation */
    uint8_t state, expr;
    int16_t intensity;

    /* blink scheduler */
    uint32_t next_blink_ms;
    uint32_t blink_start_ms;
    int16_t blink_env;    /* raw Q8.8 triangle envelope */
    uint8_t blinking;
    uint8_t _pad1;

    /* saccade scheduler */
    uint32_t next_saccade_ms;
    int16_t sacc_x, sacc_y;

    /* oscillator phases (u16 turns; advance every tick regardless of flags) */
    uint16_t breath_phase, sway_phase;

    /* one-shot gesture engine (gestures.c); own RNG stream so shake jitter
     * never perturbs the blink/saccade stream position */
    uint8_t gesture;        /* fc_gesture_t, FC_GESTURE_NONE when idle */
    uint8_t _pad2;
    uint16_t gesture_tick;  /* 0-based tick inside the active envelope */
    uint32_t gesture_rng;

    int16_t target[FC_CHANNEL_COUNT];
    int16_t current[FC_CHANNEL_COUNT];
};

/* kernels (compositor.c) */
int32_t fc_blend_mix(int32_t acc, int32_t v, int32_t w_q88);
int32_t fc_blend_add(int32_t acc, int32_t v, int32_t w_q88);
int32_t fc_blend_mul(int32_t acc, int32_t v, int32_t w_q88);
int32_t fc_ease_step(int32_t current, int32_t target, uint32_t alpha_q16);
void fc_compose_target(fc_core_t *c);
void fc_ease_all(fc_core_t *c);

/* rng.c */
uint32_t fc_rng_next(uint32_t *s);

/* sine.c: phase 0..65535 == 0..2pi, returns Q8.8 in [-256, 256] */
int32_t fc_sin_q88(uint16_t phase);

/* layers.c */
void fc_blink_tick(fc_core_t *c);
void fc_saccade_tick(fc_core_t *c);

/* gestures.c — apply writes gesture contributions into the composing target
 * (identity when idle: spin 0, energy 0, no offset/scale deltas), step
 * advances/retires the envelope once per tick after composition. */
void fc_gesture_apply(fc_core_t *c, int32_t t[FC_CHANNEL_COUNT]);
void fc_gesture_step(fc_core_t *c);
