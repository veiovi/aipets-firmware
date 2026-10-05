#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Contact-noise filter for the 8 ms touch reader. The CST816S occasionally
 * loses one coordinate read in the middle of a press; reporting that as a
 * release would split one press into two taps. A press therefore ends only
 * after PET_TOUCH_RELEASE_READS reads in a row without contact (16 ms). */
#define PET_TOUCH_RELEASE_READS 2

typedef struct {
    bool tracking; /* a press may be in progress: coordinates are read */
    bool pressed;  /* a read found contact in this press */
    uint8_t missed; /* consecutive reads without contact */
} pet_touch_filter_t;

/* Whether this cycle should read coordinates: the controller signalled or
 * holds its IRQ line, or a press is still being tracked. */
static inline bool pet_touch_filter_wants_read(pet_touch_filter_t *filter, bool irq)
{
    if (irq) filter->tracking = true;
    return filter->tracking;
}

/* The result of this cycle's read; returns whether the screen is pressed.
 * Only a press that has had contact is held through a lost read: before its
 * first coordinates there is no point to report. */
static inline bool pet_touch_filter_update(pet_touch_filter_t *filter, bool contact)
{
    if (contact) {
        filter->pressed = true;
        filter->missed = 0;
        return true;
    }
    if (filter->pressed && ++filter->missed < PET_TOUCH_RELEASE_READS) return true;
    filter->tracking = filter->pressed = false;
    filter->missed = 0;
    return false;
}
