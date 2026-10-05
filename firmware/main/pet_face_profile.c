#include "pet_face_profile.h"

extern size_t strlen(const char *value);
extern int strcmp(const char *left, const char *right);
extern void *memset(void *target, int value, size_t length);

static bool ascii_lower_or_digit(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'z');
}

bool pet_face_id_valid(const char *face_id)
{
    if (!face_id) return false;
    size_t length = strlen(face_id);
    if (length < 3 || length >= PET_FACE_ID_MAX ||
        face_id[0] < 'a' || face_id[0] > 'z' ||
        !ascii_lower_or_digit(face_id[length - 1])) return false;
    for (size_t i = 1; i + 1 < length; ++i) {
        char value = face_id[i];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-')) return false;
    }
    return true;
}

bool pet_face_gender_valid(pet_face_gender_t gender)
{
    return gender >= PET_FACE_GENDER_NEUTRAL && gender < PET_FACE_GENDER_COUNT;
}

const char *pet_face_gender_name(pet_face_gender_t gender)
{
    static const char *const names[PET_FACE_GENDER_COUNT] = {
        "neutral", "male", "female",
    };
    return pet_face_gender_valid(gender) ? names[gender] : names[0];
}

bool pet_face_voice_allowed(pet_face_gender_t gender, pet_voice_t voice)
{
    if (!pet_face_gender_valid(gender) || voice < PET_VOICE_PUCK ||
        voice >= PET_VOICE_COUNT) return false;
    if (gender == PET_FACE_GENDER_NEUTRAL) return true;
    const bool female = voice == PET_VOICE_KORE || voice == PET_VOICE_AOEDE ||
                        voice == PET_VOICE_LEDA;
    return gender == PET_FACE_GENDER_FEMALE ? female : !female;
}

bool pet_face_realtime_voice_allowed(pet_face_gender_t gender,
                                     pet_realtime_voice_t voice)
{
    if (!pet_face_gender_valid(gender) || voice < PET_REALTIME_VOICE_CEDAR ||
        voice >= PET_REALTIME_VOICE_COUNT) return false;
    if (gender == PET_FACE_GENDER_NEUTRAL) return true;
    const bool female = voice == PET_REALTIME_VOICE_MARIN ||
                        voice == PET_REALTIME_VOICE_CORAL ||
                        voice == PET_REALTIME_VOICE_SAGE ||
                        voice == PET_REALTIME_VOICE_SHIMMER;
    if (voice == PET_REALTIME_VOICE_ALLOY) return false;
    return gender == PET_FACE_GENDER_FEMALE ? female : !female;
}

bool pet_face_cartesia_gender_allowed(pet_face_gender_t face_gender,
                                      pet_cartesia_voice_gender_t voice_gender)
{
    if (!pet_face_gender_valid(face_gender) ||
        voice_gender < PET_CARTESIA_VOICE_NEUTRAL ||
        voice_gender >= PET_CARTESIA_VOICE_GENDER_COUNT) return false;
    if (face_gender == PET_FACE_GENDER_NEUTRAL) return true;
    return (face_gender == PET_FACE_GENDER_FEMALE &&
            voice_gender == PET_CARTESIA_VOICE_FEMALE) ||
           (face_gender == PET_FACE_GENDER_MALE &&
            voice_gender == PET_CARTESIA_VOICE_MALE);
}

void pet_face_profile_defaults(pet_face_profile_t *profile, const char *face_id,
                               pet_face_gender_t gender)
{
    if (!profile) return;
    memset(profile, 0, sizeof(*profile));
    if (face_id) {
        size_t i = 0;
        while (i + 1 < sizeof(profile->face_id) && face_id[i]) {
            profile->face_id[i] = face_id[i];
            i++;
        }
        profile->face_id[i] = '\0';
    }
    profile->ai_mode = PET_AI_MODE_OPENROUTER;
    profile->realtime_model = PET_REALTIME_MODEL_2_1_MINI;
    if (gender == PET_FACE_GENDER_FEMALE) {
        profile->voice = PET_VOICE_KORE;
        profile->realtime_voice = PET_REALTIME_VOICE_MARIN;
        profile->cartesia_voice_gender = PET_CARTESIA_VOICE_FEMALE;
    } else if (gender == PET_FACE_GENDER_MALE) {
        profile->voice = PET_VOICE_PUCK;
        profile->realtime_voice = PET_REALTIME_VOICE_CEDAR;
        profile->cartesia_voice_gender = PET_CARTESIA_VOICE_MALE;
    } else {
        profile->voice = PET_VOICE_PUCK;
        profile->realtime_voice = PET_REALTIME_VOICE_CEDAR;
        profile->cartesia_voice_gender = PET_CARTESIA_VOICE_NEUTRAL;
    }
}

void pet_face_profile_constrain(pet_face_profile_t *profile,
                                pet_face_gender_t gender)
{
    if (!profile) return;
    if (profile->ai_mode < PET_AI_MODE_OPENROUTER ||
        profile->ai_mode >= PET_AI_MODE_COUNT) {
        profile->ai_mode = PET_AI_MODE_OPENROUTER;
    }
    if (profile->realtime_model < PET_REALTIME_MODEL_2_1_MINI ||
        profile->realtime_model >= PET_REALTIME_MODEL_COUNT) {
        profile->realtime_model = PET_REALTIME_MODEL_2_1_MINI;
    }
    if (!pet_face_voice_allowed(gender, profile->voice)) {
        profile->voice = gender == PET_FACE_GENDER_FEMALE ?
            PET_VOICE_KORE : PET_VOICE_PUCK;
    }
    if (!pet_face_realtime_voice_allowed(gender, profile->realtime_voice)) {
        profile->realtime_voice = gender == PET_FACE_GENDER_FEMALE ?
            PET_REALTIME_VOICE_MARIN : PET_REALTIME_VOICE_CEDAR;
    }
    if (!pet_face_cartesia_gender_allowed(gender,
                                           profile->cartesia_voice_gender)) {
        profile->cartesia_voice_gender = gender == PET_FACE_GENDER_FEMALE ?
            PET_CARTESIA_VOICE_FEMALE :
            gender == PET_FACE_GENDER_MALE ? PET_CARTESIA_VOICE_MALE :
                                             PET_CARTESIA_VOICE_NEUTRAL;
    }
}

bool pet_face_profile_valid(const pet_face_profile_t *profile)
{
    return profile && pet_face_id_valid(profile->face_id) &&
        profile->ai_mode >= PET_AI_MODE_OPENROUTER &&
        profile->ai_mode < PET_AI_MODE_COUNT &&
        profile->voice >= PET_VOICE_PUCK && profile->voice < PET_VOICE_COUNT &&
        profile->realtime_model >= PET_REALTIME_MODEL_2_1_MINI &&
        profile->realtime_model < PET_REALTIME_MODEL_COUNT &&
        profile->realtime_voice >= PET_REALTIME_VOICE_CEDAR &&
        profile->realtime_voice < PET_REALTIME_VOICE_COUNT &&
        profile->cartesia_voice_gender >= PET_CARTESIA_VOICE_NEUTRAL &&
        profile->cartesia_voice_gender < PET_CARTESIA_VOICE_GENDER_COUNT;
}

pet_face_profile_t *pet_face_profiles_find(pet_face_profiles_t *profiles,
                                           const char *face_id)
{
    if (!profiles || !face_id) return NULL;
    for (uint8_t i = 0; i < profiles->count && i < PET_FACE_PROFILE_MAX; ++i) {
        if (!strcmp(profiles->entries[i].face_id, face_id)) {
            return &profiles->entries[i];
        }
    }
    return NULL;
}

const pet_face_profile_t *pet_face_profiles_find_const(
    const pet_face_profiles_t *profiles, const char *face_id)
{
    return pet_face_profiles_find((pet_face_profiles_t *)profiles, face_id);
}

void pet_face_profiles_touch(pet_face_profiles_t *profiles,
                             pet_face_profile_t *profile)
{
    if (!profiles || !profile) return;
    profiles->next_sequence++;
    if (!profiles->next_sequence) profiles->next_sequence = 1;
    profile->last_used = profiles->next_sequence;
}

bool pet_face_profiles_put(pet_face_profiles_t *profiles,
                           const pet_face_profile_t *profile)
{
    if (!profiles || !pet_face_profile_valid(profile)) return false;
    pet_face_profile_t *existing = pet_face_profiles_find(profiles,
                                                          profile->face_id);
    if (existing) {
        uint32_t last_used = existing->last_used;
        *existing = *profile;
        existing->last_used = last_used;
        pet_face_profiles_touch(profiles, existing);
        return true;
    }
    uint8_t index = profiles->count;
    if (profiles->count < PET_FACE_PROFILE_MAX) {
        profiles->count++;
    } else {
        uint32_t oldest = UINT32_MAX;
        index = UINT8_MAX;
        for (uint8_t i = 0; i < profiles->count; ++i) {
            if (profiles->entries[i].last_used < oldest) {
                oldest = profiles->entries[i].last_used;
                index = i;
            }
        }
        if (index == UINT8_MAX) return false;
    }
    profiles->entries[index] = *profile;
    pet_face_profiles_touch(profiles, &profiles->entries[index]);
    return true;
}
