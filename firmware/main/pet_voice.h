#pragma once

#include <stdint.h>

typedef enum {
    PET_VOICE_PUCK = 0,
    PET_VOICE_KORE,
    PET_VOICE_CHARON,
    PET_VOICE_FENRIR,
    PET_VOICE_AOEDE,
    PET_VOICE_LEDA,
    PET_VOICE_COUNT,
} pet_voice_t;

static inline const char *pet_voice_name(pet_voice_t voice)
{
    switch (voice) {
        case PET_VOICE_KORE: return "Kore";
        case PET_VOICE_CHARON: return "Charon";
        case PET_VOICE_FENRIR: return "Fenrir";
        case PET_VOICE_AOEDE: return "Aoede";
        case PET_VOICE_LEDA: return "Leda";
        case PET_VOICE_PUCK:
        default: return "Puck";
    }
}
