#include "pet_tilt_detector.h"

#include <stddef.h>

extern float sqrtf(float value);

#define GRAVITY_ALPHA 0.10f
#define REFERENCE_ALPHA 0.01f
#define TILT_TRIGGER_SINE 0.10f
#define TILT_RELEASE_SINE 0.04f

static float absolute(float value) { return value < 0.0f ? -value : value; }

void pet_tilt_detector_reset(pet_tilt_detector_t *detector)
{
    if (detector) *detector = (pet_tilt_detector_t){0};
}

static bool normalize(float x, float y, float z,
                      float *nx, float *ny, float *nz)
{
    float magnitude = sqrtf(x * x + y * y + z * z);
    if (magnitude < 0.001f) return false;
    float inverse = 1.0f / magnitude;
    *nx = x * inverse;
    *ny = y * inverse;
    *nz = z * inverse;
    return true;
}

static pet_tilt_direction_t classify(float cross_x, float cross_y)
{
    if (absolute(cross_y) >= absolute(cross_x)) {
        return cross_y >= 0.0f ? PET_TILT_RIGHT : PET_TILT_LEFT;
    }
    return cross_x >= 0.0f ? PET_TILT_FORWARD : PET_TILT_BACK;
}

pet_tilt_direction_t pet_tilt_detector_update(pet_tilt_detector_t *detector,
                                              float x, float y, float z,
                                              uint32_t elapsed_ms,
                                              bool motion_quiet)
{
    if (!detector) return PET_TILT_NONE;
    if (!detector->initialized) {
        detector->gravity_x = detector->reference_x = x;
        detector->gravity_y = detector->reference_y = y;
        detector->gravity_z = detector->reference_z = z;
        detector->initialized = true;
        return PET_TILT_NONE;
    }

    detector->gravity_x += (x - detector->gravity_x) * GRAVITY_ALPHA;
    detector->gravity_y += (y - detector->gravity_y) * GRAVITY_ALPHA;
    detector->gravity_z += (z - detector->gravity_z) * GRAVITY_ALPHA;

    /* Cooldown runs only after the device has returned to center. Holding the
     * pet tilted therefore remains latched forever and can never retrigger. */
    if (!detector->latched && detector->cooldown_ms > 0) {
        detector->cooldown_ms = elapsed_ms >= detector->cooldown_ms ?
            0 : detector->cooldown_ms - elapsed_ms;
    }

    if (detector->settle_samples < PET_TILT_SETTLE_SAMPLES) {
        detector->settle_samples++;
        detector->reference_x = detector->gravity_x;
        detector->reference_y = detector->gravity_y;
        detector->reference_z = detector->gravity_z;
        return PET_TILT_NONE;
    }

    float rx;
    float ry;
    float rz;
    float gx;
    float gy;
    float gz;
    if (!normalize(detector->reference_x, detector->reference_y,
                   detector->reference_z, &rx, &ry, &rz) ||
        !normalize(detector->gravity_x, detector->gravity_y,
                   detector->gravity_z, &gx, &gy, &gz)) {
        return PET_TILT_NONE;
    }

    /* r x g approximates the signed rotation axis from the resting gravity
     * vector to the current gravity vector. X/Y are the screen-plane axes. */
    float cross_x = ry * gz - rz * gy;
    float cross_y = rz * gx - rx * gz;
    detector->last_tilt_amount = sqrtf(cross_x * cross_x + cross_y * cross_y);

    if (!motion_quiet) {
        detector->candidate = PET_TILT_NONE;
        detector->hold_ms = 0;
        return PET_TILT_NONE;
    }

    if (detector->last_tilt_amount <= TILT_RELEASE_SINE) {
        if (detector->latched) {
            detector->center_hold_ms += elapsed_ms;
            if (detector->center_hold_ms >= PET_TILT_CENTER_HOLD_MS) {
                detector->latched = false;
                detector->center_hold_ms = 0;
            }
        }
        detector->candidate = PET_TILT_NONE;
        detector->hold_ms = 0;
        detector->reference_x +=
            (detector->gravity_x - detector->reference_x) * REFERENCE_ALPHA;
        detector->reference_y +=
            (detector->gravity_y - detector->reference_y) * REFERENCE_ALPHA;
        detector->reference_z +=
            (detector->gravity_z - detector->reference_z) * REFERENCE_ALPHA;
        return PET_TILT_NONE;
    }

    if (detector->latched) {
        detector->center_hold_ms = 0;
        detector->candidate = PET_TILT_NONE;
        detector->hold_ms = 0;
        return PET_TILT_NONE;
    }

    if (detector->cooldown_ms > 0 ||
        detector->last_tilt_amount < TILT_TRIGGER_SINE) {
        detector->candidate = PET_TILT_NONE;
        detector->hold_ms = 0;
        return PET_TILT_NONE;
    }

    pet_tilt_direction_t direction = classify(cross_x, cross_y);
    if (direction != detector->candidate) {
        detector->candidate = direction;
        detector->hold_ms = elapsed_ms;
    } else {
        detector->hold_ms += elapsed_ms;
    }
    if (detector->hold_ms < PET_TILT_HOLD_MS) return PET_TILT_NONE;

    detector->latched = true;
    detector->cooldown_ms = PET_TILT_COOLDOWN_MS;
    detector->center_hold_ms = 0;
    detector->candidate = PET_TILT_NONE;
    detector->hold_ms = 0;
    return direction;
}
