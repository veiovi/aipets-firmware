# AI Pets firmware

Firmware for [AI Pets](https://aipets.com): a little animated companion that
lives on an ESP32-S3 touch board and talks with you.

- Up to three pets live on the device. Swipe left or right to switch.
- Tap to talk. The device records your question and plays the pet's spoken
  answer. Speech recognition, the pet's personality, memory and voice run on
  the backend it is linked to, not on the ESP32.
- Swipe down for the control deck: Wi-Fi, volume, brightness, linking, a system
  log and the open-source notices. Swipe up to leave it.
- Long press to pet the pet. On a backend that offers stories, the pet then
  tells you what is happening in its own world, and you answer by voice.
- Shake the device for a reaction.
- Pets are `.aipetframes` animation packs. You can make your own
  ([pet packs](docs/pet-packs.md)).

## Supported boards

| Board | `AIPET_BOARD` | Display | Touch | Flash / PSRAM |
| --- | --- | --- | --- | --- |
| [Waveshare ESP32-S3-Touch-LCD-1.85B](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B) (reference) | `waveshare-esp32-s3-touch-lcd-1.85b` | 1.85" round LCD, 360 × 360, ST77916 (QSPI) | CST816S | 16 MB / 8 MB |
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | `waveshare-esp32-s3-touch-amoled-2.06` | 2.06" AMOLED, 410 × 502, CO5300 (QSPI) | FT3168 | 32 MB, 16 MB used / 8 MB |
| [Waveshare ESP32-S3-Touch-LCD-1.54](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54) | `waveshare-esp32-s3-touch-lcd-1.54` | 1.54" LCD, 240 × 240, ST7789 (SPI) | CST816S | 16 MB / 8 MB |

All three have microphones and a speaker codec (ES7210 and ES8311) and a
QMI8658 motion sensor. Another ESP32-S3 board with 16 MB flash and 8 MB PSRAM
can be added: see [porting](docs/porting.md).

## Three ways to start

1. **Install from aipets.com.** Connect the board by USB, sign in at
   [aipets.com](https://aipets.com) in Chrome or Edge, open **Devices** and use
   **Install or update firmware**. The page installs the build for the board
   you connect.
2. **Build it yourself** with ESP-IDF 5.5.3 and flash it with esptool:
   [getting started](docs/getting-started.md). Each GitHub release of this
   repository also has ready-made images for every board.
3. **Connect it to your own backend** by building with your backend's address
   and release key: [backend settings](docs/backend.md). A self-hosted AI Pets
   backend for Docker is coming soon.

## Linking a device

On first start the device asks for Wi-Fi: pick a network and type its password
on the screen. Then it shows a link code such as `K7QX-9MPA` and
`AIPETS.COM/LINK`.

1. Sign in at [aipets.com/link](https://aipets.com/link) and type the code.
2. The device asks **LINK TO** your account's name, next to a three-digit
   number. aipets.com shows a number too.
3. If the numbers match, tap **LINK**. Otherwise tap **NOT ME**: someone else
   typed your code, and the device shows a new one.

A code expires after ten minutes and the device shows a new one; after three
unused codes, tap the code to get another. You can also tap **TYPE A CODE**
and enter the four-character code that aipets.com shows under **Devices**.

## Repository layout

| Path | What it holds |
| --- | --- |
| `firmware/main` | The application: display, touch, audio, Wi-Fi, setup, linking, pets, updates |
| `firmware/components` | `frame_player` (plays `.aipetframes`) and `face_core` |
| `firmware/boards`, `vendor/waveshare` | Board support (the 1.85B's BSP is a submodule) |
| `firmware/tests` | Host tests that need no board |
| `firmware/tools` | USB pet install images, menu previews, menu assets, partition layouts |
| `tools` | Firmware build, release packaging and sound tools |

The device↔cloud wire format is defined by the `device-protocol` package of the
AI Pets cloud backend, which will be published with the self-hosted backend.

## License

Copyright 2026 [Gauss Labs](https://gausslabs.com). The firmware is open source
under the [Apache License 2.0](LICENSE). Fonts, sounds and libraries it includes
keep their own licenses ([third-party notices](THIRD_PARTY_NOTICES.md)). The
AI Pets name and logo and the official characters are not licensed, and the
logo and characters are not included: the boot screen shows a plain ring in
the logo's place. Modified builds must not use the AI Pets name or logo
([trademarks](TRADEMARKS.md)).
