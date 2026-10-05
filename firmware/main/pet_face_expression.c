#include "pet_face.h"
#include <string.h>

bool pet_expression_from_wire(const char *name, pet_expression_t *value)
{
    static const char *const names[PET_EXPRESSION_COUNT] = {
        "idle", "happy", "curious", "surprised", "sleepy", "concerned",
        "excited", "shy", "angry", "confused", "determined", "disgust",
        "embarrassed", "fear", "sad",
    };
    if (!name || !value) return false;
    for (unsigned i = 0; i < PET_EXPRESSION_COUNT; ++i) {
        if (!strcmp(name, names[i])) {
            *value = (pet_expression_t)i;
            return true;
        }
    }
    return false;
}
