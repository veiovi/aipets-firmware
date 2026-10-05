#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PET_SPEECH_MOUTH_CURRENT_SPRITES = 0,
    PET_SPEECH_MOUTH_FULL_RANGE_SPRITES = 1,
    PET_SPEECH_MOUTH_SYNCED_SPRITES = 2,
    PET_SPEECH_MOUTH_SMOOTH_24_STEP = 3,
    PET_SPEECH_MOUTH_CLASSIC_SHAPE = 4,
    PET_SPEECH_MOUTH_EXPRESSIVE_SHAPE = 5,
    PET_SPEECH_MOUTH_MODE_COUNT,
} pet_speech_mouth_mode_t;

#define PET_SPEECH_MOUTH_DEFAULT PET_SPEECH_MOUTH_CURRENT_SPRITES
/* Full-frame packs expose their authored audio-stage bank (seven stages for
 * Standard/Rich packs). The six historical renderer choices remain stable
 * only so old NVS/protocol values can be accepted and migrated safely. */
#define PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED PET_SPEECH_MOUTH_SYNCED_SPRITES

typedef struct {
    pet_speech_mouth_mode_t mode;
    uint32_t sample_rate;
    int32_t lowpass_q0;
    uint8_t envelope;
    uint8_t articulation;
    uint8_t minimum;
    uint8_t maximum;
    uint64_t envelope_total;
    uint64_t square_total;
    uint64_t samples;
    uint32_t peak;
    uint32_t slices;
} pet_speech_mouth_envelope_t;

const char *pet_speech_mouth_mode_name(pet_speech_mouth_mode_t mode);
bool pet_speech_mouth_mode_valid(pet_speech_mouth_mode_t mode);
bool pet_speech_mouth_mode_parse(const char *value, pet_speech_mouth_mode_t *mode);
pet_speech_mouth_mode_t pet_speech_mouth_full_frame_normalize(
    pet_speech_mouth_mode_t legacy_mode);

/* Mode 1 is intentionally the original level calculation, unchanged. */
uint8_t pet_speech_mouth_current_level(const int16_t *pcm, size_t samples);
/* Mode 2 expands the historical 0..100 envelope across the full byte. */
uint8_t pet_speech_mouth_full_range_level(const int16_t *pcm, size_t samples);

void pet_speech_mouth_envelope_reset(pet_speech_mouth_envelope_t *state,
                                     pet_speech_mouth_mode_t mode,
                                     uint32_t sample_rate);
uint8_t pet_speech_mouth_envelope_process(pet_speech_mouth_envelope_t *state,
                                          const int16_t *pcm, size_t samples,
                                          uint8_t *articulation_out);
uint8_t pet_speech_mouth_envelope_decay(pet_speech_mouth_envelope_t *state,
                                        uint8_t *articulation_out);
uint32_t pet_speech_mouth_envelope_rms(const pet_speech_mouth_envelope_t *state);

/* Mouth timing: a signed per-device fine-tune on top of the firmware's own
 * latency compensation. Positive shows the mouth later, negative earlier. The
 * cloud sends it as config v2 `speechMouthOffsetMs`: an integer from -300 to
 * 300 in steps of 10, missing meaning 0. */
#define PET_SPEECH_MOUTH_OFFSET_MIN_MS (-300)
#define PET_SPEECH_MOUTH_OFFSET_MAX_MS 300
#define PET_SPEECH_MOUTH_OFFSET_STEP_MS 10
#define PET_SPEECH_MOUTH_OFFSET_DEFAULT_MS 0

/* The JSON value found for `speechMouthOffsetMs`. */
typedef enum {
    PET_SPEECH_MOUTH_OFFSET_ABSENT = 0,
    PET_SPEECH_MOUTH_OFFSET_NUMBER,
    /* A string, null, boolean, object or array. */
    PET_SPEECH_MOUTH_OFFSET_OTHER,
} pet_speech_mouth_offset_json_t;

bool pet_speech_mouth_offset_valid(int32_t offset_ms);
int16_t pet_speech_mouth_offset_clamp(int32_t offset_ms);
/* The contract, exactly: a missing value is 0; a number must be a whole
 * number of milliseconds in range and on a step. False rejects the document. */
bool pet_speech_mouth_offset_parse(pet_speech_mouth_offset_json_t kind, double value,
                                   int16_t *offset_ms);

/* How long after its level is computed the mouth should show it: the audio
 * latency to the middle of its slice, less the display latency, plus the
 * offset. Never negative: the mouth cannot run ahead of the analysis. */
uint32_t pet_speech_mouth_delay_ms(uint32_t audio_latency_ms, uint32_t display_latency_ms,
                                   int32_t offset_ms);

/* A delay line of mouth values, each shown once its time comes: 24 values of
 * 20 ms, more than the longest delay holds at 24 kHz (under 400 ms). */
#define PET_SPEECH_MOUTH_DELAY_SLOTS 24
typedef struct {
    int64_t due_us;
    uint8_t level;
    uint8_t articulation;
} pet_speech_mouth_sample_t;
typedef struct {
    pet_speech_mouth_sample_t samples[PET_SPEECH_MOUTH_DELAY_SLOTS];
    uint8_t head;
    uint8_t count;
} pet_speech_mouth_delay_t;

void pet_speech_mouth_delay_reset(pet_speech_mouth_delay_t *delay);
/* Queues a value; when full, the oldest gives way. */
void pet_speech_mouth_delay_push(pet_speech_mouth_delay_t *delay, int64_t due_us,
                                 uint8_t level, uint8_t articulation);
/* Removes every value due by `now_us`; true with the newest of them. */
bool pet_speech_mouth_delay_pop_due(pet_speech_mouth_delay_t *delay, int64_t now_us,
                                    pet_speech_mouth_sample_t *sample);
/* True with the due time of the last queued value. */
bool pet_speech_mouth_delay_last_due(const pet_speech_mouth_delay_t *delay, int64_t *due_us);
