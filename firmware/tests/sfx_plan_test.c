#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pet_sfx_plan.h"

#define CHECK(condition, code) do { if (!(condition)) { fprintf(stderr, "sfx plan check %d failed\n", code); return code; } } while (0)

static pet_sfx_plan_item_t decoration(uint8_t cue, uint32_t at_ms)
{
    return (pet_sfx_plan_item_t){ .cue = cue, .requested_ms = at_ms };
}

static pet_sfx_plan_item_t conversation(uint8_t cue, uint32_t token, uint32_t at_ms)
{
    return (pet_sfx_plan_item_t){ .cue = cue, .conversation = true, .token = token, .requested_ms = at_ms };
}

int main(void)
{
    pet_sfx_plan_t plan;
    pet_sfx_plan_item_t next;
    pet_sfx_plan_effect_t effect;

    /* Decoration plays in order. */
    pet_sfx_plan_reset(&plan);
    effect = pet_sfx_plan_request(&plan, decoration(1, 0), false);
    CHECK(!effect.refused && !effect.stop_playing && !effect.superseded, 1);
    pet_sfx_plan_request(&plan, decoration(2, 10), false);
    CHECK(pet_sfx_plan_next(&plan, 20, &next) && next.cue == 1, 2);
    CHECK(pet_sfx_plan_next(&plan, 20, &next) && next.cue == 2, 3);
    CHECK(!pet_sfx_plan_next(&plan, 20, &next), 4);

    /* A full decoration queue drops its oldest entry. */
    pet_sfx_plan_reset(&plan);
    for (uint8_t cue = 1; cue <= PET_SFX_PLAN_DECORATIVE_CAPACITY; cue++) pet_sfx_plan_request(&plan, decoration(cue, 0), false);
    effect = pet_sfx_plan_request(&plan, decoration(9, 0), false);
    CHECK(effect.superseded && effect.superseded_item.cue == 1 && !effect.refused, 5);
    CHECK(pet_sfx_plan_next(&plan, 0, &next) && next.cue == 2, 6);

    /* Stale decoration is discarded, including across the millisecond wrap. */
    pet_sfx_plan_reset(&plan);
    pet_sfx_plan_request(&plan, decoration(1, 0xfffffff0u), false);
    pet_sfx_plan_request(&plan, decoration(2, 0xfffffff0u + 10u), false);
    CHECK(pet_sfx_plan_next(&plan, 0xfffffff0u + PET_SFX_PLAN_STALE_MS + 5u, &next) && next.cue == 2, 7);
    pet_sfx_plan_request(&plan, decoration(3, 100), false);
    CHECK(!pet_sfx_plan_next(&plan, 100 + PET_SFX_PLAN_STALE_MS + 1u, &next), 8);

    /* A conversation cue stops playing decoration, clears queued decoration
     * and plays first. */
    pet_sfx_plan_reset(&plan);
    pet_sfx_plan_request(&plan, decoration(1, 0), true);
    effect = pet_sfx_plan_request(&plan, conversation(5, 77, 0), true);
    CHECK(effect.stop_playing && !effect.superseded, 9);
    CHECK(pet_sfx_plan_next(&plan, 0, &next) && next.cue == 5 && next.token == 77, 10);
    CHECK(!pet_sfx_plan_next(&plan, 0, &next), 11);

    /* A conversation cue does not interrupt another conversation cue. */
    pet_sfx_plan_reset(&plan);
    effect = pet_sfx_plan_request(&plan, conversation(5, 0, 0), false);
    CHECK(!effect.stop_playing, 12);

    /* Decoration is refused while a conversation cue waits. */
    pet_sfx_plan_reset(&plan);
    pet_sfx_plan_request(&plan, conversation(5, 1, 0), false);
    effect = pet_sfx_plan_request(&plan, decoration(1, 0), false);
    CHECK(effect.refused, 13);

    /* The latest conversation cue wins; the earlier one is reported. */
    pet_sfx_plan_reset(&plan);
    pet_sfx_plan_request(&plan, conversation(5, 41, 0), false);
    effect = pet_sfx_plan_request(&plan, conversation(6, 42, 0), false);
    CHECK(effect.superseded && effect.superseded_item.token == 41, 14);
    CHECK(pet_sfx_plan_next(&plan, 1000, &next) && next.token == 42, 15);

    /* Conversation cues never go stale. */
    pet_sfx_plan_reset(&plan);
    pet_sfx_plan_request(&plan, conversation(5, 7, 0), false);
    CHECK(pet_sfx_plan_next(&plan, 60000, &next) && next.token == 7, 16);

    /* Cancel drops everything and returns the waiting conversation cue. */
    pet_sfx_plan_reset(&plan);
    pet_sfx_plan_request(&plan, decoration(1, 0), false);
    pet_sfx_plan_request(&plan, conversation(5, 99, 0), false);
    pet_sfx_plan_item_t cancelled = {0};
    CHECK(pet_sfx_plan_cancel(&plan, &cancelled) && cancelled.token == 99, 17);
    CHECK(!pet_sfx_plan_next(&plan, 0, &next), 18);
    CHECK(!pet_sfx_plan_cancel(&plan, &cancelled), 19);
    return 0;
}
