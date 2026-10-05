#include "pet_sfx_plan.h"

#include <stddef.h>

void pet_sfx_plan_reset(pet_sfx_plan_t *plan)
{
    if (!plan) return;
    plan->decorative_count = 0;
    plan->has_priority = false;
}

static void drop_oldest_decoration(pet_sfx_plan_t *plan)
{
    for (uint8_t i = 1; i < plan->decorative_count; i++) plan->decorative[i - 1] = plan->decorative[i];
    plan->decorative_count--;
}

pet_sfx_plan_effect_t pet_sfx_plan_request(pet_sfx_plan_t *plan, pet_sfx_plan_item_t item,
                                           bool playing_decoration)
{
    pet_sfx_plan_effect_t effect = {0};
    if (!plan) {
        effect.refused = true;
        return effect;
    }
    if (item.conversation) {
        if (plan->has_priority) {
            effect.superseded = true;
            effect.superseded_item = plan->priority;
        }
        plan->priority = item;
        plan->has_priority = true;
        plan->decorative_count = 0;
        effect.stop_playing = playing_decoration;
        return effect;
    }
    if (plan->has_priority) {
        effect.refused = true;
        return effect;
    }
    if (plan->decorative_count == PET_SFX_PLAN_DECORATIVE_CAPACITY) {
        effect.superseded = true;
        effect.superseded_item = plan->decorative[0];
        drop_oldest_decoration(plan);
    }
    plan->decorative[plan->decorative_count++] = item;
    return effect;
}

bool pet_sfx_plan_next(pet_sfx_plan_t *plan, uint32_t now_ms, pet_sfx_plan_item_t *next)
{
    if (!plan || !next) return false;
    if (plan->has_priority) {
        *next = plan->priority;
        plan->has_priority = false;
        return true;
    }
    while (plan->decorative_count) {
        pet_sfx_plan_item_t item = plan->decorative[0];
        drop_oldest_decoration(plan);
        /* Unsigned subtraction keeps the age correct across the ms wrap. */
        if ((uint32_t)(now_ms - item.requested_ms) <= PET_SFX_PLAN_STALE_MS) {
            *next = item;
            return true;
        }
    }
    return false;
}

bool pet_sfx_plan_cancel(pet_sfx_plan_t *plan, pet_sfx_plan_item_t *cancelled)
{
    if (!plan) return false;
    bool had = plan->has_priority;
    if (had && cancelled) *cancelled = plan->priority;
    plan->has_priority = false;
    plan->decorative_count = 0;
    return had;
}
