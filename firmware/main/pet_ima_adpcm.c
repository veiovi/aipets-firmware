#include "pet_ima_adpcm.h"

static const int16_t STEPS[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t INDEX_STEP[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static int16_t decode_nibble(uint8_t nibble, int32_t *predictor, int *index)
{
    int32_t step = STEPS[*index];
    int32_t delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;
    *predictor += (nibble & 8) ? -delta : delta;
    if (*predictor > INT16_MAX) *predictor = INT16_MAX;
    if (*predictor < INT16_MIN) *predictor = INT16_MIN;
    *index += INDEX_STEP[nibble & 7];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return (int16_t)*predictor;
}

size_t pet_ima_adpcm_decode(const uint8_t *frame, size_t length, int16_t *pcm, size_t capacity)
{
    if (!frame || !pcm || length <= PET_IMA_ADPCM_HEADER_BYTES) return 0;
    size_t samples = (length - PET_IMA_ADPCM_HEADER_BYTES) * 2u;
    if (samples > capacity || frame[2] > 88 || frame[3] != 0) return 0;
    int32_t predictor = (int16_t)(frame[0] | (frame[1] << 8));
    int index = frame[2];
    for (size_t sample = 0; sample < samples; sample++) {
        uint8_t byte = frame[PET_IMA_ADPCM_HEADER_BYTES + sample / 2u];
        pcm[sample] = decode_nibble(sample & 1u ? byte >> 4 : byte & 15u, &predictor, &index);
    }
    return samples;
}
