/* Blink and saccade schedulers (lab-ported constants).
 *
 * RNG draw order is ABI for the golden tapes: blink runs before saccade every
 * tick; blink draws 1 value only when an envelope finishes (interval), saccade
 * draws 3 values only when it fires (rx, ry, interval). Under
 * FC_FLAG_REDUCED_MOTION neither scheduler runs and the stream position is
 * unchanged — reduced motion is an input, so determinism is preserved.
 */
#include "fc_internal.h"

/* Blink: interval 2200 + r16*4200/65536 ms (x0.58 when sleepy), triangle
 * envelope over 230 ms (330 sleepy): ramp up to t=0.38, hold to t=0.58, ramp
 * down to t=1. All integer math, mirrored in constants.ts. */
void fc_blink_tick(fc_core_t *c)
{
    uint32_t dur = (c->expr == FC_EXPR_SLEEPY) ? 330u : 230u;

    if (!c->blinking) {
        if ((int32_t)(c->time_ms - c->next_blink_ms) < 0) return;
        c->blinking = 1;
        c->blink_start_ms = c->time_ms;
        c->blink_env = 0;
        return; /* env is 0 at the start tick; first nonzero next tick */
    }

    uint32_t el = c->time_ms - c->blink_start_ms;
    if (el >= dur) {
        c->blinking = 0;
        c->blink_env = 0;
        uint32_t r16 = fc_rng_next(&c->rng) >> 16;
        uint32_t interval = 2200u + (r16 * 4200u) / 65536u;
        if (c->expr == FC_EXPR_SLEEPY) {
            interval = (interval * 38011u) / 65536u; /* x0.58 */
        }
        c->next_blink_ms = c->time_ms + interval;
        return;
    }

    uint32_t up_end = dur * 38u / 100u;   /* 87 (230) / 125 (330) */
    uint32_t hold_end = dur * 58u / 100u; /* 133 (230) / 191 (330) */
    int32_t env;
    if (el < up_end) {
        env = (int32_t)(el * 256u / up_end);
    } else if (el < hold_end) {
        env = 256;
    } else {
        env = (int32_t)((dur - el) * 256u / (dur - hold_end));
    }
    c->blink_env = (int16_t)env;
}

/* Saccade: every 650 + r16*1900/65536 ms pick a gaze target and snap to it.
 * x = (r*2-1)*A, y = (r*1.6-0.8)*A with A = 1.0 thinking else 0.55.
 * Draw order on fire: rx, ry, interval. */
void fc_saccade_tick(fc_core_t *c)
{
    if ((int32_t)(c->time_ms - c->next_saccade_ms) < 0) return;

    uint32_t rx = fc_rng_next(&c->rng) >> 16;
    uint32_t ry = fc_rng_next(&c->rng) >> 16;
    uint32_t ri = fc_rng_next(&c->rng) >> 16;

    int32_t amp = (c->state == FC_STATE_THINKING) ? 256 : 141; /* 1.0 / 0.55 */
    int32_t x01 = (int32_t)(2u * rx) - 65536;                        /* Q16 in [-1,1) */
    int32_t y01 = (int32_t)(((uint64_t)ry * 104858u) >> 16) - 52429; /* Q16 in [-0.8,0.8) */

    c->sacc_x = (int16_t)((x01 * amp) >> 16);
    c->sacc_y = (int16_t)((y01 * amp) >> 16);
    c->next_saccade_ms = c->time_ms + 650u + (ri * 1900u) / 65536u;
}
