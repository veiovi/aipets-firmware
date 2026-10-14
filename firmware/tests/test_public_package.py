"""Package real partition-table bytes without opening a device."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("package_release", ROOT / "tools/package_release.py")
packager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packager)


class PublicPackageTests(unittest.TestCase):
    def build_fixture(self, directory, regions):
        build = Path(directory) / "build"
        (build / "bootloader").mkdir(parents=True)
        (build / "partition_table").mkdir()
        (build / "bootloader/bootloader.bin").write_bytes(b"boot")
        table = b"".join(packager.ENTRY.pack(b"\xaa\x50", kind, subtype, offset, size,
                                             name.encode(), 0)
                         for name, kind, subtype, offset, size in regions)
        (build / "partition_table/partition-table.bin").write_bytes(table)
        app = bytearray(128)
        app[0] = 0xE9
        struct.pack_into("<I", app, 32, packager.APP_DESC_MAGIC)
        app[48:52] = b"test"
        (build / "aipet_firmware.bin").write_bytes(app)
        out = Path(directory) / "out"
        out.mkdir()
        return build, out, bytes(app)

    def test_factory_layout_needs_no_ota_data_and_erases_all_owner_regions(self):
        with tempfile.TemporaryDirectory() as directory:
            build, out, app = self.build_fixture(directory, [
                ("nvs", 1, 2, 0x9000, 0x6000),
                ("factory", 0, 0, 0x30000, 0x380000),
                ("pet_journal", 1, 0x41, 0x3B0000, 0x10000),
                ("pet_sound", 1, 0x44, 0x3C0000, 0x200000),
                ("pet_store", 1, 0x43, 0x5C0000, 0xA40000),
            ])
            result = packager.package("test-board", build, "test", out)
            self.assertEqual(result["app"]["offsets"], ["0x30000"])
            self.assertEqual(result["application_scheme"], "factory")
            image = (out / result["full"]["file"]).read_bytes()
            self.assertEqual(len(image), 16 * 1024 * 1024)
            self.assertEqual(image[0x30000:0x30000 + len(app)], app)
            self.assertEqual(set(image[0x3B0000:]), {255})
            self.assertEqual(set(image[0x9000:0xF000]), {255})
            self.assertEqual(result["physical_verification"], "not-performed")

    def test_legacy_dual_app_layout_uses_both_offsets(self):
        with tempfile.TemporaryDirectory() as directory:
            build, out, app = self.build_fixture(directory, [
                ("otadata", 1, 0, 0x10000, 0x2000),
                ("ota_0", 0, 0x10, 0x30000, 0x380000),
                ("ota_1", 0, 0x11, 0x3B0000, 0x380000),
                ("pet_journal", 1, 0x41, 0x730000, 0x10000),
            ])
            (build / "ota_data_initial.bin").write_bytes(b"selection")
            result = packager.package("test-board", build, "test", out)
            self.assertEqual(result["app"]["offsets"], ["0x30000", "0x3b0000"])
            image = (out / result["full"]["file"]).read_bytes()
            self.assertEqual(image[0x3B0000:0x3B0000 + len(app)], app)

    def test_oversize_app_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            build, out, _ = self.build_fixture(directory, [("factory", 0, 0, 0x30000, 64)])
            with self.assertRaisesRegex(SystemExit, "does not fit"):
                packager.package("test-board", build, "test", out)

    def test_overlapping_images_are_refused(self):
        with self.assertRaisesRegex(SystemExit, "Overlapping"):
            packager.full_image([(0, b"boot"), (3, b"app")], 10)


if __name__ == "__main__":
    unittest.main()
