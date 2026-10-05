#include "pet_speech_mouth.h"

static uint32_t integer_sqrt(uint64_t value)
{
    uint64_t bit = (uint64_t)1 << 62;
    uint64_t result = 0;
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

static uint32_t average_absolute(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < samples; ++i) {
        total += pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
    }
    return (uint32_t)(total / samples);
}

bool pet_speech_mouth_mode_valid(pet_speech_mouth_mode_t mode)
{
    return mode >= PET_SPEECH_MOUTH_CURRENT_SPRITES &&
           mode < PET_SPEECH_MOUTH_MODE_COUNT;
}

const char *pet_speech_mouth_mode_name(pet_speech_mouth_mode_t mode)
{
    switch (mode) {
    case PET_SPEECH_MOUTH_CURRENT_SPRITES: return "current-sprites";
    case PET_SPEECH_MOUTH_FULL_RANGE_SPRITES: return "full-range-sprites";
    case PET_SPEECH_MOUTH_SYNCED_SPRITES: return "synced-sprites";
    case PET_SPEECH_MOUTH_SMOOTH_24_STEP: return "smooth-24-step";
    case PET_SPEECH_MOUTH_CLASSIC_SHAPE: return "classic-shape";
    case PET_SPEECH_MOUTH_EXPRESSIVE_SHAPE: return "expressive-shape";
    default: return "current-sprites";
    }
}

static bool normalized_equal(const char *left, const char *right)
{
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a == '_' || a == ' ') a = '-';
        if (b == '_' || b == ' ') b = '-';
        if (a != b) return false;
    }
    return *left == 0 && *right == 0;
}

bool pet_speech_mouth_mode_parse(const char *value, pet_speech_mouth_mode_t *mode)
{
    if (!value || !mode) return false;
    if (normalized_equal(value, "authored-7-stage") ||
        normalized_equal(value, "character-authored")) {
        *mode = PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED;
        return true;
    }
    if (value[0] >= '0' && value[0] <= '5' && value[1] == 0) {
        *mode = (pet_speech_mouth_mode_t)(value[0] - '0');
        return true;
    }
    for (pet_speech_mouth_mode_t candidate = PET_SPEECH_MOUTH_CURRENT_SPRITES;
         candidate < PET_SPEECH_MOUTH_MODE_COUNT; candidate++) {
        if (normalized_equal(value, pet_speech_mouth_mode_name(candidate))) {
            *mode = candidate;
            return true;
        }
    }
    return false;
}

pet_speech_mouth_mode_t pet_speech_mouth_full_frame_normalize(
    pet_speech_mouth_mode_t legacy_mode)
{
    (void)legacy_mode;
    /* The full-frame pack owns stage count, artwork, filtering, hysteresis,
     * and minimum hold. Old numeric settings are accepted but must not select obsolete sprite,
     * shape, or 24-step renderer behavior. */
    return PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED;
}

uint8_t pet_speech_mouth_current_level(const int16_t *pcm, size_t samples)
{
    uint32_t average = average_absolute(pcm, samples);
    uint32_t level = average > 64 ? (average - 64) / 8 : 0;
    return level > 255 ? 255 : (uint8_t)level;
}

uint8_t pet_speech_mouth_full_range_level(const int16_t *pcm, size_t samples)
{
    uint32_t historical = average_absolute(pcm, samples) / 24;
    if (historical > 100) historical = 100;
    return (uint8_t)((historical * 255u + 50u) / 100u);
}

void pet_speech_mouth_envelope_reset(pet_speech_mouth_envelope_t *state,
                                     pet_speech_mouth_mode_t mode,
                                     uint32_t sample_rate)
{
    if (!state) return;
    uint8_t *bytes = (uint8_t *)state;
    for (size_t i = 0; i < sizeof(*state); i++) bytes[i] = 0;
    state->mode = pet_speech_mouth_mode_valid(mode) ? mode : PET_SPEECH_MOUTH_DEFAULT;
    state->sample_rate = sample_rate ? sample_rate : 24000;
    state->minimum = 255;
}

/* Seven calibrated dBFS anchors: -48, -42, -36, -30, -24, -18, -12.
 * The output curve deliberately keeps quiet speech nearly closed and spends
 * most visual travel on ordinary/strong syllables. */
static uint8_t rms_to_target(uint32_t rms)
{
    static const uint16_t threshold[] = {130, 260, 519, 1036, 2068, 4125, 8231};
    static const uint8_t output[] = {0, 4, 14, 32, 64, 128, 255};
    if (rms < threshold[0]) return 0;
    if (rms >= threshold[6]) return 255;
    for (size_t i = 0; i < 6; ++i) {
        if (rms < threshold[i + 1]) {
            uint32_t span = threshold[i + 1] - threshold[i];
            uint32_t at = rms - threshold[i];
            uint32_t values = output[i + 1] - output[i];
            return (uint8_t)(output[i] + (at * values + span / 2) / span);
        }
    }
    return 255;
}

uint8_t pet_speech_mouth_envelope_process(pet_speech_mouth_envelope_t *state,
                                          const int16_t *pcm, size_t samples,
                                          uint8_t *articulation_out)
{
    if (!state || !pcm || !samples) {
        if (articulation_out) *articulation_out = 0;
        return 0;
    }
    uint64_t square = 0;
    uint64_t low_square = 0;
    uint64_t high_square = 0;
    uint32_t peak = 0;
    int32_t lowpass = state->lowpass_q0;
    /* ~800 Hz one-pole low pass at 24 kHz. Scale for other supported output
     * rates while keeping the coefficient bounded and deterministic. */
    uint32_t alpha_q15 = (5669u * 24000u + state->sample_rate / 2u) / state->sample_rate;
    if (alpha_q15 > 16384u) alpha_q15 = 16384u;
    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = pcm[i];
        uint32_t magnitude = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
        if (magnitude > peak) peak = magnitude;
        square += (uint64_t)((int64_t)sample * sample);
        lowpass += (int32_t)(((int64_t)(sample - lowpass) * alpha_q15 + 16384) >> 15);
        int32_t high = sample - lowpass;
        low_square += (uint64_t)((int64_t)lowpass * lowpass);
        high_square += (uint64_t)((int64_t)high * high);
    }
    state->lowpass_q0 = lowpass;
    uint32_t rms = integer_sqrt(square / samples);
    uint8_t target = rms_to_target(rms);
    int32_t delta = (int32_t)target - state->envelope;
    /* 20 ms slice: alpha ~= .63 for 20 ms attack and .23 for 75 ms release. */
    uint32_t response_q15 = delta > 0 ? 20709u : 7668u;
    int32_t next = state->envelope +
        (int32_t)(((int64_t)delta * response_q15 + (delta >= 0 ? 16384 : -16384)) >> 15);
    if (next < 0) next = 0;
    if (next > 255) next = 255;
    state->envelope = (uint8_t)next;

    uint64_t spectral_total = low_square + high_square;
    uint8_t spectral = spectral_total ?
        (uint8_t)((high_square * 255u) / spectral_total) : 0;
    int32_t spectral_delta = (int32_t)spectral - state->articulation;
    state->articulation = (uint8_t)(state->articulation +
        (int32_t)(((int64_t)spectral_delta * 7242 +
                   (spectral_delta >= 0 ? 16384 : -16384)) >> 15));

    state->square_total += square;
    state->samples += samples;
    if (peak > state->peak) state->peak = peak;
    if (state->envelope < state->minimum) state->minimum = state->envelope;
    if (state->envelope > state->maximum) state->maximum = state->envelope;
    state->envelope_total += state->envelope;
    state->slices++;
    if (articulation_out) *articulation_out = state->articulation;
    return state->envelope;
}

uint32_t pet_speech_mouth_envelope_rms(const pet_speech_mouth_envelope_t *state)
{
    return state && state->samples ? integer_sqrt(state->square_total / state->samples) : 0;
}

uint8_t pet_speech_mouth_envelope_decay(pet_speech_mouth_envelope_t *state,
                                        uint8_t *articulation_out)
{
    if (!state) return 0;
    int32_t delta = -(int32_t)state->envelope;
    int32_t next = state->envelope +
        (int32_t)(((int64_t)delta * 7668 - 16384) >> 15);
    state->envelope = next > 0 ? (uint8_t)next : 0;
    int32_t spectral_delta = -(int32_t)state->articulation;
    int32_t spectral = state->articulation +
        (int32_t)(((int64_t)spectral_delta * 7242 - 16384) >> 15);
    state->articulation = spectral > 0 ? (uint8_t)spectral : 0;
    if (state->envelope < state->minimum) state->minimum = state->envelope;
    state->envelope_total += state->envelope;
    state->slices++;
    if (articulation_out) *articulation_out = state->articulation;
    return state->envelope;
}

bool pet_speech_mouth_offset_valid(int32_t offset_ms)
{
    return offset_ms >= PET_SPEECH_MOUTH_OFFSET_MIN_MS &&
        offset_ms <= PET_SPEECH_MOUTH_OFFSET_MAX_MS &&
        offset_ms % PET_SPEECH_MOUTH_OFFSET_STEP_MS == 0;
}

int16_t pet_speech_mouth_offset_clamp(int32_t offset_ms)
{
    if (offset_ms < PET_SPEECH_MOUTH_OFFSET_MIN_MS) return PET_SPEECH_MOUTH_OFFSET_MIN_MS;
    if (offset_ms > PET_SPEECH_MOUTH_OFFSET_MAX_MS) return PET_SPEECH_MOUTH_OFFSET_MAX_MS;
    return (int16_t)offset_ms;
}

bool pet_speech_mouth_offset_parse(pet_speech_mouth_offset_json_t kind, double value,
                                   int16_t *offset_ms)
{
    if (!offset_ms) return false;
    if (kind == PET_SPEECH_MOUTH_OFFSET_ABSENT) {
        *offset_ms = PET_SPEECH_MOUTH_OFFSET_DEFAULT_MS;
        return true;
    }
    /* The range test first also rejects NaN and infinities. */
    if (kind != PET_SPEECH_MOUTH_OFFSET_NUMBER ||
        !(value >= PET_SPEECH_MOUTH_OFFSET_MIN_MS && value <= PET_SPEECH_MOUTH_OFFSET_MAX_MS)) return false;
    int32_t whole = (int32_t)value;
    if ((double)whole != value || !pet_speech_mouth_offset_valid(whole)) return false;
    *offset_ms = (int16_t)whole;
    return true;
}

uint32_t pet_speech_mouth_delay_ms(uint32_t audio_latency_ms, uint32_t display_latency_ms,
                                   int32_t offset_ms)
{
    int64_t delay = (int64_t)audio_latency_ms - display_latency_ms +
        pet_speech_mouth_offset_clamp(offset_ms);
    return delay > 0 ? (uint32_t)delay : 0u;
}

void pet_speech_mouth_delay_reset(pet_speech_mouth_delay_t *delay)
{
    if (!delay) return;
    delay->head = 0;
    delay->count = 0;
}

void pet_speech_mouth_delay_push(pet_speech_mouth_delay_t *delay, int64_t due_us,
                                 uint8_t level, uint8_t articulation)
{
    if (!delay) return;
    if (delay->count == PET_SPEECH_MOUTH_DELAY_SLOTS) {
        delay->head = (uint8_t)((delay->head + 1u) % PET_SPEECH_MOUTH_DELAY_SLOTS);
        delay->count--;
    }
    pet_speech_mouth_sample_t *slot =
        &delay->samples[(delay->head + delay->count) % PET_SPEECH_MOUTH_DELAY_SLOTS];
    slot->due_us = due_us;
    slot->level = level;
    slot->articulation = articulation;
    delay->count++;
}

bool pet_speech_mouth_delay_pop_due(pet_speech_mouth_delay_t *delay, int64_t now_us,
                                    pet_speech_mouth_sample_t *sample)
{
    bool found = false;
    /* Values leave in the order they came: one that falls due earlier than an
     * older one (the offset changed) waits for it. */
    while (delay && delay->count && delay->samples[delay->head].due_us <= now_us) {
        if (sample) *sample = delay->samples[delay->head];
        found = true;
        delay->head = (uint8_t)((delay->head + 1u) % PET_SPEECH_MOUTH_DELAY_SLOTS);
        delay->count--;
    }
    return found;
}

bool pet_speech_mouth_delay_last_due(const pet_speech_mouth_delay_t *delay, int64_t *due_us)
{
    if (!delay || !delay->count || !due_us) return false;
    *due_us = delay->samples[(delay->head + delay->count - 1u) % PET_SPEECH_MOUTH_DELAY_SLOTS].due_us;
    return true;
}
