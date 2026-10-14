#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The story's sound loops (cloud story-game-v1 `story.atmosphere`). */
typedef enum {
    PET_ATMOSPHERE_NONE = 0,
    PET_ATMOSPHERE_CALM, PET_ATMOSPHERE_FOREST, PET_ATMOSPHERE_WATER, PET_ATMOSPHERE_NIGHT,
    PET_ATMOSPHERE_CAVE, PET_ATMOSPHERE_STORM, PET_ATMOSPHERE_DANGER, PET_ATMOSPHERE_BATTLE,
    PET_ATMOSPHERE_MAGIC, PET_ATMOSPHERE_VICTORY,
    PET_ATMOSPHERE_COUNT,
} pet_atmosphere_t;
/* PET_ATMOSPHERE_NONE for NULL or an unknown name. Names are lower case: "calm" … "victory". */
pet_atmosphere_t pet_atmosphere_from_name(const char *name);
/* From any task. A loop: the next reply plays over it (and one already playing switches to it).
 * PET_ATMOSPHERE_NONE: a playing loop fades out now. */
void pet_atmosphere_cue(pet_atmosphere_t atmosphere);
/* The same by name: a built-in loop, else a loop of the sound bank
 * (sound-bank-v1, pet_sound_bank.h); NULL or a name neither has fades it out. */
void pet_atmosphere_cue_name(const char *name);
/* The sound bank's one-shots, from any task, by id. A cue (story.sound) waits
 * for the next reply, which plays the cues first (pet_audio.h); at most three
 * wait, and more are dropped. A stinger plays at the next mix, in place of a
 * one-shot playing. One-shots play over the loop, which gives way to half its
 * level under them. False when dropped, or not a one-shot of the bank. */
bool pet_atmosphere_queue_sound(const char *id);
bool pet_atmosphere_play_sound(const char *id);
/* The cues waiting and a stinger not yet started are dropped. */
void pet_atmosphere_drop_sounds(void);
/* Cues wait for the next reply. */
bool pet_atmosphere_sounds_waiting(void);

/* One media task calls this between turns of its loop. It may read flash;
 * every playback API below only takes copies already published in RAM. */
void pet_atmosphere_prepare(void);

/* pet_audio.c's side, always called with its codec lock held. A level is a
 * Q15 fraction of the loop's own amplitude; the loop ramps to it. */
#define PET_ATMOSPHERE_BLOCK_MAX 960u
/* Mixes the loop and a one-shot into `pcm`, after taking any new cue, within
 * the headroom the speech leaves (at most PET_ATMOSPHERE_BLOCK_MAX samples).
 * Returns the samples that still clipped; `pcm` is untouched when neither
 * plays. */
uint32_t pet_atmosphere_mix(int16_t *pcm, size_t samples, uint32_t rate, int32_t level_q15);
/* The same into a block of the module's own: the sounds alone, or NULL when
 * neither plays. At most PET_ATMOSPHERE_BLOCK_MAX samples. */
int16_t *pet_atmosphere_block(size_t samples, uint32_t rate, int32_t level_q15);
/* A loop or a one-shot plays, fades, waits to resume or is about to start. */
bool pet_atmosphere_active(void);
/* A loop plays, fades or waits to resume. */
bool pet_atmosphere_looping(void);
/* While a reply waits for the cues: the next one starts once none plays.
 * False once all have played. */
bool pet_atmosphere_play_queued(void);
/* The loop fades out over `ms`, then ends. A fade under way keeps its pace. */
void pet_atmosphere_fade_out(uint32_t ms);
/* The speaker closed under the sounds, which ends a one-shot. `resume`: the
 * loop fades back in where it was at the next mix; otherwise it ends, and the
 * cues waiting are dropped. */
void pet_atmosphere_stop(bool resume);
