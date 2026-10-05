#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Supported hardware identities, not a list of interchangeable LCD controllers.
 * Pins, rotation and electrical initialization belong to each board's BSP.
 * Adding an entry also requires its BSP, UI and signed cloud release support. */
typedef struct {
    const char *id;
    const char *hardware;
    const char *display_controller;
    const char *display_bus;
    const char *touch_controller;
    uint16_t width;
    uint16_t height;
    bool round;
    uint32_t flash_bytes;
} pet_board_profile_t;

static const pet_board_profile_t pet_board_profiles[] = {
    {
        .id = "waveshare-esp32-s3-touch-lcd-1.85b",
        .hardware = "Waveshare ESP32-S3-Touch-LCD-1.85B",
        .display_controller = "ST77916",
        .display_bus = "QSPI",
        .touch_controller = "CST816S",
        .width = 360,
        .height = 360,
        .round = true,
        .flash_bytes = 0x1000000u,
    },
    {
        .id = "waveshare-esp32-s3-touch-amoled-2.06",
        .hardware = "Waveshare ESP32-S3-Touch-AMOLED-2.06",
        .display_controller = "CO5300",
        .display_bus = "QSPI",
        .touch_controller = "FT3168",
        .width = 410,
        .height = 502,
        .round = false,
        .flash_bytes = 0x2000000u,
    },
    {
        .id = "waveshare-esp32-s3-touch-lcd-1.54",
        .hardware = "Waveshare ESP32-S3-Touch-LCD-1.54",
        .display_controller = "ST7789",
        .display_bus = "SPI",
        .touch_controller = "CST816S",
        .width = 240,
        .height = 240,
        .round = false,
        .flash_bytes = 0x1000000u,
    },
};

static inline const pet_board_profile_t *pet_board_find(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(pet_board_profiles) / sizeof(pet_board_profiles[0]); i++)
    {
        if (!strcmp(id, pet_board_profiles[i].id)) return &pet_board_profiles[i];
    }
    return NULL;
}

static inline const pet_board_profile_t *pet_board_current(void)
{
    /* The build links exactly one board BSP; never guess from USB identity. */
#ifdef PET_BOARD_AMOLED_206
    return &pet_board_profiles[1];
#elif defined(PET_BOARD_LCD_154)
    return &pet_board_profiles[2];
#else
    return &pet_board_profiles[0];
#endif
}
