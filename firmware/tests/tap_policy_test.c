#include <stdbool.h>

#include "pet_tap_policy.h"

int main(void)
{
    /* capturing, listen_pending, playing, thinking, answering */
    if (pet_tap_action(true, false, false, false, false) != PET_TAP_STOP_CAPTURE) return 1;
    if (pet_tap_action(true, true, true, true, true) != PET_TAP_STOP_CAPTURE) return 2;
    if (pet_tap_action(false, false, false, true, false) != PET_TAP_IGNORE_WAITING) return 3;
    if (pet_tap_action(false, false, true, false, true) != PET_TAP_STOP_ANSWER) return 4;
    if (pet_tap_action(false, false, false, false, true) != PET_TAP_STOP_ANSWER) return 5;
    if (pet_tap_action(false, false, false, false, false) != PET_TAP_START_LISTENING) return 6;
    /* A second tap during the listening cue cancels before the microphone opens. */
    if (pet_tap_action(false, true, false, false, false) != PET_TAP_CANCEL_LISTENING) return 7;
    if (pet_tap_action(false, true, true, true, true) != PET_TAP_CANCEL_LISTENING) return 8;
    return 0;
}
