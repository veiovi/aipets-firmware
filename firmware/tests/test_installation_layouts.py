import copy
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("installation_layouts", ROOT / "firmware/tools/installation_layouts.py")
layouts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(layouts)
IDF = Path(os.environ.get("IDF_PATH", ""))


class NativeLayoutExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory(prefix="aipet-layout-native-test-")
        cls.binary = layouts.compile_exporter(Path(cls.scratch.name), cc=os.environ.get("CC", "cc"))
        cls.geometry = layouts.read_geometry(cls.binary)

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def test_exact_native_layouts_and_reserved_regions(self):
        g = self.geometry
        self.assertEqual(len(g["layouts"]), 7)
        self.assertEqual(g["bootloaderOffset"], 0)
        self.assertEqual(g["bootloaderRegionBytes"], g["partitionTableOffset"])
        common = g["layouts"][0]["regions"][:4]
        self.assertEqual(common[0], {"name": "nvs", "type": 1, "subtype": 2,
                                    "offset": 0x9000, "bytes": 0x6000, "flags": 0})
        for layout in g["layouts"]:
            self.assertEqual(layout["regions"][:4], common)
            self.assertEqual(layout["regions"][-1]["offset"] + layout["regions"][-1]["bytes"], 0x1000000)
            self.assertEqual(layout["regions"][4]["bytes"], layout["regions"][5]["bytes"])
        self.assertEqual([v["petSlots"] for v in g["layouts"]], [0, 2, 1, 1, 1, 1, 3])
        self.assertEqual([v["petPartitionBytes"] for v in g["layouts"][2:6]],
                         [0xbc0000, 0xac0000, 0x9c0000, 0x7c0000])
        for layout in g["layouts"][2:6]:
            self.assertEqual(layout["packCapacityBytes"], layout["petPartitionBytes"] - 8192)
        # Pocket Terminal: the cloud's three-pet-3p5m-v3 capacities and table.
        pocket = g["layouts"][6]
        self.assertEqual((pocket["id"], pocket["appSlotBytes"], pocket["journalOffset"], pocket["petOffset"],
                          pocket["petPartitionBytes"], pocket["packCapacityBytes"]),
                         ("three-pet-3p5m-v3", 0x380000, 0x730000, 0x740000, 0x8c0000, 0x2dd000))
        self.assertEqual(layouts.sha256(layouts.encode_partition(pocket, g)),
                         "6b040cbd3fd1d853839000f73472525b8a33421d0eb15a787248f42054a5daf7")

    def test_native_smallest_slot_selection_boundaries(self):
        values = [v for v in self.geometry["layouts"] if v["petSlots"] == 1]
        for index, layout in enumerate(values):
            boundary = layout["appSlotBytes"] * 85 // 100
            selected = layouts.read_geometry(self.binary, boundary)["selection"]
            self.assertEqual(selected, {"appBytes": boundary, "layoutId": layout["id"]})
            if index < len(values) - 1:
                self.assertEqual(layouts.read_geometry(self.binary, boundary + 1)["selection"]["layoutId"],
                                 values[index + 1]["id"])
            else:
                with self.assertRaises(layouts.LayoutError):
                    layouts.read_geometry(self.binary, boundary + 1)
        for invalid in [0, -1, 0x100000000, True, 1.5, "100"]:
            with self.subTest(invalid=invalid), self.assertRaises(layouts.LayoutError):
                layouts.read_geometry(self.binary, invalid)
        for invalid in ["0", "-1", "+1", "1x", "1 2", "4294967296", "999999999999999999999999999"]:
            with self.subTest(native=invalid), self.assertRaises(layouts.LayoutError):
                layouts.command([str(self.binary), invalid])

    def test_partition_format_exact_lengths_checksums_and_padding(self):
        import hashlib
        for layout in self.geometry["layouts"]:
            data = layouts.encode_partition(layout, self.geometry)
            self.assertEqual(len(data), 0xc00)
            for index, r in enumerate(layout["regions"]):
                self.assertEqual(struct.unpack_from("<HBBII16sI", data, index * 32),
                                 (0x50aa, r["type"], r["subtype"], r["offset"], r["bytes"],
                                  r["name"].encode().ljust(16, b"\0"), 0))
            end = len(layout["regions"]) * 32
            self.assertEqual(data[end:end + 16], b"\xeb\xeb" + b"\xff" * 14)
            self.assertEqual(data[end + 16:end + 32], hashlib.md5(data[:end], usedforsecurity=False).digest())
            self.assertEqual(data[end + 32:], b"\xff" * (0xc00 - end - 32))

    def test_serializer_rejects_unsupported_or_unsafe_regions(self):
        mutations = [
            ("flags", 1), ("offset", 0x9001), ("offset", 0x1000000),
            ("offset", 0x1000), ("bytes", 0), ("bytes", 0x6001),
            ("bytes", 0x1000000), ("name", "too_long_for_label"), ("name", "a\0b"),
        ]
        for field, value in mutations:
            with self.subTest(field=field, value=value), self.assertRaises(layouts.LayoutError):
                bad = copy.deepcopy(self.geometry["layouts"][2])
                bad["regions"][0][field] = value
                layouts.encode_partition(bad, self.geometry)
        duplicate = copy.deepcopy(self.geometry["layouts"][2])
        duplicate["regions"][1]["name"] = duplicate["regions"][0]["name"]
        with self.assertRaises(layouts.LayoutError):
            layouts.encode_partition(duplicate, self.geometry)

    def test_output_never_replaces_existing_directory(self):
        with tempfile.TemporaryDirectory(prefix="aipet-layout-output-test-") as directory:
            target = Path(directory) / "new"
            layouts.write_export(target, {"status": "test-only"}, {"example.bin": b"test"})
            with self.assertRaises(FileExistsError):
                layouts.write_export(target, {}, {"example.bin": b"changed"})
            self.assertEqual((target / "example.bin").read_bytes(), b"test")


@unittest.skipUnless((IDF / layouts.IDF_GENERATOR).exists(), "Set IDF_PATH to ESP-IDF v5.5.3 for the independent oracle")
class IdfLayoutArtifactTests(unittest.TestCase):
    def test_all_layouts_are_deterministic_and_idf_verified(self):
        with tempfile.TemporaryDirectory(prefix="aipet-layout-idf-test-") as scratch:
            a, b = Path(scratch) / "a", Path(scratch) / "b"
            a.mkdir(); b.mkdir()
            first, files = layouts.create_export(IDF, a, app_bytes=0x180000)
            second, again = layouts.create_export(IDF, b, app_bytes=0x180000)
            self.assertEqual(first, second)
            self.assertEqual(files, again)
            self.assertEqual(len(files), 14)
            self.assertEqual(first["status"], "geometry-only-not-installation-authority")
            self.assertEqual(first["selection"]["layoutId"], "single-pet-2m-v2")
            for v in first["layouts"]:
                self.assertEqual(v["partitionTableSha256"], layouts.sha256(files[v["partitionTableFile"]]))
            # Repository CSVs must produce exactly the native tables, including
            # the Pocket table that its firmware builds with.
            for csv, layout_id in [("partitions.csv", "embedded-v1"), ("partitions-vnext.csv", "dual-pet-v1"),
                                   ("partitions-pocket.csv", "three-pet-3p5m-v3")]:
                oracle = layouts.command([sys.executable, str(a / "gen_esp32part.py"), "--quiet",
                                          "--flash-size", "16MB", str(ROOT / "firmware" / csv)])
                self.assertEqual(oracle, files[f"{layout_id}.bin"])

    def test_changed_generator_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="aipet-layout-pin-test-") as scratch:
            root = Path(scratch)
            bad = root / "idf" / layouts.IDF_GENERATOR
            bad.parent.mkdir(parents=True)
            bad.write_bytes((IDF / layouts.IDF_GENERATOR).read_bytes() + b"\n# tampered\n")
            with self.assertRaisesRegex(layouts.LayoutError, "pin"):
                layouts.pinned_generator(root / "idf", root)
            self.assertFalse((root / "gen_esp32part.py").exists())

    def test_changed_source_cannot_be_reported_as_one_export(self):
        with tempfile.TemporaryDirectory(prefix="aipet-layout-race-test-") as scratch:
            with patch.object(layouts, "source_identity", side_effect=[{"commit": "old"}, {"commit": "new"}]):
                with self.assertRaisesRegex(layouts.LayoutError, "Source changed"):
                    layouts.create_export(IDF, Path(scratch))

    def test_idf_disagreement_does_not_produce_a_manifest(self):
        real = layouts.encode_partition
        def mismatched(layout, geometry):
            return b"\0" + real(layout, geometry)[1:]
        with tempfile.TemporaryDirectory(prefix="aipet-layout-mismatch-test-") as scratch:
            with patch.object(layouts, "encode_partition", side_effect=mismatched):
                with self.assertRaisesRegex(layouts.LayoutError, "byte mismatch"):
                    layouts.create_export(IDF, Path(scratch))


if __name__ == "__main__":
    unittest.main()
