"""Move packs in the portable C validator, natively under ASan/UBSan.

frame_player_move_test.c checks the committed move seed, the byte edits of
the frame-pack compiler's move-pack tests and the player's refusal. No ESP-IDF
build or physical device access.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PLAYER = ROOT / "components/frame_player"
FIXTURES = ROOT / "tests/fixtures/frame-player-fuzz"


class FramePlayerMoveTests(unittest.TestCase):
    def test_move_packs(self):
        with tempfile.TemporaryDirectory(prefix="frame-player-move-") as directory:
            binary = str(Path(directory) / "frame_player_move_test")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", str(PLAYER / "include"), str(ROOT / "tests/frame_player_move_test.c"),
                *sorted(str(source) for source in (PLAYER / "src").glob("*.c")),
                str(PLAYER / "vendor/miniz/miniz_tinfl.c"), "-o", binary,
            ], check=True)
            result = subprocess.run([
                binary, str(FIXTURES / "v2-move.aipetframes"), str(FIXTURES / "v2-features.aipetframes"),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("the player's refusal checked", result.stdout)


if __name__ == "__main__":
    unittest.main()
