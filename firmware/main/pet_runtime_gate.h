#pragma once
#include <stdatomic.h>
#include <stdbool.h>

/* Reservation, not a busy snapshot. No network/flash work holds the audio
 * mutex; a racing tap simply remains idle until the next checkpoint. */
typedef enum { PET_RESOURCE_FREE, PET_RESOURCE_VOICE, PET_RESOURCE_CONTROL } pet_resource_t;
typedef struct { atomic_int owner; } pet_runtime_gate_t;
static inline bool pet_runtime_claim(pet_runtime_gate_t *gate,pet_resource_t owner)
{
    int expected=PET_RESOURCE_FREE;
    return atomic_compare_exchange_strong(&gate->owner,&expected,owner);
}
static inline void pet_runtime_release(pet_runtime_gate_t *gate,pet_resource_t owner)
{
    int expected=owner;
    atomic_compare_exchange_strong(&gate->owner,&expected,PET_RESOURCE_FREE);
}
static inline bool pet_runtime_owns(pet_runtime_gate_t *gate,pet_resource_t owner)
{return atomic_load(&gate->owner)==(int)owner;}
