#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The 18 embedded interaction cues (sfx/README.md). The remaining original
 * sources stay in sfx/ with their provenance but are not linked. */
typedef enum {
    PET_SFX_WAKE,
    PET_SFX_CONNECT,
    PET_SFX_LISTEN,
    PET_SFX_SUBMIT,
    PET_SFX_DEPLOY_COMPLETE,
    PET_SFX_CANCEL,
    PET_SFX_RETRY,
    PET_SFX_ERROR,
    PET_SFX_NO_VOICE,
    PET_SFX_SETTINGS_OPEN,
    PET_SFX_SETTINGS_CLOSE,
    PET_SFX_FACE_SWIPE_0,
    PET_SFX_FACE_SWIPE_1,
    PET_SFX_FACE_SWIPE_2,
    PET_SFX_FACE_SWIPE_3,
    PET_SFX_FACE_SWIPE_4,
    PET_SFX_GESTURE_SHAKE,
    /* Ordinary accepted UI tap. */
    PET_SFX_GESTURE_POP,
    PET_SFX_COUNT,
} pet_sfx_t;

typedef enum {
    PET_SFX_PLAYED,
    /* Not played: effects off, the codec busy with speech, or superseded. */
    PET_SFX_SKIPPED,
    /* Stopped or dropped by pet_sfx_cancel(). */
    PET_SFX_CANCELLED,
} pet_sfx_outcome_t;

/* Runs on the cue worker; it must only hand work to another task. */
typedef void (*pet_sfx_listen_done_t)(uint32_t token, pet_sfx_outcome_t outcome);

/* Start the cue worker after pet_audio_init(). */
esp_err_t pet_sfx_init(pet_sfx_listen_done_t listen_done);

/*
 * Queue a cue without blocking. Conversation cues (listen, submit, cancel,
 * retry, no voice, error) stop playing decoration and wait alone, latest
 * first. Decoration is refused while a conversation cue waits, and dropped
 * when it has waited too long. Speech and the microphone always win.
 */
esp_err_t pet_sfx_play(pet_sfx_t effect);

/* Queue the listening cue. Its outcome is reported with `token` (nonzero,
 * below 0x80000000) so the microphone opens only after the cue. */
esp_err_t pet_sfx_play_listen(uint32_t token);

/* Play a cue and wait up to `timeout_ms` for it to finish (console use). */
esp_err_t pet_sfx_play_and_wait(pet_sfx_t effect, uint32_t timeout_ms);

/* Stop the playing cue within one 20 ms chunk and drop queued cues. */
void pet_sfx_cancel(void);

/* Effects toggle for decoration; conversation cues still play. On by default. */
void pet_sfx_set_effects_enabled(bool enabled);
bool pet_sfx_effects_enabled(void);

/* Five-note face-swipe sequence. Any index wraps modulo five. */
pet_sfx_t pet_sfx_for_face_swipe(uint8_t index);
