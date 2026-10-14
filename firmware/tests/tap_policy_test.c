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
    /* holdable, held, since the last tap: an answer that can be held holds at
     * the first tap and plays on at the next; a held one always plays on. */
    if (pet_answer_tap(true, false, 5000000) != PET_ANSWER_HOLD) return 9;
    if (pet_answer_tap(true, true, PET_TAP_DOUBLE_US) != PET_ANSWER_PLAY_ON) return 10;
    if (pet_answer_tap(false, true, 2000000) != PET_ANSWER_PLAY_ON) return 11;
    /* A double tap stops it, held or not; one that can't be held stops as before. */
    if (pet_answer_tap(true, false, PET_TAP_DOUBLE_US - 1) != PET_ANSWER_STOP) return 12;
    if (pet_answer_tap(true, true, 120000) != PET_ANSWER_STOP) return 13;
    if (pet_answer_tap(false, false, 5000000) != PET_ANSWER_STOP) return 14;
    /* held, listened, capturing, listen_pending, answering, thinking: held, an
     * idle pet listens and a speaking one stops first; one listen per hold. */
    if (pet_talk_action(true, false, false, false, false, false) != PET_TALK_LISTEN) return 15;
    if (pet_talk_action(true, false, false, false, true, false) != PET_TALK_STOP_ANSWER) return 16;
    if (pet_talk_action(true, true, false, false, false, false) != PET_TALK_NONE) return 17;
    if (pet_talk_action(true, false, false, false, false, true) != PET_TALK_NONE) return 18;
    /* Holding never ends a listen; releasing sends it, or cancels it in its cue. */
    if (pet_talk_action(true, false, true, false, false, false) != PET_TALK_NONE) return 19;
    if (pet_talk_action(false, true, true, false, false, false) != PET_TALK_SEND) return 20;
    if (pet_talk_action(false, true, false, true, false, false) != PET_TALK_CANCEL_LISTENING) return 21;
    if (pet_talk_action(false, true, false, false, true, true) != PET_TALK_NONE) return 22;
    return 0;
}
