/* xorshift32. One stream per core; consumers draw in fixed order (blink, then
 * saccade) and only when their scheduler fires, so the stream position is a
 * pure function of the input history. */
#include "fc_internal.h"

uint32_t fc_rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

#if defined(FC_TESTING)
FC_EXPORT(fc_test_xorshift32)
uint32_t fc_test_xorshift32(uint32_t s)
{
    return fc_rng_next(&s);
}
#endif
