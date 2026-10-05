import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ROM = Path(os.environ.get("IDF_PATH", "")) / "components/esp_rom"


@unittest.skipUnless((ROM / "linux/esp_rom_crc.c").exists(), "Set IDF_PATH for pinned ROM CRC")
class FirmwareControlTests(unittest.TestCase):
    def test_durable_firmware_worker(self):
        with tempfile.TemporaryDirectory(prefix="pet-firmware-control-") as directory:
            binary = str(Path(directory) / "control")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "tests/firmware_host"),
                "-I", str(ROOT / "tests/replacement_host"), "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(ROOT / "main"), "-I", str(ROM / "include"), str(ROM / "linux/esp_rom_crc.c"),
                *[str(ROOT / "main" / p) for p in ["pet_flash_layout.c", "pet_firmware_receipt.c", "pet_ota_selection.c", "pet_firmware_control.c"]],
                str(ROOT / "tests/firmware_control_test.c"), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
