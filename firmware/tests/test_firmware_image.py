import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MBED = Path(os.environ.get("IDF_PATH", "")) / "components/mbedtls/mbedtls"


@unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
class FirmwareImageTests(unittest.TestCase):
    def test_inactive_slot_flash_adapter(self):
        with tempfile.TemporaryDirectory(prefix="pet-firmware-image-") as directory:
            binary = str(Path(directory) / "image")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"',
                "-DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1", "-I", str(ROOT / "tests/firmware_host"),
                "-I", str(ROOT / "tests/replacement_host"), "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(ROOT / "main"),
                "-I", str(MBED.parents[1] / "esp_rom/include"),
                str(MBED.parents[1] / "esp_rom/linux/esp_rom_crc.c"),
                str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
                *[str(ROOT / "main" / p) for p in ["pet_flash_layout.c", "pet_firmware_receipt.c", "pet_firmware_image.c", "pet_ota_selection.c"]],
                str(ROOT / "tests/firmware_image_test.c"), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
