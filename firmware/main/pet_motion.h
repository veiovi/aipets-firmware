#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PET_MOTION_TRIGGER_SHAKE = 0,
    PET_MOTION_TRIGGER_TILT_LEFT,
    PET_MOTION_TRIGGER_TILT_RIGHT,
    PET_MOTION_TRIGGER_TILT_FORWARD,
    PET_MOTION_TRIGGER_TILT_BACK,
} pet_motion_trigger_t;

typedef void (*pet_motion_gesture_callback_t)(uint8_t gesture,
                                              pet_motion_trigger_t trigger);
typedef bool (*pet_motion_enabled_callback_t)(void);

/* Starts the QMI8658 accelerometer monitor. The callback runs from the motion
 * task and must remain non-blocking. */
esp_err_t pet_motion_start(pet_motion_gesture_callback_t callback,
                           pet_motion_enabled_callback_t enabled_callback,
                           uint8_t sensitivity);
void pet_motion_set_sensitivity(uint8_t sensitivity);
