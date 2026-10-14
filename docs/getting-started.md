# Getting started

Build for your exact [supported board](../README.md#supported-boards), then
install over USB. The hosted **Install or update firmware** page at
[aipets.com](https://aipets.com) is the simpler option when it offers a
compatible release for your board and current layout.

## What you need

- A supported ESP32-S3 board and a USB-C data cable.
- [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/index.html),
  Git and Python 3.

## Get the source

```sh
git clone --recursive https://github.com/veiovi/aipets-firmware.git
cd aipets-firmware
```

For an existing clone, run `git submodule update --init`. The submodule is
Waveshare's board support for the 1.85B.

## Build

Activate your ESP-IDF 5.5.3 environment, then run from the repository root:

```sh
tools/build-firmware.sh waveshare-esp32-s3-touch-lcd-1.85b
```

Replace the board name with its exact `AIPET_BOARD` value from the README.
Dependencies are pinned in `firmware/dependencies*.lock`; the first build
downloads the required components. Outputs go to `build/<board>/`.

The script applies the base, vNext, vNext release, Pocket and Pocket release
defaults, plus AMOLED defaults for that board. Its optional second argument
sets the build directory; a third defaults file overrides configuration for
[your backend](backend.md). Use a fresh build directory when changing board
or defaults. Reproducible-build settings remove build timestamps and source
paths; compare hashes with identical source, toolchain and configuration.

The current partition table contains:

| Region | Offset | Size |
| --- | --- | --- |
| NVS settings | `0x9000` | `0x6000` |
| Factory application | `0x30000` | `0x380000` |
| Pet journal | `0x3b0000` | `0x10000` |
| Story sound bank | `0x3c0000` | `0x200000` |
| Pet store | `0x5c0000` | `0xa40000` |

The authoritative complete table is
[`firmware/partitions-pocket.csv`](../firmware/partitions-pocket.csv).
There is no second application slot or OTA selection partition in this layout.

### Host tests

```sh
python3 -m unittest discover -s firmware/tests -p 'test_*.py'
```

Tests need Python 3 and a C compiler. Some require LVGL, ESP-IDF or Pillow;
inspect reported skips. `LVGL_SOURCE_DIR` can point to
`firmware/managed_components/lvgl__lvgl` after a build, and `IDF_PATH` to
your ESP-IDF installation. For a code change, run the affected modules first.

## Flash and migrate

**The old dual-application layout is incompatible with this app's data map.**
In particular, `0x3b0000` is now the pet journal, not a second application.
Never use old two-slot flash commands, and never write this app alone over
an older partition table. Firmware updates are USB-only in the current layout.

For an existing device, prefer the board-matched hosted installer when it
explicitly supports migration from your installed layout. A manual migration
that preserves data requires a verified backup and a layout-aware migration
procedure; this repository does not provide a one-command preserving migration.
Do not restore an old raw NVS or pet journal into an incompatible layout.
Backups contain Wi-Fi credentials and device credentials: keep them private.

### Fresh installation or deliberate reset

This procedure **erases all device settings, Wi-Fi credentials, account linking,
pets, sound content and the vendor demo**. Reconnect and reinstall content
afterward. Use it only when starting fresh or when you accept that loss.

Replace `PORT` with your device's serial port. Use esptool from your ESP-IDF
environment; enter download mode as described in your board's Waveshare guide
if it cannot connect. From the repository root after building:

```sh
esptool.py --chip esp32s3 -p PORT erase_flash
esptool.py --chip esp32s3 -p PORT write_flash \
  0x0 build/waveshare-esp32-s3-touch-lcd-1.85b/bootloader/bootloader.bin \
  0x8000 build/waveshare-esp32-s3-touch-lcd-1.85b/partition_table/partition-table.bin \
  0x30000 build/waveshare-esp32-s3-touch-lcd-1.85b/aipet_firmware.bin
```

Use the directory for your exact board. For packaged images, check
`SHA256SUMS`, the board identity and the layout in `manifest.json` first.
A full image is for a fresh/reset installation; use its recorded offsets.

### Updating within the same layout

Only after confirming that the device already uses `one-pet-10m-v5`, an
app-only write at `0x30000` updates firmware without writing settings or pet
regions. Use a build for the same board and a compatible firmware epoch.
Do not erase flash, write the full image or downgrade as part of this update.
Verify boot and a real interaction afterward; a successful USB write alone
does not prove the update works.

## First boot

1. Choose a Wi-Fi network, or **HIDDEN** to enter its name, then its password.
2. [Link the device](../README.md#linking-a-device) to your backend account.
3. Install one pet from the backend, or prepare an unsigned animation pack
   over USB ([pet packs](pet-packs.md)).

Swipe down for the control deck. The system log shows device activity and
**ABOUT** lists open-source notices. From `firmware/`, use
`idf.py -B ../build/<board> -p PORT monitor` to read serial output. Review logs
for private details before sharing them.
