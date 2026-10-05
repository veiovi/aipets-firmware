import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FirmwareReceiptTests(unittest.TestCase):
    def test_private_nvs_adapter(self):
        with tempfile.TemporaryDirectory(prefix="pet-firmware-nvs-") as directory:
            binary = str(Path(directory) / "receipt")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/onboarding_host/include"),
                *[str(ROOT / "main" / p) for p in ["pet_flash_layout.c", "pet_firmware_receipt.c", "pet_firmware_receipt_nvs.c"]],
                str(ROOT / "tests/firmware_receipt_nvs_test.c"), "-o", binary], check=True)
            subprocess.run([binary], check=True)

    def test_durable_states_and_power_loss(self):
        with tempfile.TemporaryDirectory(prefix="pet-firmware-receipt-") as directory:
            binary = str(Path(directory) / "receipt")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "main/pet_flash_layout.c"), str(ROOT / "main/pet_firmware_receipt.c"),
                str(ROOT / "tests/firmware_receipt_test.c"), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
