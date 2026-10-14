# AI Pets firmware

Firmware for [AI Pets](https://aipets.com): a little animated companion that
lives on an ESP32-S3 touch board and talks with you.

- One installed pet, with an animation pack up to 10,739,712 bytes.
- Tap to talk. The device records your question and plays the pet's spoken
  answer. Speech recognition, the pet's personality, memory and voice run on
  the backend it is linked to, not on the ESP32.
- Swipe down for the control deck: Wi-Fi, volume, brightness, linking, a system
  log and the open-source notices. Swipe up to leave it.
- Long press to pet the pet. On a backend that offers stories, the pet then
  starts or pauses a story. Story screens show character progress, blessings
  and the current adventure; compatible backends can add animated events,
  atmosphere loops and sound cues.
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
QMI8658 motion sensor. These exact models are the supported build targets;
other ESP32 boards and similarly named Waveshare models are not interchangeable.
See [porting](docs/porting.md) to add another board.

## Current source

The current source uses firmware epoch **8** (`aipet-8.0.0-240`) and the
`one-pet-10m-v5` partition layout: one factory application, one pet and a
dedicated 2 MiB story sound bank. **Firmware updates use USB only.** Signed
pet and sound content can still arrive over Wi-Fi from a compatible backend.

This is a source update, not a new binary release. Older release downloads
use an older layout; do not mix their flashing instructions with this source.
Read the [migration instructions](docs/getting-started.md#flash-and-migrate)
and [changelog](CHANGELOG.md) before updating an existing device.

## Three ways to start

1. **Install from aipets.com.** Connect the board by USB, sign in at
   [aipets.com](https://aipets.com) in Chrome or Edge, open **Devices** and use
   **Install or update firmware**. Select and verify the exact board model.
   The hosted installer has its own release schedule and may not yet offer
   this source revision.
2. **Build it yourself** with ESP-IDF 5.5.3 and flash it with esptool:
   [getting started](docs/getting-started.md). Use only board-matched images
   and the installation metadata that accompanies them.
3. **Connect it to your own backend** by building with your backend's address
   and release key: [backend settings](docs/backend.md).

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

For backend integration, the firmware's HTTP and session parsers describe
the protocol it accepts; see [backend settings](docs/backend.md).

## License

Copyright 2026 [Gauss Labs](https://gausslabs.com). The firmware is open source
under the [Apache License 2.0](LICENSE). Fonts, sounds and libraries it includes
keep their own licenses ([third-party notices](THIRD_PARTY_NOTICES.md)). The
AI Pets name and logo and the official characters are not licensed, and the
logo and characters are not included: the boot screen shows a plain ring in
the logo's place. Modified builds must not use the AI Pets name or logo
([trademarks](TRADEMARKS.md)).
