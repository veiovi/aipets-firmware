#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pet_ai.h"

#define PET_PCM_GAIN_Q15_3_DB 46341U
#define PET_PCM_GAIN_Q15_6_DB 65536U
#define PET_PCM_GAIN_Q15_9_DB 92682U
#define PET_PCM_LIMITER_THRESHOLD 28000U
#define PET_PCM_LIMITER_HEADROOM 4767U

typedef struct {
    uint64_t input_square_sum;
    uint64_t output_square_sum;
    uint64_t sample_count;
    uint64_t limited_samples;
    uint32_t input_peak;
    uint32_t output_peak;
} pet_pcm_gain_stats_t;

void pet_pcm_gain_stats_reset(pet_pcm_gain_stats_t *stats);
void pet_pcm_gain_apply(int16_t *pcm, size_t samples, pet_realtime_boost_t boost,
                        pet_pcm_gain_stats_t *stats);
uint32_t pet_pcm_gain_rms(uint64_t square_sum, uint64_t sample_count);
