#include "pet_pcm_gain.h"

#include <stdbool.h>
#include <string.h>

static uint32_t boost_gain_q15(pet_realtime_boost_t boost)
{
    switch (boost) {
        case PET_REALTIME_BOOST_3_DB: return PET_PCM_GAIN_Q15_3_DB;
        case PET_REALTIME_BOOST_6_DB: return PET_PCM_GAIN_Q15_6_DB;
        case PET_REALTIME_BOOST_9_DB: return PET_PCM_GAIN_Q15_9_DB;
        case PET_REALTIME_BOOST_OFF:
        default: return 32768U;
    }
}

static uint32_t sample_magnitude(int16_t sample)
{
    return sample < 0 ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
}

static int16_t boost_sample(int16_t sample, uint32_t gain_q15, bool *limited)
{
    uint32_t magnitude = sample_magnitude(sample);
    /* PCM16 magnitude times the largest Q15 gain remains within uint32_t. */
    uint32_t boosted = (magnitude * gain_q15 + 16384U) >> 15;
    if (boosted > PET_PCM_LIMITER_THRESHOLD) {
        uint32_t delta = boosted - PET_PCM_LIMITER_THRESHOLD;
        boosted = PET_PCM_LIMITER_THRESHOLD +
                  (delta * PET_PCM_LIMITER_HEADROOM) /
                  (delta + PET_PCM_LIMITER_HEADROOM);
        *limited = true;
    }
    if (boosted > INT16_MAX) boosted = INT16_MAX;
    return sample < 0 ? (int16_t)-(int32_t)boosted : (int16_t)boosted;
}

void pet_pcm_gain_stats_reset(pet_pcm_gain_stats_t *stats)
{
    if (stats) memset(stats, 0, sizeof(*stats));
}

void pet_pcm_gain_apply(int16_t *pcm, size_t samples, pet_realtime_boost_t boost,
                        pet_pcm_gain_stats_t *stats)
{
    if (!pcm || !samples) return;
    bool bypass = boost <= PET_REALTIME_BOOST_OFF || boost >= PET_REALTIME_BOOST_COUNT;
    uint32_t gain_q15 = boost_gain_q15(boost);
    for (size_t i = 0; i < samples; ++i) {
        int16_t input = pcm[i];
        uint32_t input_magnitude = sample_magnitude(input);
        bool limited = false;
        int16_t output = bypass ? input : boost_sample(input, gain_q15, &limited);
        uint32_t output_magnitude = sample_magnitude(output);
        pcm[i] = output;
        if (stats) {
            stats->input_square_sum += input_magnitude * input_magnitude;
            stats->output_square_sum += output_magnitude * output_magnitude;
            stats->sample_count++;
            if (limited) stats->limited_samples++;
            if (input_magnitude > stats->input_peak) stats->input_peak = input_magnitude;
            if (output_magnitude > stats->output_peak) stats->output_peak = output_magnitude;
        }
    }
}

static uint32_t integer_sqrt_u64(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
}

uint32_t pet_pcm_gain_rms(uint64_t square_sum, uint64_t sample_count)
{
    return sample_count ? integer_sqrt_u64(square_sum / sample_count) : 0;
}
