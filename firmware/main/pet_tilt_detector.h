#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PET_TILT_NONE = 0,
    PET_TILT_LEFT,
    PET_TILT_RIGHT,
    PET_TILT_FORWARD,
    PET_TILT_BACK,
} pet_tilt_direction_t;

enum {
    PET_TILT_SETTLE_SAMPLES = 20,
    PET_TILT_HOLD_MS = 300,
    PET_TILT_COOLDOWN_MS = 1500,
    PET_TILT_CENTER_HOLD_MS = 500,
};

typedef struct {
    float gravity_x;
    float gravity_y;
    float gravity_z;
    float reference_x;
    float reference_y;
    float reference_z;
    float last_tilt_amount;
    uint32_t hold_ms;
    uint32_t cooldown_ms;
    uint32_t center_hold_ms;
    uint16_t settle_samples;
    pet_tilt_direction_t candidate;
    bool initialized;
    bool latched;
} pet_tilt_detector_t;

void pet_tilt_detector_reset(pet_tilt_detector_t *detector);

/* Recognizes a deliberate roughly 30-degree tilt held for 300 ms. The
 * reference is the stable orientation observed when idle, so this works when
 * the pet starts flat, upright, or in a stand. Dynamic acceleration suppresses
 * recognition, preventing a shake from also becoming a tilt. */
pet_tilt_direction_t pet_tilt_detector_update(pet_tilt_detector_t *detector,
                                              float x, float y, float z,
                                              uint32_t elapsed_ms,
                                              bool motion_quiet);
