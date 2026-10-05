"""Pocket Terminal runtime (three installed pets) with a simulated store."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from test_pet_release_v2_pack import minimal_pack

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", ""))
MBED = IDF / "components/mbedtls/mbedtls"
LVGL = Path(os.environ.get("LVGL_SOURCE_DIR", ROOT / "managed_components/lvgl__lvgl"))


@unittest.skipUnless((MBED / "library/sha256.c").exists() and (LVGL / "lvgl.h").exists(), "Set IDF_PATH and use pinned LVGL")
class VnextPocketTests(unittest.TestCase):
    def test_installed_pets_show_offline_and_switch(self):
        with tempfile.TemporaryDirectory(prefix="pet-pocket-runtime-") as directory:
            binary = str(Path(directory) / "pocket")
            frame = ROOT / "components/frame_player"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                "-DLV_CONF_SKIP", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"', "-DCONFIG_PET_POCKET_TERMINAL=1",
                '-DCONFIG_PET_VNEXT_RELEASE_KEY_ID=""', '-DCONFIG_PET_VNEXT_RELEASE_PUBLIC_KEY=""',
                "-DCONFIG_PET_VNEXT_FIRMWARE_EPOCH=3", "-DCONFIG_PET_VNEXT_BOOTLOADER_BYTES=0", '-DCONFIG_PET_VNEXT_BOOTLOADER_SHA256=""',
                "-I", str(ROOT / "tests/firmware_host"), "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(ROOT / "main"), "-I", str(LVGL), "-I", str(frame / "include"),
                "-I", str(ROOT / "components/face_core/include"),
                "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(IDF / "components/json/cJSON"),
                "-I", str(IDF / "components/esp_rom/include"),
                *[str(ROOT / "main" / p) for p in ["pet_slot_inventory.c", "pet_replace.c", "pet_replace_journal.c",
                    "pet_journal.c", "pet_flash_layout.c", "pet_session_wire.c", "pet_control_wire.c"]],
                str(IDF / "components/json/cJSON/cJSON.c"),
                *[str(frame / "src" / p) for p in ["frame_player.c", "frame_director.c", "frame_actions.c", "frame_codec.c"]],
                str(frame / "vendor/miniz/miniz_tinfl.c"),
                str(ROOT / "tests/vnext_pocket_test.c"), "-o", binary], check=True)
            result = subprocess.run([binary], input=minimal_pack(), capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
