#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The keys of a config v2 `settings` document, as the cloud's strict schema
 * has them: the eight required settings and the optional speechMouthOffsetMs.
 * Any other key rejects the document. */
static inline bool pet_config_v2_setting_known(const char *key)
{
    static const char *const known[] = {
        "volume", "brightness", "shakeSensitivity", "recordingTimeoutSeconds",
        "animationProfile", "aiPetId", "faceId", "speechProfile", "speechMouthOffsetMs",
    };
    for (size_t i = 0; key && i < sizeof(known) / sizeof(known[0]); ++i) {
        if (!strcmp(key, known[i])) return true;
    }
    return false;
}
