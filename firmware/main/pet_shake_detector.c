#include "pet_shake_detector.h"

#include <stddef.h>
#include "face_core.h"

extern float sqrtf(float value);

static float absolute(float value) { return value < 0.0f ? -value : value; }

/* A shake should require purposeful, repeated motion rather than a change in
 * orientation or a single bump. The low-pass estimate follows gravity and the
 * remaining vector is linear acceleration. The first hardware implementation
 * compared the difference between adjacent 25 ms samples with thresholds that
 * were specified for acceleration; ordinary 2-3 Hz hand shakes therefore
 * stayed below threshold even at maximum sensitivity. Detect the physical
 * acceleration itself and use direction reversals to reject tilts and bumps. */
#define GRAVITY_TRACK_ALPHA 0.05f
#define NOISE_TRACK_ALPHA 0.04f
#define NOISE_MULTIPLIER 5.0f
#define DIRECTION_REVERSAL_DOT -0.20f
#define SHAKE_REQUIRED_IMPULSES 3
#define SHAKE_DEFAULT_SENSITIVITY 75

static float base_threshold(uint8_t sensitivity)
{
    if (!sensitivity) return __builtin_inff();
    return 4.5f - ((float)(sensitivity - 1) * 3.3f / 99.0f);
}

void pet_shake_detector_reset(pet_shake_detector_t *detector)
{
    if (!detector) return;
    uint8_t sensitivity = detector->sensitivity_configured ?
        detector->sensitivity : SHAKE_DEFAULT_SENSITIVITY;
    *detector = (pet_shake_detector_t){
        .sensitivity = sensitivity,
        .sensitivity_configured = true,
    };
}

void pet_shake_detector_set_sensitivity(pet_shake_detector_t *detector,
                                        uint8_t sensitivity)
{
    if (!detector) return;
    if (sensitivity > 100) sensitivity = 100;
    *detector = (pet_shake_detector_t){
        .sensitivity = sensitivity,
        .sensitivity_configured = true,
    };
}

static void reset_impulses(pet_shake_detector_t *detector)
{
    detector->window_ms = 0;
    detector->impulse_count = 0;
    detector->previous_impulse_x = 0;
    detector->previous_impulse_y = 0;
    detector->previous_impulse_z = 0;
}

bool pet_shake_detector_update(pet_shake_detector_t *detector,
                               float x, float y, float z, uint32_t elapsed_ms)
{
    if (!detector) return false;
    if (!detector->sensitivity_configured) {
        pet_shake_detector_set_sensitivity(detector, SHAKE_DEFAULT_SENSITIVITY);
    }
    if (!detector->sensitivity) return false;
    if (!detector->initialized) {
        detector->gravity_x = x;
        detector->gravity_y = y;
        detector->gravity_z = z;
        detector->initialized = true;
        return false;
    }

    detector->gravity_x += (x - detector->gravity_x) * GRAVITY_TRACK_ALPHA;
    detector->gravity_y += (y - detector->gravity_y) * GRAVITY_TRACK_ALPHA;
    detector->gravity_z += (z - detector->gravity_z) * GRAVITY_TRACK_ALPHA;

    if (detector->settle_samples < PET_SHAKE_SETTLE_SAMPLES) {
        detector->settle_samples++;
        if (detector->settle_samples == PET_SHAKE_SETTLE_SAMPLES) {
            detector->previous_linear_x = x - detector->gravity_x;
            detector->previous_linear_y = y - detector->gravity_y;
            detector->previous_linear_z = z - detector->gravity_z;
            detector->have_previous_linear = true;
        }
        return false;
    }

    float linear_x = x - detector->gravity_x;
    float linear_y = y - detector->gravity_y;
    float linear_z = z - detector->gravity_z;
    if (!detector->have_previous_linear) {
        detector->previous_linear_x = linear_x;
        detector->previous_linear_y = linear_y;
        detector->previous_linear_z = linear_z;
        detector->have_previous_linear = true;
        return false;
    }
    float jerk_x = linear_x - detector->previous_linear_x;
    float jerk_y = linear_y - detector->previous_linear_y;
    float jerk_z = linear_z - detector->previous_linear_z;
    detector->previous_linear_x = linear_x;
    detector->previous_linear_y = linear_y;
    detector->previous_linear_z = linear_z;
    detector->last_jerk = sqrtf(jerk_x * jerk_x + jerk_y * jerk_y + jerk_z * jerk_z);
    detector->last_linear_accel = sqrtf(linear_x * linear_x +
                                        linear_y * linear_y +
                                        linear_z * linear_z);
    float configured = base_threshold(detector->sensitivity);
    if (detector->impulse_count == 0 && detector->cooldown_ms == 0 &&
        detector->last_linear_accel < configured * 0.65f) {
        if (detector->noise_mean == 0.0f) {
            detector->noise_mean = detector->last_linear_accel;
        }
        float delta = detector->last_linear_accel - detector->noise_mean;
        detector->noise_mean += delta * NOISE_TRACK_ALPHA;
        detector->noise_deviation +=
            (absolute(delta) - detector->noise_deviation) * NOISE_TRACK_ALPHA;
    }
    float adaptive = detector->noise_mean +
                     detector->noise_deviation * NOISE_MULTIPLIER;
    detector->effective_threshold = adaptive > configured ? adaptive : configured;

    if (detector->cooldown_ms > 0) {
        detector->cooldown_ms = elapsed_ms >= detector->cooldown_ms ?
            0 : detector->cooldown_ms - elapsed_ms;
        reset_impulses(detector);
        return false;
    }

    if (detector->refractory_ms > 0) {
        detector->refractory_ms = elapsed_ms >= detector->refractory_ms ?
            0 : detector->refractory_ms - elapsed_ms;
    }

    if (detector->impulse_count > 0) {
        detector->window_ms += elapsed_ms;
        if (detector->window_ms > PET_SHAKE_WINDOW_MS) {
            detector->last_rejection = 2; /* candidate window expired */
            reset_impulses(detector);
        }
    }

    if (detector->refractory_ms == 0 && detector->last_linear_accel >=
        detector->effective_threshold) {
        float inverse = 1.0f / detector->last_linear_accel;
        float direction_x = linear_x * inverse;
        float direction_y = linear_y * inverse;
        float direction_z = linear_z * inverse;
        bool accepted = detector->impulse_count == 0;
        if (!accepted) {
            float dot = direction_x * detector->previous_impulse_x +
                        direction_y * detector->previous_impulse_y +
                        direction_z * detector->previous_impulse_z;
            accepted = dot <= DIRECTION_REVERSAL_DOT;
        }
        detector->refractory_ms = PET_SHAKE_REFRACTORY_MS;
        if (!accepted) {
            detector->last_rejection = 1; /* impulse did not reverse direction */
            return false;
        }
        if (detector->impulse_count == 0) detector->window_ms = 0;
        detector->previous_impulse_x = direction_x;
        detector->previous_impulse_y = direction_y;
        detector->previous_impulse_z = direction_z;
        detector->impulse_count++;
        detector->last_rejection = 0;
        if (detector->impulse_count >= SHAKE_REQUIRED_IMPULSES) {
            reset_impulses(detector);
            detector->cooldown_ms = PET_SHAKE_COOLDOWN_MS;
            return true;
        }
    }

    return false;
}

uint8_t pet_shake_pick_gesture(uint32_t random_word)
{
    static const uint8_t gestures[] = {
        FC_GESTURE_SHAKE,
        FC_GESTURE_NOD,
        FC_GESTURE_ZOOM_IN,
        FC_GESTURE_HEARTBEAT,
        FC_GESTURE_WOBBLE,
        FC_GESTURE_POP,
    };
    return gestures[random_word % (sizeof(gestures) / sizeof(gestures[0]))];
}
