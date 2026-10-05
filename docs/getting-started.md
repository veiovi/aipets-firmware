# Getting started

Build the firmware for your board, flash it with esptool and link the device.
To install without any tools, use **Install or update firmware** on aipets.com
instead ([README](../README.md#three-ways-to-start)).

## What you need

- A [supported board](../README.md#supported-boards) and a USB-C data cable.
- [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/index.html),
  or Docker to build in the `espressif/idf:v5.5.3` image.
- Git and Python 3.

## Get the source

Clone this repository with its submodule, Waveshare's board support for the
1.85B:

```sh
git clone --recursive <this repository's URL>
```

In an existing clone, run `git submodule update --init`.

## Build

In an ESP-IDF 5.5.3 shell, from the repository root:

```sh
. ~/esp/esp-idf-v5.5.3/export.sh    # wherever you installed ESP-IDF
tools/build-firmware.sh waveshare-esp32-s3-touch-lcd-1.85b
```

Use the `AIPET_BOARD` name of your board from the
[board table](../README.md#supported-boards). The first build downloads the
board's components, pinned by `firmware/dependencies*.lock`. The results are
in `build/<board>/`:

| File | Flash offset |
| --- | --- |
| `bootloader/bootloader.bin` | `0x0` |
| `partition_table/partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0x10000` |
| `aipet_firmware.bin` | `0x30000` and `0x3b0000` (both app slots) |

Or build in Docker, as the CI does:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.3 \
  tools/build-firmware.sh waveshare-esp32-s3-touch-lcd-1.54
```

`tools/build-firmware.sh` applies the release configuration:
`firmware/sdkconfig.defaults`, then the `vnext`, `vnext.release`, `pocket` and
`pocket.release` defaults, and `sdkconfig.amoled_206.defaults` for the AMOLED.
Builds are reproducible (`CONFIG_APP_REPRODUCIBLE_BUILD`): the same commit
gives byte-identical images on another machine with ESP-IDF 5.5.3.
A third argument adds your own defaults file last
([backend settings](backend.md)).

### Host tests

The tests in `firmware/tests` run on your computer with Python 3 and a C
compiler:

```sh
python3 -m unittest discover -s firmware/tests -p 'test_*.py'
```

Tests that need LVGL, ESP-IDF or Pillow skip without them. Set
`LVGL_SOURCE_DIR` (for example `firmware/managed_components/lvgl__lvgl` after a
build) and `IDF_PATH` to run them all.

## Flash

esptool comes with ESP-IDF (`esptool.py`), or install it with
`pip install esptool`. Replace `PORT` with the board's serial port, for example
`/dev/cu.usbmodem1101` on macOS, `/dev/ttyACM0` on Linux or `COM5` on Windows.
If esptool can't connect, put the board in download mode as Waveshare's
guide for your board describes.

**A new board, or starting over.** This removes everything on the board,
including Waveshare's demo, Wi-Fi settings and pets:

```sh
esptool.py --chip esp32s3 -p PORT erase_flash
esptool.py --chip esp32s3 -p PORT write_flash 0x0 aipets-firmware-<board>-<version>-full.bin
```

For your own build, write its files instead of the full image, from
`build/<board>/`:

```sh
esptool.py --chip esp32s3 -p PORT write_flash 0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin 0x10000 ota_data_initial.bin \
  0x30000 aipet_firmware.bin 0x3b0000 aipet_firmware.bin
```

**Updating a board that already runs this firmware** keeps its Wi-Fi, link and
pets. Write only the app, to both app slots, and don't go back to an older
release:

```sh
esptool.py --chip esp32s3 -p PORT write_flash \
  0x30000 aipets-firmware-<board>-<version>-app.bin \
  0x3b0000 aipets-firmware-<board>-<version>-app.bin
```

Check downloaded images against the release's `SHA256SUMS` first.

## First boot

1. The boot screen checks the display, touch, Wi-Fi and pets.
2. **WI-FI**: choose your network, or **HIDDEN** to type its name, then type
   the password on the **JOIN** keyboard.
3. **LINK**: the device shows its link code. [Link it](../README.md#linking-a-device)
   on aipets.com, or on your own backend.
4. Without pets, the device stays in its menu. Install a pet from aipets.com,
   or over USB ([pet packs](pet-packs.md)).

Swipe down to open the control deck at any time. Its system log shows what the
device is doing, and **ABOUT** lists the open-source notices. To watch the
serial log, run `idf.py -B ../build/<board> -p PORT monitor` from `firmware/`.
