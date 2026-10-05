"""Pocket Terminal installation and voice gates with real inventory and pack rules."""
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
class VnextPocketInstallTests(unittest.TestCase):
    def test_installation_and_voice_on_three_slots(self):
        with tempfile.TemporaryDirectory(prefix="pet-pocket-install-") as directory:
            binary = str(Path(directory) / "install")
            frame = ROOT / "components/frame_player"
            common = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                "-DLV_CONF_SKIP", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"', "-DCONFIG_PET_POCKET_TERMINAL=1",
                '-DCONFIG_PET_VNEXT_RELEASE_KEY_ID=""', '-DCONFIG_PET_VNEXT_RELEASE_PUBLIC_KEY=""',
                "-DCONFIG_PET_VNEXT_FIRMWARE_EPOCH=3", "-DCONFIG_PET_VNEXT_BOOTLOADER_BYTES=0", '-DCONFIG_PET_VNEXT_BOOTLOADER_SHA256=""',
                "-I", str(ROOT / "tests/firmware_host"), "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(ROOT / "main"), "-I", str(LVGL), "-I", str(frame / "include"),
                "-I", str(ROOT / "components/face_core/include"),
                "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(IDF / "components/json/cJSON"),
                "-I", str(IDF / "components/esp_rom/include")]
            release_object = str(Path(directory) / "release.o")
            # The actual compatibility rules, with only the signed-record seam
            # replaced; its real cryptography has its own suite.
            subprocess.run(common + ["-Dpet_release_v2_record_verify=unused_record_verify", "-c",
                str(ROOT / "main/pet_release_v2.c"), "-o", release_object], check=True)
            subprocess.run(common + ["-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                str(IDF / "components/esp_rom/linux/esp_rom_crc.c"), release_object,
                *[str(ROOT / "main" / p) for p in ["pet_ota_selection.c", "pet_replace.c", "pet_flash_layout.c",
                    "pet_release_v2_pack.c", "pet_release_v2_protection.c", "pet_slot_protection.c", "pet_slot_inventory.c",
                    "pet_firmware_release.c", "pet_session_wire.c", "pet_control_wire.c",
                    "pet_replace_journal.c", "pet_journal.c"]],
                *[str(frame / "src" / p) for p in ["frame_player.c", "frame_director.c", "frame_actions.c", "frame_codec.c"]],
                str(frame / "vendor/miniz/miniz_tinfl.c"), str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
                str(IDF / "components/json/cJSON/cJSON.c"),
                str(ROOT / "tests/vnext_pocket_install_test.c"), "-o", binary], check=True)
            result = subprocess.run([binary], input=minimal_pack(), capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
