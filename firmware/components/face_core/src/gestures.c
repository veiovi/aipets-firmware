/* Universal one-shot gestures.
 *
 * Every gesture is a fixed-timestep envelope over whole-face channels only:
 * FC_CH_GESTURE_SPIN (turns), FC_CH_GESTURE_ENERGY, and additive deltas on
 * FC_CH_HEAD_X/Y and FC_CH_HEAD_SCALE_X/Y. Faces need zero per-character work
 * to support them, and an idle engine is a strict identity — composed targets
 * are bit-identical to a build without this file, so existing golden tapes
 * are unaffected until a gesture is actually triggered.
 *
 * Reduced motion: SPIN_CW/SPIN_CCW/WOBBLE downgrade to POP at trigger time
 * (a full rotation under a motion-sensitivity setting is exactly what the
 * setting exists to prevent); every remaining amplitude is halved.
 *
 * Sound: the core is freestanding and silent. The application layer pairs
 * fc_trigger_gesture with its pet_sfx cue.
 */
#include "fc_internal.h"

static const uint16_t fc_gesture_dur[FC_GESTURE_COUNT] = {
    [FC_GESTURE_NONE] = 0,
    [FC_GESTURE_SPIN_CW] = 30,   /* 990 ms */
    [FC_GESTURE_SPIN_CCW] = 30,
    [FC_GESTURE_SHAKE] = 18,     /* 594 ms */
    [FC_GESTURE_NOD] = 20,       /* 660 ms */
    [FC_GESTURE_ZOOM_IN] = 40,   /* 1320 ms */
    [FC_GESTURE_HEARTBEAT] = 24, /* 792 ms */
    [FC_GESTURE_BOUNCE] = 22,    /* 726 ms */
    [FC_GESTURE_WOBBLE] = 36,    /* 1188 ms */
    [FC_GESTURE_POP] = 8,        /* 264 ms */
};

/* 3x^2 - 2x^3 for x in Q8.8 [0, 256] — monotone ease-in-out. */
static int32_t fc_smoothstep_q88(int32_t x)
{
    if (x <= 0) return 0;
    if (x >= 256) return 256;
    int32_t s = (x * x + 128) >> 8;              /* x^2, Q8.8 */
    return (s * (768 - 2 * x) + 128) >> 8;       /* x^2 * (3 - 2x) */
}

/* half-sine bell over p in [0, 256]: 0 -> 1 -> 0 */
static int32_t fc_bell_q88(int32_t p)
{
    if (p <= 0 || p >= 256) return 0;
    return fc_sin_q88((uint16_t)(p << 7)); /* 0..256 -> 0..pi */
}

static int32_t fc_iabs32(int32_t v) { return v < 0 ? -v : v; }

FC_EXPORT(fc_trigger_gesture)
void fc_trigger_gesture(fc_core_t *c, uint8_t gesture)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    if (gesture == FC_GESTURE_NONE || gesture >= FC_GESTURE_COUNT) return;
    /* Permanently retire the squeeze/bounce effect while preserving its stable
     * protocol ID for older gateways and stored sequences. */
    if (gesture == FC_GESTURE_BOUNCE) gesture = FC_GESTURE_HEARTBEAT;
    if (c->flags & FC_FLAG_REDUCED_MOTION) {
        if (gesture == FC_GESTURE_SPIN_CW || gesture == FC_GESTURE_SPIN_CCW ||
            gesture == FC_GESTURE_WOBBLE) {
            gesture = FC_GESTURE_POP;
        }
    }
    c->gesture = gesture;   /* latest wins; retrigger restarts */
    c->gesture_tick = 0;
}

FC_EXPORT(fc_active_gesture)
uint8_t fc_active_gesture(const fc_core_t *c)
{
    FC_ASSERT(c != NULL && c->magic == FC_MAGIC);
    return c->gesture;
}

void fc_gesture_step(fc_core_t *c)
{
    if (c->gesture == FC_GESTURE_NONE) return;
    c->gesture_tick++;
    if (c->gesture_tick >= fc_gesture_dur[c->gesture]) {
        c->gesture = FC_GESTURE_NONE;
        c->gesture_tick = 0;
    }
}

void fc_gesture_apply(fc_core_t *c, int32_t t[FC_CHANNEL_COUNT])
{
    if (c->gesture == FC_GESTURE_NONE) return;

    const int32_t tick = c->gesture_tick;
    const int32_t dur = fc_gesture_dur[c->gesture];
    const int32_t p = tick * 256 / (dur - 1);          /* progress, Q8.8 0..256 */
    /* reduced motion halves every surviving amplitude */
    const int32_t amp = (c->flags & FC_FLAG_REDUCED_MOTION) ? 128 : 256;
#define A(v) (((v) * amp) >> 8)

    int32_t spin = 0, energy = 0, dx = 0, dy = 0, dsx = 0, dsy = 0;

    switch (c->gesture) {
    case FC_GESTURE_SPIN_CW:
    case FC_GESTURE_SPIN_CCW: {
        /* eased full turn; 256 turns == 360° == 0°, so the hand-off back to
         * the idle value of 0 on the next tick is visually seamless */
        int32_t e = fc_smoothstep_q88(p);
        spin = (c->gesture == FC_GESTURE_SPIN_CW) ? e : -e;
        energy = fc_bell_q88(p);
        break;
    }
    case FC_GESTURE_SHAKE: {
        int32_t decay = 256 - p;
        uint32_t r = fc_rng_next(&c->gesture_rng);
        int32_t mag = 90 + (int32_t)(r & 31u);          /* 0.35..0.47 */
        dx = ((tick & 1) ? mag : -mag) * decay >> 8;
        int32_t vmag = 20 + (int32_t)((r >> 8) & 15u);
        dy = (((r >> 16) & 1u) ? vmag : -vmag) * decay >> 8;
        energy = decay;
        break;
    }
    case FC_GESTURE_NOD: {
        /* two downward humps: |sin| through two full cycles */
        int32_t s = fc_sin_q88((uint16_t)((p * 512) & 0xFFFF));
        dy = (fc_iabs32(s) * 70 + 128) >> 8;            /* 0..0.27 down */
        energy = fc_bell_q88(p);
        break;
    }
    case FC_GESTURE_ZOOM_IN: {
        int32_t e;
        if (tick < 12) e = fc_smoothstep_q88(tick * 256 / 11);
        else if (tick < 28) e = 256;
        else e = 256 - fc_smoothstep_q88((tick - 28) * 256 / 11);
        dsx = (77 * e + 128) >> 8;                      /* up to 1.30x */
        dsy = dsx;
        energy = e;
        break;
    }
    case FC_GESTURE_HEARTBEAT: {
        int32_t e = 0;
        if (tick < 8) e = fc_bell_q88(tick * 256 / 7);
        else if (tick >= 10 && tick < 18) e = fc_bell_q88((tick - 10) * 256 / 7);
        dsx = (31 * e + 128) >> 8;                      /* up to 1.12x */
        dsy = dsx;
        energy = e;
        break;
    }
    case FC_GESTURE_WOBBLE: {
        /* three decaying rotary oscillations, max ~±20° */
        int32_t decay = 256 - p;
        int32_t s = fc_sin_q88((uint16_t)((tick * 5461) & 0xFFFF));
        spin = ((s * 14 + 128) >> 8) * decay >> 8;
        energy = decay;
        break;
    }
    case FC_GESTURE_POP: {
        int32_t e = fc_bell_q88(p);
        dsx = (38 * e + 128) >> 8;                      /* up to 1.15x */
        dsy = dsx;
        energy = e;
        break;
    }
    default:
        return;
    }

    t[FC_CH_GESTURE_SPIN] = A(spin);
    t[FC_CH_GESTURE_ENERGY] = energy;                   /* not halved: it is a signal */
    t[FC_CH_HEAD_X] += A(dx);
    t[FC_CH_HEAD_Y] += A(dy);
    t[FC_CH_HEAD_SCALE_X] += A(dsx);
    t[FC_CH_HEAD_SCALE_Y] += A(dsy);
#undef A
}
