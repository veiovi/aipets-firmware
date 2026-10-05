#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The policy half of the sound cue worker: which cue plays next, which cues
 * are dropped and when a playing cue must stop. Pure and allocation-free so
 * host tests exercise it exactly; pet_sfx.c owns the task and the codec.
 *
 * Conversation cues (listening, submission, failures) take priority: one
 * waits at a time, the latest wins, and it stops decoration that is playing
 * or queued. Decoration (taps, swipes, settings, connection, shake) waits in
 * a small queue while nothing conversational is pending and goes stale.
 */

#define PET_SFX_PLAN_DECORATIVE_CAPACITY 3u
/* Decoration that waited longer than this no longer matches what happened. */
#define PET_SFX_PLAN_STALE_MS 400u

typedef struct {
    uint8_t cue;
    bool conversation;
    /* Nonzero when the requester waits for the outcome. */
    uint32_t token;
    uint32_t requested_ms;
} pet_sfx_plan_item_t;

typedef struct {
    pet_sfx_plan_item_t decorative[PET_SFX_PLAN_DECORATIVE_CAPACITY];
    uint8_t decorative_count;
    pet_sfx_plan_item_t priority;
    bool has_priority;
} pet_sfx_plan_t;

typedef struct {
    /* The cue playing now is decoration and must stop. */
    bool stop_playing;
    /* The request was refused: decoration while a conversation cue waits. */
    bool refused;
    /* A waiting item lost its turn; report it when it carries a token. */
    bool superseded;
    pet_sfx_plan_item_t superseded_item;
} pet_sfx_plan_effect_t;

void pet_sfx_plan_reset(pet_sfx_plan_t *plan);

/* `playing_decoration` is true while a decorative cue is playing. */
pet_sfx_plan_effect_t pet_sfx_plan_request(pet_sfx_plan_t *plan, pet_sfx_plan_item_t item,
                                           bool playing_decoration);

/* The next cue to play, skipping stale decoration. False when idle. */
bool pet_sfx_plan_next(pet_sfx_plan_t *plan, uint32_t now_ms, pet_sfx_plan_item_t *next);

/* Drop everything. Returns the waiting conversation cue so its requester can
 * be told it was cancelled. */
bool pet_sfx_plan_cancel(pet_sfx_plan_t *plan, pet_sfx_plan_item_t *cancelled);
