/* Blend kernels, target composition, and easing.
 *
 * Rounding convention (mirrored exactly by the TS reference):
 *   Q8.8 blend:  (x * w + 128) >> 8      arithmetic shift == floor
 *   Q16 easing:  (d * alpha + 32768) >> 16, then snap when |target-current| <= 1
 */
#include "fc_internal.h"

int32_t fc_blend_mix(int32_t acc, int32_t v, int32_t w_q88)
{
    return acc + (((v - acc) * w_q88 + 128) >> 8);
}

int32_t fc_blend_add(int32_t acc, int32_t v, int32_t w_q88)
{
    return acc + ((v * w_q88 + 128) >> 8);
}

int32_t fc_blend_mul(int32_t acc, int32_t v, int32_t w_q88)
{
    int32_t m = 256 + (((v - 256) * w_q88 + 128) >> 8); /* lerp(1.0, v, w) */
    return (acc * m + 128) >> 8;
}

int32_t fc_ease_step(int32_t current, int32_t target, uint32_t alpha_q16)
{
    int32_t d = target - current;
    if (d == 0) return current;
    current += ((d * (int32_t)alpha_q16) + 32768) >> 16;
    d = target - current;
    if (d != 0 && d >= -1 && d <= 1) current = target;
    return current;
}

static int32_t fc_mouth_resolve(int32_t speech, int32_t gate, int32_t clamp_m,
                                int32_t floor_m)
{
    int32_t m = gate ? speech : 0;
    if (m < floor_m) m = floor_m;
    if (m > clamp_m) m = clamp_m;
    return m;
}

void fc_compose_target(fc_core_t *c)
{
    int32_t t[FC_CHANNEL_COUNT] = {0};

    /* base pose (lab literals: eyeOpen 1, pupil 1, smile .45, cheek .2) */
    t[FC_CH_EYE_OPEN] = 256;
    t[FC_CH_PUPIL_SCALE] = 256;
    t[FC_CH_SMILE] = 115;
    t[FC_CH_CHEEK] = 51;
    t[FC_CH_MOUTH_WIDTH] = 141;      /* 0.55 neutral width */
    t[FC_CH_MOUTH_ROUNDNESS] = 51;  /* 0.20 neutral roundness */
    t[FC_CH_HEAD_SCALE_X] = 256;
    t[FC_CH_HEAD_SCALE_Y] = 256;

    int32_t gate = (c->state == FC_STATE_SPEAKING) ? 1 : 0;
    int32_t mouth = gate ? c->speech : 0;

    /* emotion: per-key MIX toward the table by intensity */
    const fc_emotion_t *e = &fc_emotions[c->expr];
    for (int i = 0; i < 7; i++) {
        if (e->mask & (uint8_t)(1u << i)) {
            t[i] = fc_blend_mix(t[i], e->v[i], c->intensity);
        }
    }
    if (e->mouth_lerp >= 0) {
        mouth = fc_blend_mix(mouth, e->mouth_lerp, c->intensity);
    }
    int32_t floor_m = 0;
    if (e->mouth_floor > 0) {
        floor_m = (e->mouth_floor * c->intensity + 128) >> 8;
    }

    /* operational-state overlay (booting/provisioning/connecting/idle/speaking
     * are deliberately neutral rows) */
    int32_t clamp_m = 256;
    switch (c->state) {
    case FC_STATE_LISTENING:
        t[FC_CH_EYE_OPEN] += 31;      /* +0.12 */
        t[FC_CH_PUPIL_SCALE] += 15;   /* +0.06 */
        clamp_m = 0;
        break;
    case FC_STATE_THINKING:
        t[FC_CH_EYE_OPEN] = (t[FC_CH_EYE_OPEN] * 230 + 128) >> 8; /* *0.9 */
        t[FC_CH_BROW_TILT] += 102;    /* +0.4 */
        if (!(c->flags & FC_FLAG_REDUCED_MOTION)) {
            /* head sway, amp 0.35 — off under reduced motion (it is secondary
             * motion, and gating it also lets reduced-motion poses converge) */
            t[FC_CH_HEAD_TILT] += (fc_sin_q88(c->sway_phase) * 90 + 128) >> 8;
        }
        break;
    case FC_STATE_OFFLINE:
        t[FC_CH_EYE_OPEN] = (t[FC_CH_EYE_OPEN] * 195 + 128) >> 8; /* *0.76 */
        t[FC_CH_SMILE] = -64;         /* -0.25 */
        t[FC_CH_BROW_TILT] = -179;    /* -0.7 */
        break;
    case FC_STATE_ERROR:
        t[FC_CH_EYE_OPEN] = (t[FC_CH_EYE_OPEN] * 210 + 128) >> 8; /* *0.82 */
        t[FC_CH_SMILE] = -148;        /* -0.58 */
        t[FC_CH_BROW_TILT] = -243;    /* -0.95 */
        break;
    default:
        break;
    }

    t[FC_CH_MOUTH_OPEN] = fc_mouth_resolve(mouth, 1, clamp_m, floor_m);

    /* Semantic mouth-shape channels let data-only packs select a useful
     * sprite without embedding any character-specific lip geometry here. */
    int32_t mouth_open = t[FC_CH_MOUTH_OPEN];
    int32_t positive_smile = t[FC_CH_SMILE] > 0 ? t[FC_CH_SMILE] : 0;
    t[FC_CH_MOUTH_WIDTH] = 128 + ((mouth_open * 72 + 128) >> 8) +
                           ((positive_smile * 40 + 128) >> 8);
    if (c->state == FC_STATE_SPEAKING && mouth_open > 24) {
        /* Amplitude, not wall-clock phase, selects the semantic mouth family.
         * That keeps a held input convergent while live speech still traverses
         * round, medium and wide shapes naturally. */
        t[FC_CH_MOUTH_ROUNDNESS] = mouth_open < 96 ? 205 :
            mouth_open < 180 ? 92 : 45;
    } else if (c->expr == FC_EXPR_SURPRISED) {
        t[FC_CH_MOUTH_ROUNDNESS] = fc_blend_mix(51, 230, c->intensity);
    }

    /* behavior channels: raw oscillator/scheduler outputs (snap channels) */
    if (!(c->flags & FC_FLAG_REDUCED_MOTION)) {
        t[FC_CH_BREATH] = (fc_sin_q88(c->breath_phase) * 166 + 128) >> 8; /* amp 0.65 */
        t[FC_CH_GAZE_X] = c->sacc_x;
        t[FC_CH_GAZE_Y] = c->sacc_y;
        t[FC_CH_BLINK] = c->blink_env;
    }

    /* Touch is an immediate local reflex and therefore remains active under
     * reduced motion. It overrides ambient gaze only while pressed. */
    t[FC_CH_TOUCH_X] = c->touch_x;
    t[FC_CH_TOUCH_Y] = c->touch_y;
    t[FC_CH_TOUCH_PRESS] = c->touch_pressure;
    if (c->touch_pressure > 0) {
        t[FC_CH_GAZE_X] = c->touch_x;
        t[FC_CH_GAZE_Y] = c->touch_y;
    }

    static const int16_t tap_env[7] = {0, 24, 48, 80, 128, 160, 192};
    int32_t tap = tap_env[c->tap_ticks <= 6u ? c->tap_ticks : 6u];
    if (c->flags & FC_FLAG_REDUCED_MOTION) tap >>= 1;
    int32_t squash = c->touch_pressure > tap ? c->touch_pressure : tap;
    t[FC_CH_TOUCH_SQUASH] = squash;

    /* Tilt is exposed directly and also contributes restrained head motion.
     * Reduced motion halves the visual transform while preserving input data. */
    t[FC_CH_TILT_X] = c->tilt_x;
    t[FC_CH_TILT_Y] = c->tilt_y;
    int32_t tilt_weight = (c->flags & FC_FLAG_REDUCED_MOTION) ? 128 : 256;
    int32_t tx = (c->tilt_x * tilt_weight + 128) >> 8;
    int32_t ty = (c->tilt_y * tilt_weight + 128) >> 8;
    t[FC_CH_HEAD_X] = (tx * 32 + 128) >> 8;
    t[FC_CH_HEAD_Y] = ((ty * 26 + 128) >> 8) + ((squash * 10 + 128) >> 8);
    t[FC_CH_HEAD_TILT] += (tx * 51 + 128) >> 8;
    t[FC_CH_HEAD_SCALE_X] = 256 + ((squash * 10 + 128) >> 8);
    t[FC_CH_HEAD_SCALE_Y] = 256 - ((squash * 18 + 128) >> 8);

    t[FC_CH_ACCESSORY_MOTION] = (c->flags & FC_FLAG_REDUCED_MOTION) ? 0 :
        t[FC_CH_BREATH] + ((tx * 64 + 128) >> 8) + ((tap * 48 + 128) >> 8);
    t[FC_CH_AUDIO_ENERGY] = c->speech;
    switch (c->state) {
    case FC_STATE_BOOTING: t[FC_CH_STATUS_ENERGY] = 80; break;
    case FC_STATE_PROVISIONING: t[FC_CH_STATUS_ENERGY] = 128; break;
    case FC_STATE_CONNECTING: t[FC_CH_STATUS_ENERGY] = 160; break;
    case FC_STATE_LISTENING: t[FC_CH_STATUS_ENERGY] = 256; break;
    case FC_STATE_THINKING: t[FC_CH_STATUS_ENERGY] = 160; break;
    case FC_STATE_SPEAKING: t[FC_CH_STATUS_ENERGY] = c->speech; break;
    case FC_STATE_OFFLINE: t[FC_CH_STATUS_ENERGY] = 96; break;
    case FC_STATE_ERROR: t[FC_CH_STATUS_ENERGY] = 256; break;
    default: t[FC_CH_STATUS_ENERGY] = 0; break;
    }

    /* universal gestures layer whole-face motion on top of everything above;
     * identity when idle so gesture-free composition stays bit-exact */
    fc_gesture_apply(c, t);

    /* blink multiplies the composed eye target LAST (lab order):
     * eyeOpen *= 1 - blink*0.98, then the clamp floor keeps a visible lid line */
    int32_t bm = 256 - ((t[FC_CH_BLINK] * 251 + 128) >> 8);
    t[FC_CH_EYE_OPEN] = (t[FC_CH_EYE_OPEN] * bm + 128) >> 8;
    t[FC_CH_EYE_OPEN_LEFT] = t[FC_CH_EYE_OPEN];
    t[FC_CH_EYE_OPEN_RIGHT] = t[FC_CH_EYE_OPEN];
    t[FC_CH_BROW_LEFT] = t[FC_CH_BROW_TILT];
    t[FC_CH_BROW_RIGHT] = -t[FC_CH_BROW_TILT];

    for (int i = 0; i < FC_CHANNEL_COUNT; i++) {
        int32_t v = t[i];
        if (v < fc_clamp_min[i]) v = fc_clamp_min[i];
        if (v > fc_clamp_max[i]) v = fc_clamp_max[i];
        c->target[i] = (int16_t)v;
    }
}

void fc_ease_all(fc_core_t *c)
{
    for (int i = 0; i < FC_CHANNEL_COUNT; i++) {
        uint32_t alpha = fc_alpha_q16[i];
        if ((c->flags & FC_FLAG_EXTERNAL_SPEECH_ENVELOPE) &&
            (i == FC_CH_MOUTH_OPEN || i == FC_CH_MOUTH_WIDTH ||
             i == FC_CH_MOUTH_ROUNDNESS || i == FC_CH_AUDIO_ENERGY)) {
            alpha = 65536u;
        }
        c->current[i] = (int16_t)fc_ease_step(c->current[i], c->target[i],
                                              alpha);
    }
}

#if defined(FC_TESTING)
FC_EXPORT(fc_test_blend)
int32_t fc_test_blend(uint32_t op, int32_t acc, int32_t v, int32_t w_q88)
{
    switch (op) {
    case 0: return fc_blend_mix(acc, v, w_q88);
    case 1: return fc_blend_add(acc, v, w_q88);
    case 2: return fc_blend_mul(acc, v, w_q88);
    default: __builtin_trap();
    }
}

FC_EXPORT(fc_test_ease)
int32_t fc_test_ease(int32_t current, int32_t target, uint32_t alpha_q16)
{
    return fc_ease_step(current, target, alpha_q16);
}

FC_EXPORT(fc_test_mouth_resolve)
int32_t fc_test_mouth_resolve(int32_t speech, int32_t gate, int32_t clamp_m,
                              int32_t floor_m)
{
    int32_t m = gate ? speech : 0;
    return fc_mouth_resolve(m, 1, clamp_m, floor_m);
}
#endif
