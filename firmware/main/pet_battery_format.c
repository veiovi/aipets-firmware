#include "pet_battery.h"

typedef struct {
    char *text;
    size_t capacity;
    size_t used;
} text_builder_t;

static void append_char(text_builder_t *builder, char value)
{
    if (builder->used + 1 < builder->capacity) {
        builder->text[builder->used++] = value;
        builder->text[builder->used] = '\0';
    }
}

static void append_text(text_builder_t *builder, const char *value)
{
    while (value && *value) append_char(builder, *value++);
}

static void append_uint(text_builder_t *builder, unsigned value,
                        unsigned minimum_digits)
{
    char reversed[10];
    unsigned count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(reversed));
    while (count < minimum_digits && count < sizeof(reversed))
        reversed[count++] = '0';
    while (count) append_char(builder, reversed[--count]);
}

void pet_battery_format_detail(const pet_battery_snapshot_t *snapshot,
                               char *text, size_t capacity)
{
    if (!text || !capacity) return;
    if (!snapshot || !snapshot->valid) {
        text_builder_t builder = { .text = text, .capacity = capacity };
        text[0] = '\0';
        append_text(&builder, "Battery unavailable");
        return;
    }
    const char *state = snapshot->state == PET_BATTERY_CHARGING ? "Charging" :
        snapshot->state == PET_BATTERY_FULL ? "Full" :
        snapshot->state == PET_BATTERY_DISCHARGING ? "Discharging" : "Connected";
    text_builder_t builder = { .text = text, .capacity = capacity };
    text[0] = '\0';
    append_text(&builder, state);
    /* AXP2101 exposes a percentage, but no measured capacity or ETA. */
    if (!snapshot->full_mah) return;
    append_text(&builder, " | ");
    append_uint(&builder, snapshot->remaining_mah, 1);
    append_char(&builder, '/');
    append_uint(&builder, snapshot->full_mah, 1);
    append_text(&builder, " mAh");
    if (!snapshot->eta_valid) return;
    append_text(&builder, " | ");
    append_uint(&builder, snapshot->eta_minutes / 60, 1);
    append_char(&builder, 'h');
    append_char(&builder, ' ');
    append_uint(&builder, snapshot->eta_minutes % 60, 2);
    append_text(&builder, snapshot->state == PET_BATTERY_CHARGING ?
                "m to full" : "m left");
}
