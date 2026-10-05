# Porting to another board

The firmware runs on ESP32-S3 boards with a touch display, a microphone and a
speaker. One build supports exactly one board, chosen by `AIPET_BOARD`.

## What a board needs

- **ESP32-S3 with at least 16 MB flash and 8 MB octal PSRAM.** The flash layout
  (`firmware/partitions-pocket.csv`) has two 3.5 MiB app slots, a 64 KiB pet
  journal and room for three pets of 3 MB each, all in the first 16 MB. A pet
  on screen is copied into PSRAM. A larger flash works; its upper part is unused.
- **A display and touch controller** with ESP-IDF `esp_lcd` drivers. The pet,
  drawn at 240 × 240, is scaled to fill the screen; menus scale from a 360 px
  layout and have a compact form below 300 px.
- **Audio** as on the supported boards: an ES8311 speaker codec and an ES7210
  microphone ADC on one I2S port, configured over I2C (`pet_audio_board.c`).
  Other codecs need changes there.
- Optional: a QMI8658 motion sensor for shake reactions, and a battery reading
  (`pet_battery.c`).

## Where a board lives

| File | What to add |
| --- | --- |
| `firmware/boards/<board>/` | A component for the board. `amoled_206` only depends on Waveshare's BSP from the component registry. `lcd_154` is a small BSP of our own, for a board without one. The 1.85B uses Waveshare's BSP from the `vendor/waveshare` submodule. |
| `firmware/CMakeLists.txt` | An `AIPET_BOARD` branch: the BSP component (`AIPET_BSP_COMPONENT`), the board directory and its dependency lockfile. |
| `firmware/dependencies.<board>.lock` | The pinned component versions. The first build writes it; commit it. |
| `firmware/main/CMakeLists.txt` | A `PET_BOARD_<NAME>` compile definition for the board. |
| `firmware/main/pet_board.h` | The board's profile: ID, hardware name, controllers, resolution, round or not, flash size. `pet_board_current()` returns it. |
| `firmware/main/pet_display.c` | How the display starts: through the BSP's `bsp_display_start_with_config()`, or from its panel and touch handles (`bsp_display_new()`, `bsp_touch_new()`), as the AMOLED and 1.54 do. |
| `firmware/main/pet_battery.c` | How the battery is read, if there is one. |
| `firmware/sdkconfig.<board>.defaults` | Board-only settings, if any. |
| `tools/build-firmware.sh` | The new `AIPET_BOARD` name, with its defaults file if it has one. |

`boards/lcd_154` shows the smallest set of BSP functions the firmware uses:
`bsp_i2c_init()`, `bsp_i2c_get_handle()`, `bsp_display_new()`,
`bsp_touch_new()`, `bsp_display_backlight_on()` and
`bsp_display_brightness_set()`, plus the pin macros in `include/bsp/esp-bsp.h`
and the `BSP_I2C_NUM`, `BSP_I2S_NUM` and `BSP_DISPLAY_BRIGHTNESS_LEDC_CH`
options in its `Kconfig`.

## Configuration

`firmware/sdkconfig.defaults` holds the settings every board shares: the
ESP32-S3 target, 16 MB QIO flash, octal PSRAM at 80 MHz, 240 MHz CPU, LVGL and
the TLS and network buffers. The release defaults that `tools/build-firmware.sh`
adds on top select the Pocket firmware, its partition table, the qualified
rollback bootloader profile and reproducible builds.

The bootloader and partition table are the same for every board; only the app
differs. Keep it that way, so boards share installation tooling. A firmware
image for one board refuses to update another board.

## Check it

- `tools/build-firmware.sh <board>` builds it.
- The host tests run the menus at several screen sizes:
  `python3 -m unittest discover -s firmware/tests -p 'test_*.py'`.
- `firmware/tools/ui_preview.py` renders every menu page with real LVGL for the
  supported boards. Add your board's flags there to preview yours.
- Installing from aipets.com, and pets from aipets.com, need the backend to
  know the board's hardware name. Pets installed over USB
  ([pet packs](pet-packs.md)) work on any board.
