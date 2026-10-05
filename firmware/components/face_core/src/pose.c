/* Data tables. Every Q8.8 literal follows the rule q = Math.round(x * 256)
 * (JS round-half-up). constants.ts recomputes all of these from the float
 * literals — a transcription error here fails presentation.test.ts. */
#include "fc_internal.h"

#define M(ch) ((uint8_t)(1u << (ch)))

const fc_emotion_t fc_emotions[FC_EXPR_COUNT] = {
    /* NEUTRAL */
    { .mask = 0, .v = {0}, .mouth_lerp = -1, .mouth_floor = 0 },
    /* HAPPY: eyeOpen .82, smile 1, cheek .92 */
    { .mask = M(FC_CH_EYE_OPEN) | M(FC_CH_SMILE) | M(FC_CH_CHEEK),
      .v = { [FC_CH_EYE_OPEN] = 210, [FC_CH_SMILE] = 256, [FC_CH_CHEEK] = 236 },
      .mouth_lerp = -1, .mouth_floor = 0 },
    /* CURIOUS: eyeOpen 1.08, smile .28, browTilt .85, headTilt -.8 */
    { .mask = M(FC_CH_EYE_OPEN) | M(FC_CH_SMILE) | M(FC_CH_BROW_TILT) | M(FC_CH_HEAD_TILT),
      .v = { [FC_CH_EYE_OPEN] = 276, [FC_CH_SMILE] = 72,
             [FC_CH_BROW_TILT] = 218, [FC_CH_HEAD_TILT] = -205 },
      .mouth_lerp = -1, .mouth_floor = 0 },
    /* SURPRISED: eyeOpen 1.26, pupil .78, smile 0, browTilt -.2, mouth lerp .78 */
    { .mask = M(FC_CH_EYE_OPEN) | M(FC_CH_PUPIL_SCALE) | M(FC_CH_SMILE) | M(FC_CH_BROW_TILT),
      .v = { [FC_CH_EYE_OPEN] = 323, [FC_CH_PUPIL_SCALE] = 200,
             [FC_CH_SMILE] = 0, [FC_CH_BROW_TILT] = -51 },
      .mouth_lerp = 200, .mouth_floor = 0 },
    /* SLEEPY: eyeOpen .34, pupil .95, smile .3, cheek .08, headTilt .28 */
    { .mask = M(FC_CH_EYE_OPEN) | M(FC_CH_PUPIL_SCALE) | M(FC_CH_SMILE) |
              M(FC_CH_CHEEK) | M(FC_CH_HEAD_TILT),
      .v = { [FC_CH_EYE_OPEN] = 87, [FC_CH_PUPIL_SCALE] = 243, [FC_CH_SMILE] = 77,
             [FC_CH_CHEEK] = 20, [FC_CH_HEAD_TILT] = 72 },
      .mouth_lerp = -1, .mouth_floor = 0 },
    /* CONCERNED: eyeOpen .86, smile -.45, browTilt -.85, cheek .05 */
    { .mask = M(FC_CH_EYE_OPEN) | M(FC_CH_SMILE) | M(FC_CH_BROW_TILT) | M(FC_CH_CHEEK),
      .v = { [FC_CH_EYE_OPEN] = 220, [FC_CH_SMILE] = -115,
             [FC_CH_BROW_TILT] = -218, [FC_CH_CHEEK] = 13 },
      .mouth_lerp = -1, .mouth_floor = 0 },
    /* EXCITED: eyeOpen 1.18, pupil 1.12, smile 1, cheek 1, mouth floor .36 */
    { .mask = M(FC_CH_EYE_OPEN) | M(FC_CH_PUPIL_SCALE) | M(FC_CH_SMILE) | M(FC_CH_CHEEK),
      .v = { [FC_CH_EYE_OPEN] = 302, [FC_CH_PUPIL_SCALE] = 287,
             [FC_CH_SMILE] = 256, [FC_CH_CHEEK] = 256 },
      .mouth_lerp = -1, .mouth_floor = 92 },
    /* SHY: eyeOpen .72, pupil .95, smile .62, cheek 1, headTilt .44 */
    { .mask = M(FC_CH_EYE_OPEN) | M(FC_CH_PUPIL_SCALE) | M(FC_CH_SMILE) |
              M(FC_CH_CHEEK) | M(FC_CH_HEAD_TILT),
      .v = { [FC_CH_EYE_OPEN] = 184, [FC_CH_PUPIL_SCALE] = 243, [FC_CH_SMILE] = 159,
             [FC_CH_CHEEK] = 256, [FC_CH_HEAD_TILT] = 113 },
      .mouth_lerp = -1, .mouth_floor = 0 },
};

/* Per-channel easing alphas at the fixed 33 ms step.
 * k=11/s -> 19989, k=18/s (mouth) -> 29360, k=24/s reflex -> 35849;
 * direct input/scheduler channels snap. */
const uint32_t fc_alpha_q16[FC_CHANNEL_COUNT] = {
    [FC_CH_EYE_OPEN] = 19989,
    [FC_CH_PUPIL_SCALE] = 19989,
    [FC_CH_MOUTH_OPEN] = 29360,
    [FC_CH_SMILE] = 19989,
    [FC_CH_BROW_TILT] = 19989,
    [FC_CH_HEAD_TILT] = 19989,
    [FC_CH_CHEEK] = 19989,
    [FC_CH_GAZE_X] = 65536,
    [FC_CH_GAZE_Y] = 65536,
    [FC_CH_BREATH] = 65536,
    [FC_CH_BLINK] = 65536,
    [FC_CH_EYE_OPEN_LEFT] = 19989,
    [FC_CH_EYE_OPEN_RIGHT] = 19989,
    [FC_CH_MOUTH_WIDTH] = 29360,
    [FC_CH_MOUTH_ROUNDNESS] = 29360,
    [FC_CH_BROW_LEFT] = 19989,
    [FC_CH_BROW_RIGHT] = 19989,
    [FC_CH_HEAD_X] = 19989,
    [FC_CH_HEAD_Y] = 19989,
    [FC_CH_HEAD_SCALE_X] = 35849,
    [FC_CH_HEAD_SCALE_Y] = 35849,
    [FC_CH_TOUCH_X] = 65536,
    [FC_CH_TOUCH_Y] = 65536,
    [FC_CH_TOUCH_PRESS] = 65536,
    [FC_CH_TOUCH_SQUASH] = 35849,
    [FC_CH_TILT_X] = 65536,
    [FC_CH_TILT_Y] = 65536,
    [FC_CH_ACCESSORY_MOTION] = 65536,
    [FC_CH_AUDIO_ENERGY] = 29360,
    [FC_CH_STATUS_ENERGY] = 19989,
    [30] = 65536, [31] = 65536,
};

/* Final clamp bounds. Reserved channels are pinned to 0. */
const int16_t fc_clamp_min[FC_CHANNEL_COUNT] = {
    [FC_CH_EYE_OPEN] = 10,     /* 0.04 floor: closed-lid line stays visible */
    [FC_CH_PUPIL_SCALE] = 64,
    [FC_CH_MOUTH_OPEN] = 0,
    [FC_CH_SMILE] = -256,
    [FC_CH_BROW_TILT] = -256,
    [FC_CH_HEAD_TILT] = -256,
    [FC_CH_CHEEK] = 0,
    [FC_CH_GAZE_X] = -256,
    [FC_CH_GAZE_Y] = -256,
    [FC_CH_BREATH] = -256,
    [FC_CH_BLINK] = 0,
    [FC_CH_EYE_OPEN_LEFT] = 10,
    [FC_CH_EYE_OPEN_RIGHT] = 10,
    [FC_CH_MOUTH_WIDTH] = 0,
    [FC_CH_MOUTH_ROUNDNESS] = 0,
    [FC_CH_BROW_LEFT] = -256,
    [FC_CH_BROW_RIGHT] = -256,
    [FC_CH_HEAD_X] = -256,
    [FC_CH_HEAD_Y] = -256,
    [FC_CH_HEAD_SCALE_X] = 128,
    [FC_CH_HEAD_SCALE_Y] = 128,
    [FC_CH_TOUCH_X] = -256,
    [FC_CH_TOUCH_Y] = -256,
    [FC_CH_TOUCH_PRESS] = 0,
    [FC_CH_TOUCH_SQUASH] = 0,
    [FC_CH_TILT_X] = -256,
    [FC_CH_TILT_Y] = -256,
    [FC_CH_ACCESSORY_MOTION] = -256,
    [FC_CH_AUDIO_ENERGY] = 0,
    [FC_CH_STATUS_ENERGY] = 0,
    [FC_CH_GESTURE_SPIN] = -256,
    [FC_CH_GESTURE_ENERGY] = 0,
};

const int16_t fc_clamp_max[FC_CHANNEL_COUNT] = {
    [FC_CH_EYE_OPEN] = 333,    /* 1.3 */
    [FC_CH_PUPIL_SCALE] = 384,
    [FC_CH_MOUTH_OPEN] = 256,
    [FC_CH_SMILE] = 256,
    [FC_CH_BROW_TILT] = 256,
    [FC_CH_HEAD_TILT] = 256,
    [FC_CH_CHEEK] = 256,
    [FC_CH_GAZE_X] = 256,
    [FC_CH_GAZE_Y] = 256,
    [FC_CH_BREATH] = 256,
    [FC_CH_BLINK] = 256,
    [FC_CH_EYE_OPEN_LEFT] = 333,
    [FC_CH_EYE_OPEN_RIGHT] = 333,
    [FC_CH_MOUTH_WIDTH] = 256,
    [FC_CH_MOUTH_ROUNDNESS] = 256,
    [FC_CH_BROW_LEFT] = 256,
    [FC_CH_BROW_RIGHT] = 256,
    [FC_CH_HEAD_X] = 256,
    [FC_CH_HEAD_Y] = 256,
    [FC_CH_HEAD_SCALE_X] = 384,
    [FC_CH_HEAD_SCALE_Y] = 384,
    [FC_CH_TOUCH_X] = 256,
    [FC_CH_TOUCH_Y] = 256,
    [FC_CH_TOUCH_PRESS] = 256,
    [FC_CH_TOUCH_SQUASH] = 256,
    [FC_CH_TILT_X] = 256,
    [FC_CH_TILT_Y] = 256,
    [FC_CH_ACCESSORY_MOTION] = 256,
    [FC_CH_AUDIO_ENERGY] = 256,
    [FC_CH_STATUS_ENERGY] = 256,
    [FC_CH_GESTURE_SPIN] = 256,
    [FC_CH_GESTURE_ENERGY] = 256,
};
