/*
 * face_core — portable, freestanding animation core for AI Pet faces.
 *
 * Rules of this component (enforced by the wasm harness build):
 *   - no floats anywhere
 *   - no libc beyond compiler builtins (memcpy/memset provided in support.c)
 *   - no LVGL / FreeRTOS / ESP-IDF headers
 *   - no heap: all state lives in a caller-provided arena
 *   - fixed 33 ms timestep; all dynamics are deterministic given the seed
 *
 * Fixed point: Q8.8 throughout (256 == 1.0), int16 storage, int32 arithmetic.
 * Rounding convention: (x * w + 128) >> 8 and (d * alpha + 32768) >> 16 with
 * arithmetic right shift (floor). The TypeScript reference mirrors this with
 * Math.floor((x * w + 128) / 256) — keep the two in lockstep.
 *
 * One accepted deviation from the pixel-face-lab JS reference: sine comes from
 * a 64-entry quarter-wave Q8.8 LUT (sine.c), not Math.sin. Golden tapes are
 * C-core goldens, not lab-parity goldens.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasm__)
#define FC_EXPORT(sym) __attribute__((export_name(#sym)))
#else
#define FC_EXPORT(sym)
#endif

typedef int16_t fc_q88_t; /* Q8.8: 256 == 1.0 */

enum {
    FC_CHANNEL_COUNT = 32,
    FC_TICK_MS = 33,
};

/* Channel order is an INTERNAL ABI for host fixtures and firmware adapters:
 * APPEND-ONLY. Face packs bind to semantic channel names at compile time and
 * must not persist these numeric values as their public compatibility API. */
enum {
    FC_CH_EYE_OPEN = 0,   /* 0.04 .. 1.3  */
    FC_CH_PUPIL_SCALE,    /* ~0.78 .. 1.12 */
    FC_CH_MOUTH_OPEN,     /* 0 .. 1 */
    FC_CH_SMILE,          /* -1 frown .. +1 */
    FC_CH_BROW_TILT,      /* -1 .. 1 */
    FC_CH_HEAD_TILT,      /* -1 .. 1 */
    FC_CH_CHEEK,          /* 0 .. 1 */
    FC_CH_GAZE_X,         /* -1 .. 1, snap (no easing) */
    FC_CH_GAZE_Y,         /* -1 .. 1, snap */
    FC_CH_BREATH,         /* -0.65 .. 0.65, raw oscillator, snap */
    FC_CH_BLINK,          /* 0 .. 1, raw triangle envelope, snap */
    FC_CH_EYE_OPEN_LEFT,  /* resolved left-eye openness */
    FC_CH_EYE_OPEN_RIGHT, /* resolved right-eye openness */
    FC_CH_MOUTH_WIDTH,    /* 0 .. 1 semantic mouth width */
    FC_CH_MOUTH_ROUNDNESS,/* 0 .. 1 wide-to-round mouth blend */
    FC_CH_BROW_LEFT,      /* -1 .. 1 left brow attitude */
    FC_CH_BROW_RIGHT,     /* -1 .. 1 right brow attitude */
    FC_CH_HEAD_X,         /* -1 .. 1 normalized head translation */
    FC_CH_HEAD_Y,         /* -1 .. 1 normalized head translation */
    FC_CH_HEAD_SCALE_X,   /* 1 == neutral */
    FC_CH_HEAD_SCALE_Y,   /* 1 == neutral */
    FC_CH_TOUCH_X,        /* latest normalized touch x */
    FC_CH_TOUCH_Y,        /* latest normalized touch y */
    FC_CH_TOUCH_PRESS,    /* 0 .. 1 current pressure/down state */
    FC_CH_TOUCH_SQUASH,   /* 0 .. 1 press/tap reflex envelope */
    FC_CH_TILT_X,         /* -1 .. 1 filtered IMU/user tilt input */
    FC_CH_TILT_Y,         /* -1 .. 1 filtered IMU/user tilt input */
    FC_CH_ACCESSORY_MOTION, /* -1 .. 1 secondary-motion driver */
    FC_CH_AUDIO_ENERGY,   /* 0 .. 1 normalized playback energy */
    FC_CH_STATUS_ENERGY,  /* 0 .. 1 operational-state activity */
    FC_CH_GESTURE_SPIN,   /* whole-face rotation in TURNS, -1 .. 1 (256 == 360°).
                           * Adapters apply it modulo one turn as a rotation of
                           * the final framebuffer; 0 when no gesture is active. */
    FC_CH_GESTURE_ENERGY, /* 0 .. 1 activity envelope of the running gesture;
                           * packs may bind accents to it. 0 when idle. */
};

/* Values mirror pet_face_state_t order in firmware/main/pet_face.h exactly. */
typedef uint8_t fc_state_t;
enum {
    FC_STATE_BOOTING = 0,
    FC_STATE_PROVISIONING,
    FC_STATE_CONNECTING,
    FC_STATE_IDLE,
    FC_STATE_LISTENING,
    FC_STATE_THINKING,
    FC_STATE_SPEAKING,
    FC_STATE_OFFLINE,
    FC_STATE_ERROR,
    FC_STATE_COUNT,
};

/* Values mirror pet_expression_t order in firmware/main/pet_face.h exactly. */
typedef uint8_t fc_expression_t;
enum {
    FC_EXPR_NEUTRAL = 0,
    FC_EXPR_HAPPY,
    FC_EXPR_CURIOUS,
    FC_EXPR_SURPRISED,
    FC_EXPR_SLEEPY,
    FC_EXPR_CONCERNED,
    FC_EXPR_EXCITED,
    FC_EXPR_SHY,
    FC_EXPR_COUNT,
};

enum {
    FC_FLAG_REDUCED_MOTION = 1u << 0,
    /* Speech has already passed a playback-aligned attack/release envelope.
     * Mouth/audio channels snap so FaceCore does not add a second lag. */
    FC_FLAG_EXTERNAL_SPEECH_ENVELOPE = 1u << 1,
};

/* Universal one-shot gestures every face can perform. They write only to
 * whole-face channels (gesture spin/energy, head offsets, head scale), so any
 * renderer or pack gets them for free. Under FC_FLAG_REDUCED_MOTION the
 * rotational gestures downgrade to POP and all amplitudes halve. */
typedef uint8_t fc_gesture_t;
enum {
    FC_GESTURE_NONE = 0,
    FC_GESTURE_SPIN_CW,   /* full 360° spin, eased in/out      (~1.0 s) */
    FC_GESTURE_SPIN_CCW,  /* full 360° spin, other way         (~1.0 s) */
    FC_GESTURE_SHAKE,     /* decaying head shake — "no"/deny   (~0.6 s) */
    FC_GESTURE_NOD,       /* two downward nods — "yes"/confirm (~0.66 s) */
    FC_GESTURE_ZOOM_IN,   /* slow push-in, hold, release       (~1.3 s) */
    FC_GESTURE_HEARTBEAT, /* two quick scale thumps — affection (~0.8 s) */
    FC_GESTURE_BOUNCE,    /* retired compatibility ID; remaps to HEARTBEAT */
    FC_GESTURE_WOBBLE,    /* decaying rotary wobble — dizzy    (~1.2 s) */
    FC_GESTURE_POP,       /* quick scale overshoot — acknowledge (~0.26 s) */
    FC_GESTURE_COUNT,
};

typedef struct fc_core fc_core_t; /* opaque; lives entirely in the caller arena */

/* Bytes the caller must provide, 8-aligned. */
uint32_t fc_arena_size(void);

/* NULL if the arena is too small or not 8-aligned. On success the core is
 * ready: presentation = idle/neutral, live pose preloaded to the composed
 * neutral-idle base so the first ticks ease from a sensible face. */
fc_core_t *fc_init(void *arena, uint32_t arena_bytes);

/* Seed 0 remaps to 0xA5F0C3D1. Resets virtual time, tick count, oscillator
 * phases, and blink/saccade schedules — call once right after fc_init. */
void fc_seed(fc_core_t *c, uint32_t seed);

/* Sticky latest-wins inputs; fc_tick consumes them. Operational state is
 * applied on the next tick. Expression/intensity changes after the first are
 * subject to an 8-tick (264 ms) dwell; a change during dwell is queued and
 * latest-wins. Re-setting the active expression does not reset the dwell. */
void fc_set_state(fc_core_t *c, uint8_t state);
void fc_set_expression(fc_core_t *c, uint8_t expression, fc_q88_t intensity);
/* Compatibility convenience for existing callers. Equivalent to calling the
 * two setters above; state still remains immediate and expression still dwells. */
void fc_set_presentation(fc_core_t *c, uint8_t state, uint8_t expression, fc_q88_t intensity);
void fc_set_flags(fc_core_t *c, uint32_t flags);
/* Raw speech level 0..256 (Q8.8). Until the envelope module lands this feeds
 * the mouth directly: target = min(mouthClamp, max(speech*gate, mouthFloor)). */
void fc_set_speech(fc_core_t *c, fc_q88_t level);
/* Normalized Q8.8 inputs. x/y clamp to [-256, 256], pressure to [0, 256].
 * Pressure > 0 overrides ambient gaze with touch gaze. */
void fc_set_touch(fc_core_t *c, fc_q88_t x, fc_q88_t y, fc_q88_t pressure);
void fc_trigger_tap(fc_core_t *c);
void fc_set_tilt(fc_core_t *c, fc_q88_t x, fc_q88_t y);

/* Start a universal gesture (latest wins; retriggering restarts). Unknown ids
 * are ignored. The caller layer pairs this with its sound cue — the core
 * stays silent by design. fc_active_gesture returns FC_GESTURE_NONE when the
 * envelope has finished, which is the app's cue that the face is quiet again. */
void fc_trigger_gesture(fc_core_t *c, uint8_t gesture);
uint8_t fc_active_gesture(const fc_core_t *c);

/* Advance exactly one 33 ms step. */
void fc_tick(fc_core_t *c);

/* int16[FC_CHANNEL_COUNT], stable address for the core's lifetime. */
const int16_t *fc_pose(const fc_core_t *c);

/* Accepted presentation after next-tick state application and expression
 * dwell. Adapters must use these values for one-hot state/expression bindings
 * instead of the latest requested inputs. */
uint8_t fc_active_state(const fc_core_t *c);
uint8_t fc_active_expression(const fc_core_t *c);
fc_q88_t fc_active_expression_intensity(const fc_core_t *c);

uint32_t fc_tick_count(const fc_core_t *c);

#ifdef __cplusplus
}
#endif
