#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PET_BATTERY_DISCHARGING,
    PET_BATTERY_CHARGING,
    PET_BATTERY_FULL,
    PET_BATTERY_IDLE,
} pet_battery_state_t;

typedef struct {
    bool valid;
    bool eta_valid;
    uint8_t percent;
    uint16_t remaining_mah;
    uint16_t full_mah;
    uint16_t eta_minutes;
    int16_t average_current_ma;
    pet_battery_state_t state;
} pet_battery_snapshot_t;

typedef void (*pet_battery_callback_t)(const pet_battery_snapshot_t *snapshot);

/* Adds a read-only BQ27220 or AXP2101 client to the selected board's BSP I2C
 * bus. It never resets or configures the gauge, charging policy, or rails. */
esp_err_t pet_battery_start(pet_battery_callback_t callback);
void pet_battery_set_enabled(bool enabled);
esp_err_t pet_battery_read(pet_battery_snapshot_t *snapshot);
void pet_battery_format_detail(const pet_battery_snapshot_t *snapshot,
                               char *text, size_t capacity);
