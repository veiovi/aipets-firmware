"""The sound bank (sound-bank-v1): firmware/main/pet_sound_bank.c on the host
with simulated flash, NVS and server (sound_bank_test.c), and the bank builder
firmware/tools/sound_bank.py, whose output the firmware's parser reads."""
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MBED = Path(os.environ.get("IDF_PATH", "")) / "components/mbedtls/mbedtls"
TOOL = ROOT / "tools/sound_bank.py"
_spec = importlib.util.spec_from_file_location("make_atmospheres", ROOT / "tools/make_atmospheres.py")
atmospheres = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(atmospheres)


def compile_bank_test(directory: str) -> str:
    binary = str(Path(directory) / "bank")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"', "-I", str(ROOT / "tests/onboarding_host/include"),
        "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(ROOT / "main"),
        str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
        str(ROOT / "main/pet_sound_bank.c"), str(ROOT / "tests/sound_bank_test.c"), "-o", binary,
    ], check=True)
    return binary


def write_sounds(directory: Path) -> list:
    """Three sounds given out of order: two one-shots and a loop, as tones."""
    sounds = [
        {"id": "wonder-relic", "kind": "one-shot", "rate": 16000, "adpcm": "wonder.adpcm", "seconds": 0.063,
         "role": "stinger", "mood": "wonder", "title": "Something precious appears"},
        {"id": "campfire-night", "kind": "loop", "rate": 12000, "adpcm": "campfire.adpcm", "seconds": 0.168,
         "role": "loop", "mood": "calm", "title": "Campfire night", "fallback": "calm"},
        {"id": "dice-wood-1", "kind": "one-shot", "rate": 16000, "adpcm": "dice.adpcm", "seconds": 0.032,
         "role": "dice", "mood": "battle", "title": "Dice roll"},
    ]
    for blocks, sound in zip((2, 4, 1), sounds):
        tone = [round(8000 * math.sin(n / 7)) for n in range(blocks * atmospheres.BLOCK_SAMPLES)]
        (directory / sound["adpcm"]).write_bytes(atmospheres.encode(tone))
    (directory / "sounds.json").write_text(json.dumps(sounds), encoding="utf-8")
    return sounds


def run_tool(directory: Path, output: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), str(directory / "sounds.json"), str(directory / output)],
                          capture_output=True, text=True)


class SoundBankTests(unittest.TestCase):
    @unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
    def test_parser_download_record_and_refusals(self):
        with tempfile.TemporaryDirectory(prefix="pet-sound-bank-") as directory:
            result = subprocess.run([compile_bank_test(directory)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_the_builder_writes_the_same_sbk1_bank_and_manifest_each_time(self):
        with tempfile.TemporaryDirectory(prefix="pet-sound-bank-tool-") as name:
            directory = Path(name)
            sounds = write_sounds(directory)
            for output in ("first", "second"):
                result = run_tool(directory, output)
                self.assertEqual(result.returncode, 0, result.stderr)
            bank = (directory / "first/sound-bank.bin").read_bytes()
            text = (directory / "first/sound-bank.json").read_text(encoding="utf-8")
            self.assertEqual(bank, (directory / "second/sound-bank.bin").read_bytes())
            self.assertEqual(text, (directory / "second/sound-bank.json").read_text(encoding="utf-8"))
            ordered = sorted(sounds, key=lambda sound: sound["id"])
            magic, version, count, data_offset, data_bytes = struct.unpack_from("<4sHHII", bank)
            self.assertEqual((magic, version, count, data_offset), (b"SBK1", 1, 3, 16 + 3 * 36))
            self.assertEqual(data_offset + data_bytes, len(bank))
            offset = 0
            for index, sound in enumerate(ordered):
                adpcm = (directory / sound["adpcm"]).read_bytes()
                kind = {"one-shot": 0, "loop": 1}[sound["kind"]]
                entry = struct.unpack_from("<24sIIHBB", bank, 16 + 36 * index)
                expected = (sound["id"].encode().ljust(24, b"\0"), offset, len(adpcm), sound["rate"], kind, 0)
                self.assertEqual(entry, expected)
                self.assertEqual(bank[data_offset + offset:data_offset + offset + len(adpcm)], adpcm)
                offset += len(adpcm)
            manifest = json.loads(text)
            self.assertEqual(manifest["version"], 1)
            self.assertEqual(manifest["sha256"], hashlib.sha256(bank).hexdigest())
            self.assertEqual(manifest["bytes"], len(bank))
            self.assertEqual(manifest["sounds"], [{key: sound[key] for key in sound if key != "adpcm"}
                                                  for sound in ordered])

    @unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
    def test_the_firmware_reads_the_builders_bank(self):
        with tempfile.TemporaryDirectory(prefix="pet-sound-bank-tool-") as name:
            directory = Path(name)
            sounds = write_sounds(directory)
            self.assertEqual(run_tool(directory, "out").returncode, 0)
            lines = subprocess.run([compile_bank_test(name), str(directory / "out/sound-bank.bin")],
                                   capture_output=True, text=True, check=True).stdout.splitlines()
            offset = 16 + 3 * 36
            for line, sound in zip(lines, sorted(sounds, key=lambda sound: sound["id"]), strict=True):
                size = (directory / sound["adpcm"]).stat().st_size
                kind = {"one-shot": 0, "loop": 1}[sound["kind"]]
                self.assertEqual(line, f"{sound['id']} {kind} {sound['rate']} {offset} {size}")
                offset += size

    def test_the_builder_refuses_what_the_firmware_would(self):
        with tempfile.TemporaryDirectory(prefix="pet-sound-bank-tool-") as name:
            directory = Path(name)
            sounds = write_sounds(directory)
            (directory / "dice.adpcm").write_bytes((directory / "dice.adpcm").read_bytes() + b"\0")
            self.assertIn("whole 256-byte blocks", run_tool(directory, "out").stderr)
            sounds[2]["adpcm"] = "wonder.adpcm"
            sounds[2]["id"] = "Dice"
            (directory / "sounds.json").write_text(json.dumps(sounds), encoding="utf-8")
            self.assertIn("is not 1 to 23 of a-z", run_tool(directory, "out").stderr)
            self.assertFalse((directory / "out").exists())


if __name__ == "__main__":
    unittest.main()
