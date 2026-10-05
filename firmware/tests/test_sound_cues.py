"""Host-only checks of the sound cue policy and the tap policy.

No ESP-IDF build or physical device access. The C policies are compiled with
AddressSanitizer and UBSan; the embed list is checked against the manifest.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KEPT_CUES = [
    "wake", "connect", "start", "stop", "complete", "cancel", "retry", "error", "no_voice",
    "settings_open", "settings_close", "face_swipe_0", "face_swipe_1", "face_swipe_2",
    "face_swipe_3", "face_swipe_4", "gesture_shake", "gesture_pop",
]


def run_c(test: str, *sources: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="pet-sound-") as directory:
        binary = str(Path(directory) / test)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
            "-I", str(ROOT / "main"), *map(str, sources), "-o", binary,
        ], check=True)
        subprocess.run([binary], check=True)


class SoundCueTests(unittest.TestCase):
    def test_cue_plan(self) -> None:
        run_c("sfx_plan", ROOT / "tests/sfx_plan_test.c", ROOT / "main/pet_sfx_plan.c")

    def test_tap_policy(self) -> None:
        run_c("tap_policy", ROOT / "tests/tap_policy_test.c")

    def test_exactly_the_chosen_cues_are_embedded(self) -> None:
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        embedded = re.findall(r'"sfx/([a-z0-9_]+)\.pcm"', cmake)
        self.assertEqual(sorted(embedded), sorted(KEPT_CUES))
        sfx = (ROOT / "main/pet_sfx.c").read_text(encoding="utf-8")
        declared = re.findall(r"DECLARE_ASSET\(([a-z0-9_]+)\);", sfx)
        self.assertEqual(sorted(declared), sorted(KEPT_CUES))
        self.assertEqual(sum((ROOT / f"main/sfx/{cue}.pcm").stat().st_size for cue in KEPT_CUES), 473966)

    def test_every_source_keeps_its_recorded_hash(self) -> None:
        manifest = json.loads((ROOT / "main/sfx/manifest.json").read_text(encoding="utf-8"))
        for cue, details in manifest["cues"].items():
            pcm = ROOT / f"main/sfx/{cue}.pcm"
            self.assertTrue(pcm.exists(), cue)
            self.assertEqual(hashlib.sha256(pcm.read_bytes()).hexdigest().upper(), details["pcm_sha256"], cue)



if __name__ == "__main__":
    unittest.main()
