# Third-party notices

The AI Pets firmware is Copyright 2026 Gauss Labs and licensed under the
[Apache License 2.0](LICENSE). It includes, and its builds link, work by others
under the licenses below. Those licenses, not ours, apply to that work. The
AI Pets name, logo and characters are covered by [TRADEMARKS.md](TRADEMARKS.md).

## In this repository

| Component | Where | License |
| --- | --- | --- |
| Share Tech Mono, © 2012 Carrois Type Design, Ralph du Carrois, Reserved Font Name "Share" | `firmware/third_party/share-tech-mono/` (font and `OFL.txt`); its converted glyphs in `firmware/main/pet_menu_font.c` | [SIL Open Font License 1.1](firmware/third_party/share-tech-mono/OFL.txt) |
| miniz 3.1.2 (inflate only, hardened), © 2013–2014 RAD Game Tools and Valve Software, © 2010–2014 Rich Geldreich and Tenacious Software LLC | `firmware/components/frame_player/vendor/miniz/` | MIT ([LICENSE](firmware/components/frame_player/vendor/miniz/LICENSE)) |
| UISFX 0.4.0 sounds: `connect`, `start`, `stop`, `complete`, `cancel`, `retry`, `error`, `gesture_shake`, `gesture_pop` | `firmware/main/sfx/` ([sources](firmware/main/sfx/manifest.json)) | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| Waveshare ESP32-S3-Touch-LCD-1.85B board support | `vendor/waveshare` (git submodule, pinned commit) | Apache-2.0 |

The menu font's copyright notice and license are compiled into every firmware
image and shown on the device's **ABOUT** page. `pet_menu_font.c` is a Modified
Version of the font, so it does not use the Reserved Font Name.

## Fetched at build time and linked into the firmware

Component versions are pinned by `firmware/dependencies.lock` (1.85B),
`firmware/dependencies.amoled_206.lock` and `firmware/dependencies.lcd_154.lock`.
Each component's license file is downloaded with it into
`firmware/managed_components/`.

| Component | License |
| --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf/tree/v5.5.3) 5.5.3, including the libraries it builds into the firmware (FreeRTOS, lwIP, Mbed TLS, newlib, wpa_supplicant, cJSON and others) | Apache-2.0, and each library's own license as listed in ESP-IDF's [COPYRIGHT.rst](https://github.com/espressif/esp-idf/blob/v5.5.3/docs/en/COPYRIGHT.rst) |
| [LVGL](https://github.com/lvgl/lvgl) 9.5.0 | MIT |
| Montserrat, built into LVGL (`lv_font_montserrat_14`), © 2011 The Montserrat Project Authors | SIL Open Font License 1.1 |
| Font Awesome Free symbols in that font, © 2022 Fonticons, Inc., Reserved Font Name "Font Awesome" | SIL Open Font License 1.1 |
| Espressif components: `esp_codec_dev`, `esp_lvgl_adapter`, `esp_websocket_client`, `esp_lcd_touch`, and per board `esp_lcd_st77916`, `esp_lcd_touch_cst816s`, `i2c_bus`, `esp_lcd_touch_ft5x06`, `esp_io_expander`, `esp_lvgl_port` | Apache-2.0 |
| Waveshare components: `qmi8658`, and for the AMOLED 2.06 `esp32_s3_touch_amoled_2_06` and `esp_lcd_sh8601` | Apache-2.0 |
| libpng 1.6.58 and zlib 1.3.2 (AMOLED 2.06 and 1.54 builds) | libpng License; zlib License |

The device's **ABOUT** page credits the fonts with their copyright notices and
shows the SIL Open Font License, which Montserrat and Font Awesome share with
the menu font.

## Fetched at build time, not linked

The component graph also downloads these, which the three builds do not link:
`esp_new_jpeg` 1.0.2 (ESPRESSIF MIT License, which permits use on Espressif
products only), FreeType 2.14.3 (FreeType License or GPL-2.0), and the
Apache-2.0 components `esp_lv_decoder`, `esp_lv_fs`, `esp_mmap_assets`,
`button`, `knob`, `bq27220`, `pcf85063a`, `esp_lcd_panel_io_additions` and
`cmake_utilities`. A change that links one of them must add it above.

## Tools

The Python tools in `firmware/tools` and `tools` use Pillow, NumPy and
soundfile when you install them. They are not part of this repository or the
firmware.
