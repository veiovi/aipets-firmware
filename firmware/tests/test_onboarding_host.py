"""Exercise the actual firmware wizard with real LVGL and simulated hardware.

Set LVGL_SOURCE_DIR to the installed/pinned lvgl__lvgl directory. This is a
native host test, never an ESP-IDF build. No dependency on the cloud checkout.
"""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LVGL = Path(os.environ.get("LVGL_SOURCE_DIR", ROOT / "managed_components/lvgl__lvgl"))


@unittest.skipUnless((LVGL / "lvgl.h").exists(), "Set LVGL_SOURCE_DIR to installed LVGL")
class OnboardingHostTests(unittest.TestCase):
    def test_real_lvgl_wizard(self):
        with tempfile.TemporaryDirectory(prefix="pet-wizard-host-") as directory:
            output = Path(directory)
            cc = os.environ.get("CC", "cc")
            common = [cc, "-std=c11", "-g", "-DLV_CONF_SKIP", "-DLV_USE_STDLIB_MALLOC=1",
                      "-DLV_USE_STDLIB_STRING=1", "-DLV_USE_STDLIB_SPRINTF=1", "-I", str(LVGL)]
            sources = sorted((LVGL / "src").rglob("*.c"))

            def compile_source(item):
                index, source = item
                obj = output / f"lvgl-{index}.o"
                subprocess.run(common + ["-c", str(source), "-o", str(obj)],
                               check=True, capture_output=True)
                return str(obj)

            with ThreadPoolExecutor(max_workers=4) as pool:
                objects = list(pool.map(compile_source, enumerate(sources)))
            binary = output / "wizard"
            # The generic build, the Pocket Terminal as released (no moves)
            # and the Pocket Terminal with CONFIG_PET_DEVICE_RELINK.
            modes = ([], ["-DCONFIG_PET_POCKET_TERMINAL=1"],
                     ["-DCONFIG_PET_POCKET_TERMINAL=1", "-DCONFIG_PET_DEVICE_RELINK=1"])
            boards = ([], ["-DPET_BOARD_AMOLED_206=1"], ["-DPET_BOARD_LCD_154=1"])
            variants = [mode + board for mode in modes for board in boards]
            cjson = Path(os.environ.get("IDF_PATH", "")) / "components/json/cJSON"
            if (cjson / "cJSON.c").exists():
                variants.append(["-DCONFIG_PET_VNEXT_ENROLLMENT=1", "-DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1",
                    "-DCONFIG_PET_POCKET_TERMINAL=1", "-DPET_BOARD_AMOLED_206=1", "-I", str(cjson),
                    str(cjson / "cJSON.c"), str(ROOT / "main/pet_control_wire.c"),
                    str(ROOT / "main/pet_usb_wifi.c"), str(ROOT / "main/pet_install.c")])
            for flags in variants:
                subprocess.run(common + ["-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "tests/onboarding_host/include"),
                    "-I", str(ROOT / "main"),
                    str(ROOT / "tests/onboarding_host/ui_test.c"),
                    str(ROOT / "main/pet_enrollment.c"), str(ROOT / "main/pet_menu_font.c"),
                    str(ROOT / "main/pet_menu_icons.c"), *objects, "-lm", "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
