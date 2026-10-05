#pragma once

#include <stdint.h>

typedef enum {
    PET_AI_MODE_OPENROUTER = 0,
    PET_AI_MODE_OPENAI_REALTIME,
    PET_AI_MODE_CARTESIA_AGENT,
    PET_AI_MODE_COUNT,
} pet_ai_mode_t;

typedef enum {
    PET_REALTIME_MODEL_2_1_MINI = 0,
    PET_REALTIME_MODEL_2_1,
    PET_REALTIME_MODEL_1_5,
    PET_REALTIME_MODEL_COUNT,
} pet_realtime_model_t;

typedef enum {
    PET_REALTIME_VOICE_CEDAR = 0,
    PET_REALTIME_VOICE_MARIN,
    PET_REALTIME_VOICE_ALLOY,
    PET_REALTIME_VOICE_ASH,
    PET_REALTIME_VOICE_BALLAD,
    PET_REALTIME_VOICE_CORAL,
    PET_REALTIME_VOICE_ECHO,
    PET_REALTIME_VOICE_SAGE,
    PET_REALTIME_VOICE_SHIMMER,
    PET_REALTIME_VOICE_VERSE,
    PET_REALTIME_VOICE_COUNT,
} pet_realtime_voice_t;

typedef enum {
    PET_REALTIME_BOOST_OFF = 0,
    PET_REALTIME_BOOST_3_DB,
    PET_REALTIME_BOOST_6_DB,
    PET_REALTIME_BOOST_9_DB,
    PET_REALTIME_BOOST_COUNT,
} pet_realtime_boost_t;

typedef enum {
    PET_CARTESIA_VOICE_NEUTRAL = 0,
    PET_CARTESIA_VOICE_MALE,
    PET_CARTESIA_VOICE_FEMALE,
    PET_CARTESIA_VOICE_GENDER_COUNT,
} pet_cartesia_voice_gender_t;

#define PET_REALTIME_BOOST_DEFAULT PET_REALTIME_BOOST_6_DB

static inline const char *pet_ai_mode_name(pet_ai_mode_t mode)
{
    switch (mode) {
        case PET_AI_MODE_OPENAI_REALTIME: return "openai-realtime";
        case PET_AI_MODE_CARTESIA_AGENT: return "cartesia-agent";
        case PET_AI_MODE_OPENROUTER:
        default: return "openrouter";
    }
}

static inline const char *pet_realtime_model_name(pet_realtime_model_t model)
{
    switch (model) {
        case PET_REALTIME_MODEL_2_1: return "gpt-realtime-2.1";
        case PET_REALTIME_MODEL_1_5: return "gpt-realtime-1.5";
        case PET_REALTIME_MODEL_2_1_MINI:
        default: return "gpt-realtime-2.1-mini";
    }
}

static inline const char *pet_realtime_voice_name(pet_realtime_voice_t voice)
{
    static const char *const names[PET_REALTIME_VOICE_COUNT] = {
        "cedar", "marin", "alloy", "ash", "ballad", "coral", "echo", "sage", "shimmer", "verse",
    };
    return voice < PET_REALTIME_VOICE_COUNT ? names[voice] : names[0];
}

static inline const char *pet_cartesia_voice_gender_name(pet_cartesia_voice_gender_t gender)
{
    static const char *const names[PET_CARTESIA_VOICE_GENDER_COUNT] = {
        "neutral", "male", "female",
    };
    return gender < PET_CARTESIA_VOICE_GENDER_COUNT ? names[gender] : names[0];
}

static inline const char *pet_realtime_boost_name(pet_realtime_boost_t boost)
{
    static const char *const names[PET_REALTIME_BOOST_COUNT] = {
        "off", "+3 dB", "+6 dB", "+9 dB",
    };
    return boost >= PET_REALTIME_BOOST_OFF && boost < PET_REALTIME_BOOST_COUNT ?
        names[boost] : names[PET_REALTIME_BOOST_DEFAULT];
}
