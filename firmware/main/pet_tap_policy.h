#pragma once

#include <stdbool.h>

typedef enum {
    PET_TAP_START_LISTENING,
    PET_TAP_STOP_CAPTURE,
    PET_TAP_IGNORE_WAITING,
    PET_TAP_STOP_ANSWER,
    PET_TAP_CANCEL_LISTENING,
} pet_tap_action_t;

/* Capture wins because its stop is the one-and-only submission edge. A tap
 * during the listening cue cancels before the microphone ever opens. Once the
 * UI has moved to Thinking, queued or accidental taps must not cancel that
 * submitted turn. Answering remains explicitly interruptible. */
static inline pet_tap_action_t pet_tap_action(bool capturing, bool listen_pending,
                                              bool playing, bool thinking, bool answering)
{
    if (capturing) return PET_TAP_STOP_CAPTURE;
    if (listen_pending) return PET_TAP_CANCEL_LISTENING;
    if (playing || answering) return PET_TAP_STOP_ANSWER;
    if (thinking) return PET_TAP_IGNORE_WAITING;
    return PET_TAP_START_LISTENING;
}
