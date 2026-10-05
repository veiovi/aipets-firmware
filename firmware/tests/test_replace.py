import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MBED = Path(os.environ.get("IDF_PATH", "")) / "components/mbedtls/mbedtls"


class ReplaceTests(unittest.TestCase):
    @unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
    def test_replacement_control_worker(self):
        with tempfile.TemporaryDirectory(prefix="pet-replace-control-") as directory:
            binary = str(Path(directory) / "control")
            cjson = MBED.parents[1] / "json/cJSON"
            sources = ["pet_replace.c", "pet_replace_journal.c", "pet_replace_writer.c",
                       "pet_journal.c", "pet_replace_receipt.c", "pet_replace_control.c"]
            vendor = str(Path(directory) / "cjson.o")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-fsanitize=address,undefined",
                "-Wno-deprecated-declarations", "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", vendor,
            ], check=True)
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"',
                "-I", str(ROOT / "tests/onboarding_host/include"), "-I", str(cjson),
                "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(ROOT / "main"),
                str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"), vendor,
                *[str(ROOT / "main" / source) for source in sources],
                str(ROOT / "tests/replace_control_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    def test_durable_cloud_receipts(self):
        with tempfile.TemporaryDirectory(prefix="pet-replace-receipt-") as directory:
            binary = str(Path(directory) / "receipt")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "main/pet_replace.c"), str(ROOT / "main/pet_replace_journal.c"),
                str(ROOT / "main/pet_journal.c"), str(ROOT / "main/pet_replace_receipt.c"),
                str(ROOT / "tests/replace_receipt_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    @unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
    def test_single_slot_partition_adapter(self):
        with tempfile.TemporaryDirectory(prefix="pet-single-store-") as directory:
            binary = str(Path(directory) / "store")
            sources = ["pet_replace.c", "pet_replace_journal.c", "pet_replace_writer.c",
                       "pet_journal.c", "pet_flash_layout.c", "pet_flash_layout_esp.c", "pet_single_store.c"]
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"',
                "-DMBEDTLS_MD5_C",
                "-I", str(ROOT / "tests/replacement_host"), "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(ROOT / "main"),
                str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
                str(MBED / "library/md5.c"),
                *[str(ROOT / "main" / source) for source in sources],
                str(ROOT / "tests/single_store_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    def test_known_physical_flash_layouts(self):
        with tempfile.TemporaryDirectory(prefix="pet-flash-layout-") as directory:
            binary = str(Path(directory) / "layout")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "main/pet_replace.c"), str(ROOT / "main/pet_flash_layout.c"),
                str(ROOT / "tests/flash_layout_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    def test_single_slot_state(self):
        with tempfile.TemporaryDirectory(prefix="pet-replace-") as directory:
            binary = str(Path(directory) / "replace")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "main/pet_replace.c"), str(ROOT / "tests/replace_test.c"),
                "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    @unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
    def test_single_slot_flash_writer(self):
        with tempfile.TemporaryDirectory(prefix="pet-replace-writer-") as directory:
            binary = str(Path(directory) / "writer")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"',
                "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(ROOT / "main"),
                str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
                str(ROOT / "main/pet_replace.c"), str(ROOT / "main/pet_replace_journal.c"),
                str(ROOT / "main/pet_replace_writer.c"), str(ROOT / "main/pet_journal.c"),
                str(ROOT / "tests/replace_writer_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    def test_single_slot_journal_power_loss(self):
        with tempfile.TemporaryDirectory(prefix="pet-replace-journal-") as directory:
            binary = str(Path(directory) / "journal")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "main/pet_replace.c"), str(ROOT / "main/pet_replace_journal.c"),
                str(ROOT / "main/pet_journal.c"), str(ROOT / "tests/replace_journal_test.c"),
                "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
