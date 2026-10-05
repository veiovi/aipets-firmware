#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    PET_SHAKE_SAMPLE_MS = 25,
    PET_SHAKE_SETTLE_SAMPLES = 20,
    PET_SHAKE_WINDOW_MS = 900,
    PET_SHAKE_COOLDOWN_MS = 4000,
    PET_SHAKE_REFRACTORY_MS = 50,
};

typedef struct {
    float gravity_x;
    float gravity_y;
    float gravity_z;
    uint32_t window_ms;
    uint32_t cooldown_ms;
    uint32_t refractory_ms;
    uint16_t settle_samples;
    uint8_t impulse_count;
    uint8_t sensitivity;
    uint8_t last_rejection;
    bool initialized;
    bool sensitivity_configured;
    bool have_previous_linear;
    float previous_linear_x;
    float previous_linear_y;
    float previous_linear_z;
    float previous_impulse_x;
    float previous_impulse_y;
    float previous_impulse_z;
    float noise_mean;
    float noise_deviation;
    float last_linear_accel;
    float last_jerk;
    float effective_threshold;
} pet_shake_detector_t;

void pet_shake_detector_reset(pet_shake_detector_t *detector);
void pet_shake_detector_set_sensitivity(pet_shake_detector_t *detector,
                                        uint8_t sensitivity);

/* Feed acceleration in m/s^2 at a steady cadence. Returns true once for a
 * deliberate shake: three alternating linear-acceleration impulses inside 900 ms.
 * A four-second cooldown prevents one sustained shake from spamming cues. */
bool pet_shake_detector_update(pet_shake_detector_t *detector,
                               float x, float y, float z, uint32_t elapsed_ms);

/* Maps an arbitrary random word onto the seven semantic gestures requested
 * for physical shake reactions. Spin gestures are intentionally excluded. */
uint8_t pet_shake_pick_gesture(uint32_t random_word);
