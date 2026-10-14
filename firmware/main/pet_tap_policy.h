#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/* A tap this soon after the one before is a double tap. */
#define PET_TAP_DOUBLE_US 350000

typedef enum {
    PET_ANSWER_STOP,
    PET_ANSWER_HOLD,
    PET_ANSWER_PLAY_ON,
} pet_answer_tap_t;

/* What a tap that reaches an answer (PET_TAP_STOP_ANSWER) does to it when the
 * answer can be held (`holdable`: the Pocket in story mode, with a gateway
 * that holds speech) or is held now: the first tap holds it at once and the
 * next lets it play on. A double tap stops it, so a double tap while the pet
 * speaks holds it, then stops it; a single tap never waits for a second. */
static inline pet_answer_tap_t pet_answer_tap(bool holdable, bool held, int64_t since_last_tap_us)
{
    if (since_last_tap_us < PET_TAP_DOUBLE_US || (!holdable && !held)) return PET_ANSWER_STOP;
    return held ? PET_ANSWER_PLAY_ON : PET_ANSWER_HOLD;
}

typedef enum {
    PET_TALK_NONE,
    PET_TALK_LISTEN,
    PET_TALK_STOP_ANSWER,
    PET_TALK_SEND,
    PET_TALK_CANCEL_LISTENING,
} pet_talk_action_t;

/* The talk button: hold to talk, release to send. While it is held the pet
 * listens once (`listened`), as soon as it is free: an answer stops first, as
 * a tap stops it, and a submitted turn keeps waiting. Releasing it sends what
 * the microphone heard, or cancels a listen whose microphone never opened. */
static inline pet_talk_action_t pet_talk_action(bool held, bool listened, bool capturing,
                                                bool listen_pending, bool answering, bool thinking)
{
    if (!held) return capturing ? PET_TALK_SEND : listen_pending ? PET_TALK_CANCEL_LISTENING : PET_TALK_NONE;
    if (listened || capturing || listen_pending || thinking) return PET_TALK_NONE;
    return answering ? PET_TALK_STOP_ANSWER : PET_TALK_LISTEN;
}
