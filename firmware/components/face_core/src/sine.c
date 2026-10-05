/* 64-entry quarter-wave Q8.8 sine LUT. Table entries are
 * round(256 * sin((i/64) * pi/2)) — the TS mirror in constants.ts recomputes
 * the identical table. No interpolation: resolution is 1/256 of a turn in
 * phase and <= 1 LSB in amplitude, both invisible on a 120 px canvas. */
#include "fc_internal.h"

static const int16_t fc_sine_quarter[64] = {
      0,   6,  13,  19,  25,  31,  38,  44,
     50,  56,  62,  68,  74,  80,  86,  92,
     98, 104, 109, 115, 121, 126, 132, 137,
    142, 147, 152, 157, 162, 167, 172, 177,
    181, 185, 190, 194, 198, 202, 206, 209,
    213, 216, 220, 223, 226, 229, 231, 234,
    237, 239, 241, 243, 245, 247, 248, 250,
    251, 252, 253, 254, 255, 255, 256, 256,
};

int32_t fc_sin_q88(uint16_t phase)
{
    uint32_t quadrant = (uint32_t)(phase >> 14);      /* 0..3 */
    uint32_t idx = (uint32_t)((phase >> 8) & 0x3Fu);  /* 0..63 within quadrant */
    int32_t v;
    switch (quadrant) {
    case 0: v = fc_sine_quarter[idx]; break;
    case 1: v = (idx == 0) ? 256 : fc_sine_quarter[64 - idx]; break;
    case 2: v = -fc_sine_quarter[idx]; break;
    default: v = (idx == 0) ? -256 : -fc_sine_quarter[64 - idx]; break;
    }
    return v;
}

#if defined(FC_TESTING)
FC_EXPORT(fc_test_sin_q88)
int32_t fc_test_sin_q88(uint32_t phase)
{
    return fc_sin_q88((uint16_t)phase);
}
#endif
