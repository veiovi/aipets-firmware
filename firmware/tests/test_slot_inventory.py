"""Host-only three-pet store inventory: ASan/UBSan, torn writes, no ESP-IDF."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MBED = Path(os.environ.get("IDF_PATH", "")) / "components/mbedtls/mbedtls"


class SlotInventoryTests(unittest.TestCase):
    def test_inventory_journal_and_slot_geometry(self):
        with tempfile.TemporaryDirectory(prefix="pet-slot-inventory-") as directory:
            binary = str(Path(directory) / "slots")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(ROOT / "main"),
                *[str(ROOT / "main" / source) for source in ("pet_flash_layout.c", "pet_replace.c",
                  "pet_replace_journal.c", "pet_journal.c", "pet_slot_inventory.c")],
                str(ROOT / "tests/slot_inventory_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)


    def _store_binary(self, directory, test):
        binary = str(Path(directory) / Path(test).stem)
        sources = ["pet_flash_layout.c", "pet_flash_layout_esp.c", "pet_replace.c", "pet_replace_journal.c",
                   "pet_replace_writer.c", "pet_journal.c", "pet_slot_inventory.c", "pet_slot_store.c"]
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"', "-DMBEDTLS_MD5_C",
            "-I", str(ROOT / "tests/replacement_host"), "-I", str(ROOT / "tests/onboarding_host/include"),
            "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(ROOT / "main"),
            "-I", str(ROOT / "components/frame_player/include"),
            str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"), str(MBED / "library/md5.c"),
            *[str(ROOT / "main" / source) for source in sources],
            str(ROOT / "tests" / test), "-o", binary,
        ], check=True)
        return binary

    @unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
    def test_store_on_simulated_flash(self):
        with tempfile.TemporaryDirectory(prefix="pet-slot-store-") as directory:
            subprocess.run([self._store_binary(directory, "slot_store_test.c")], check=True)

    @unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
    def test_installation_writes_only_its_target_slot(self):
        with tempfile.TemporaryDirectory(prefix="pet-slot-install-") as directory:
            result = subprocess.run([self._store_binary(directory, "slot_install_test.c")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("three-pet installation", result.stdout)

    def test_pocket_partition_table_matches_the_firmware_geometry(self):
        """ESP-IDF builds partitions-pocket.csv; the device identifies it by the C table."""
        names = {"nvs": 2, "phy": 1, "ota": 0, "coredump": 3, "ota_0": 0x10, "ota_1": 0x11}
        rows = []
        for line in (ROOT / "partitions-pocket.csv").read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            name, kind, subtype, offset, size = [part.strip() for part in line.split(",")[:5]]
            rows.append((name, 0 if kind == "app" else 1,
                         names[subtype] if subtype in names else int(subtype, 0), int(offset, 0), int(size, 0)))
        with tempfile.TemporaryDirectory(prefix="pet-pocket-layout-") as directory:
            binary = str(Path(directory) / "regions")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "main"), str(ROOT / "main/pet_flash_layout.c"),
                            str(ROOT / "tests/pocket_layout_regions.c"), "-o", binary], check=True)
            output = subprocess.run([binary], check=True, capture_output=True, text=True).stdout
        expected = [(n, int(t), int(s), int(o), int(b)) for n, t, s, o, b in
                    (line.split(",") for line in output.splitlines())]
        self.assertEqual(rows, expected)
        defaults = (ROOT / "sdkconfig.pocket.defaults").read_text(encoding="utf-8")
        self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-pocket.csv"', defaults)
        self.assertIn("CONFIG_PET_POCKET_TERMINAL=y", defaults)
        release = (ROOT / "sdkconfig.pocket.release.defaults").read_text(encoding="utf-8")
        values = dict(line.split("=", 1) for line in release.splitlines() if line and not line.startswith("#"))
        # Epoch 4 protects every slot during firmware updates; a Pocket never
        # updates to an older epoch.
        self.assertGreaterEqual(int(values["CONFIG_PET_VNEXT_FIRMWARE_EPOCH"]), 4)
        self.assertTrue(4096 <= int(values["CONFIG_PET_VNEXT_BOOTLOADER_BYTES"]) <= 0x8000)
        self.assertRegex(values["CONFIG_PET_VNEXT_BOOTLOADER_SHA256"], r'^"[0-9a-f]{64}"$')


if __name__ == "__main__":
    unittest.main()
