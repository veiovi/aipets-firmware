#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Full-frame presentation ceilings. The animation arbiter evaluates inputs at
 * 30 Hz; these values only cap how often a changed logical frame may be
 * presented. Essential blink, speech, touch, listening, and thinking actions
 * are never disabled by a profile. */
typedef enum {
    PET_ANIMATION_FULL = 0,
    PET_ANIMATION_BALANCED,
    PET_ANIMATION_REDUCED,
    PET_ANIMATION_PROFILE_COUNT,
} pet_animation_profile_t;

typedef enum {
    PET_ANIMATION_ACTIVITY_DEFAULT = 0,
    PET_ANIMATION_ACTIVITY_BURST,
    PET_ANIMATION_ACTIVITY_SUSTAINED,
    PET_ANIMATION_ACTIVITY_IDLE,
} pet_animation_activity_t;

#define PET_ANIMATION_PROFILE_DEFAULT PET_ANIMATION_BALANCED

static inline bool pet_animation_profile_valid(pet_animation_profile_t profile)
{
    return profile >= PET_ANIMATION_FULL && profile < PET_ANIMATION_PROFILE_COUNT;
}

static inline const char *pet_animation_profile_name(pet_animation_profile_t profile)
{
    switch (profile) {
        case PET_ANIMATION_FULL: return "Full";
        case PET_ANIMATION_BALANCED: return "Balanced";
        case PET_ANIMATION_REDUCED: return "Reduced";
        default: return "Balanced";
    }
}

/* The cloud protocol uses stable lowercase values. Keep this separate from the
 * title-case display name so UI copy can change without changing the wire. */
static inline const char *pet_animation_profile_wire_value(
    pet_animation_profile_t profile)
{
    switch (profile) {
        case PET_ANIMATION_FULL: return "full";
        case PET_ANIMATION_BALANCED: return "balanced";
        case PET_ANIMATION_REDUCED: return "reduced";
        default: return NULL;
    }
}

static inline bool pet_animation_profile_parse_wire(
    const char *value, pet_animation_profile_t *profile)
{
    if (!value || !profile) return false;

    for (pet_animation_profile_t candidate = PET_ANIMATION_FULL;
         candidate < PET_ANIMATION_PROFILE_COUNT; candidate++) {
        const char *expected = pet_animation_profile_wire_value(candidate);
        const char *left = value;
        const char *right = expected;
        bool same = true;
        while (*left && *right) {
            char a = *left++;
            char b = *right++;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same && *left == '\0' && *right == '\0') {
            *profile = candidate;
            return true;
        }
    }
    return false;
}

static inline uint32_t pet_animation_profile_interval_ms(
    pet_animation_profile_t profile, pet_animation_activity_t activity)
{
    const bool burst = activity == PET_ANIMATION_ACTIVITY_BURST;
    const bool idle = activity == PET_ANIMATION_ACTIVITY_IDLE;
    switch (profile) {
        case PET_ANIMATION_FULL:
            return burst ? 67u : idle ? 167u : 84u;   /* 15 / 6 / 12 fps */
        case PET_ANIMATION_REDUCED:
            return burst ? 100u : idle ? 500u : 200u; /* 10 / 2 / 5 fps */
        case PET_ANIMATION_BALANCED:
        default:
            return burst ? 67u : idle ? 250u : 125u;  /* 15 / 4 / 8 fps */
    }
}

/* Capture and playback both use the sustained ceiling. The frame player adapts
 * decorative work internally when audio queues or display deadlines become
 * unhealthy instead of silently forcing a different saved profile. */
static inline uint32_t pet_animation_capture_interval_ms(
    pet_animation_profile_t profile)
{
    return pet_animation_profile_interval_ms(
        profile, PET_ANIMATION_ACTIVITY_SUSTAINED);
}

static inline uint32_t pet_animation_streaming_speech_interval_ms(
    pet_animation_profile_t profile)
{
    return pet_animation_profile_interval_ms(
        profile, PET_ANIMATION_ACTIVITY_SUSTAINED);
}
