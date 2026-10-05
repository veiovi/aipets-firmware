#!/bin/sh
# Build the AI Pets firmware for one board. Run it in an ESP-IDF v5.5.3 shell.
#
#   tools/build-firmware.sh <board> [build directory] [your defaults file]
#
# <board> is one of
#   waveshare-esp32-s3-touch-lcd-1.85b
#   waveshare-esp32-s3-touch-amoled-2.06
#   waveshare-esp32-s3-touch-lcd-1.54
# The build directory defaults to build/<board>. A defaults file you pass is
# applied last, so its settings win (see docs/backend.md).
set -eu
board=${1:?"usage: tools/build-firmware.sh <board> [build directory] [defaults file]"}
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build=${2:-"$root/build/$board"}
case "$build" in /*) ;; *) build="$PWD/$build" ;; esac

defaults="sdkconfig.defaults;sdkconfig.vnext.defaults;sdkconfig.vnext.release.defaults"
defaults="$defaults;sdkconfig.pocket.defaults;sdkconfig.pocket.release.defaults"
case "$board" in
  waveshare-esp32-s3-touch-lcd-1.85b)
    if [ ! -d "$root/vendor/waveshare/Examples" ]; then
      echo "The 1.85B needs Waveshare's BSP: git submodule update --init vendor/waveshare" >&2
      exit 2
    fi ;;
  waveshare-esp32-s3-touch-amoled-2.06) defaults="$defaults;sdkconfig.amoled_206.defaults" ;;
  waveshare-esp32-s3-touch-lcd-1.54) ;;
  *) echo "Unknown board: $board" >&2; exit 2 ;;
esac
if [ -n "${3:-}" ]; then
  defaults="$defaults;$(CDPATH='' cd -- "$(dirname -- "$3")" && pwd)/$(basename -- "$3")"
fi

cd "$root/firmware"
idf.py -B "$build" -D SDKCONFIG="$build/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" \
  -D AIPET_BOARD="$board" build
