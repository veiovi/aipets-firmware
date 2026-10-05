#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_ai.h"
#include "pet_face_limits.h"
#include "pet_voice.h"

#define PET_FACE_PROFILE_MAX 8

typedef enum {
    PET_FACE_GENDER_NEUTRAL = 0,
    PET_FACE_GENDER_MALE,
    PET_FACE_GENDER_FEMALE,
    PET_FACE_GENDER_COUNT,
} pet_face_gender_t;

typedef struct {
    char face_id[PET_FACE_ID_MAX];
    pet_ai_mode_t ai_mode;
    pet_voice_t voice;
    pet_realtime_model_t realtime_model;
    pet_realtime_voice_t realtime_voice;
    pet_cartesia_voice_gender_t cartesia_voice_gender;
    uint32_t last_used;
} pet_face_profile_t;

typedef struct {
    pet_face_profile_t entries[PET_FACE_PROFILE_MAX];
    uint32_t next_sequence;
    uint8_t count;
} pet_face_profiles_t;

bool pet_face_id_valid(const char *face_id);
bool pet_face_gender_valid(pet_face_gender_t gender);
const char *pet_face_gender_name(pet_face_gender_t gender);
bool pet_face_voice_allowed(pet_face_gender_t gender, pet_voice_t voice);
bool pet_face_realtime_voice_allowed(pet_face_gender_t gender,
                                     pet_realtime_voice_t voice);
bool pet_face_cartesia_gender_allowed(pet_face_gender_t face_gender,
                                      pet_cartesia_voice_gender_t voice_gender);
void pet_face_profile_defaults(pet_face_profile_t *profile, const char *face_id,
                               pet_face_gender_t gender);
void pet_face_profile_constrain(pet_face_profile_t *profile,
                                pet_face_gender_t gender);
bool pet_face_profile_valid(const pet_face_profile_t *profile);
pet_face_profile_t *pet_face_profiles_find(pet_face_profiles_t *profiles,
                                           const char *face_id);
const pet_face_profile_t *pet_face_profiles_find_const(
    const pet_face_profiles_t *profiles, const char *face_id);
bool pet_face_profiles_put(pet_face_profiles_t *profiles,
                           const pet_face_profile_t *profile);
void pet_face_profiles_touch(pet_face_profiles_t *profiles,
                             pet_face_profile_t *profile);
