import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ROM = Path(os.environ.get("IDF_PATH", "")) / "components/esp_rom"


@unittest.skipUnless((ROM / "linux/esp_rom_crc.c").exists(), "Set IDF_PATH for pinned ROM CRC")
class OtaSelectionTests(unittest.TestCase):
    def test_raw_boot_selection(self):
        with tempfile.TemporaryDirectory(prefix="pet-ota-selection-") as directory:
            binary = str(Path(directory) / "selection")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"), "-I", str(ROM / "include"),
                str(ROM / "linux/esp_rom_crc.c"), str(ROOT / "main/pet_ota_selection.c"),
                str(ROOT / "tests/ota_selection_test.c"), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
