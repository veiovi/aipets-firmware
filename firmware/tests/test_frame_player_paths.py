"""Host-only checks of the frame player's two ways of drawing a full frame.

An RLE frame, or any frame with something drawn over it, is composed on a
scratch canvas; a raw or zlib frame with nothing over it is compared with the
framebuffer in place. The speaking-pose fuzz seed is played with its full
frames as authored and with every RLE full frame re-encoded as zlib: both must
render the same pixels and dirty rectangles on every tick, through idle bob,
speech and speaking drift. The host benchmark must build and run.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
PLAYER = ROOT / "components/frame_player"
SEED = ROOT / "tests/fixtures/frame-player-fuzz/v2-speaking.aipetframes"
SOURCES = [*sorted(str(source) for source in (PLAYER / "src").glob("*.c")), str(PLAYER / "vendor/miniz/miniz_tinfl.c")]


def rle_frames_as_zlib(pack: bytes) -> tuple[bytes, int]:
    """Append a zlib copy of each full-frame RLE resource and point the frame
    directory at it; restamp the length and both CRCs."""
    data = bytearray(pack)
    canvas = struct.unpack_from("<H", data, 12)[0]
    directory = struct.unpack_from("<I", data, 40)[0]
    rewritten = 0
    for index in range(struct.unpack_from("<H", data, 44)[0]):
        entry = directory + index * 12
        offset, length = struct.unpack_from("<II", data, entry)
        if data[entry + 8] != 3:
            continue
        runs = data[offset:offset + length]
        pixels = b"".join(bytes([runs[at + 1]]) * runs[at] for at in range(0, length, 2))
        assert len(pixels) == canvas * canvas
        stream = zlib.compress(pixels, 9)
        struct.pack_into("<II", data, entry, len(data), len(stream))
        data[entry + 8] = 5
        data += stream
        rewritten += 1
    struct.pack_into("<I", data, 16, len(data))
    struct.pack_into("<I", data, 20, zlib.crc32(data[128:]))
    header = bytearray(data[:128])
    header[24:28] = bytes(4)
    struct.pack_into("<I", data, 24, zlib.crc32(header))
    return bytes(data), rewritten


class FramePlayerPathsTests(unittest.TestCase):
    def test_rle_and_zlib_frames_render_identically(self):
        with tempfile.TemporaryDirectory(prefix="frame-player-paths-") as directory:
            zlib_pack, rewritten = rle_frames_as_zlib(SEED.read_bytes())
            self.assertGreater(rewritten, 0)
            other = Path(directory) / "zlib.aipetframes"
            other.write_bytes(zlib_pack)
            binary = str(Path(directory) / "frame_player_paths")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", str(PLAYER / "include"), str(ROOT / "tests/frame_player_paths.c"), *SOURCES, "-o", binary,
            ], check=True)
            result = subprocess.run([binary, str(SEED), str(other)], capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("both encodings identical", result.stdout)

    def test_benchmark_builds_and_runs(self):
        with tempfile.TemporaryDirectory(prefix="frame-player-bench-") as directory:
            binary = str(Path(directory) / "frame_player_bench")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", str(PLAYER / "include"), "-I", str(PLAYER / "src"),
                str(ROOT / "tests/frame_player_bench.c"), *SOURCES, "-o", binary,
            ], check=True)
            result = subprocess.run([binary, "-r", "1", "-t", "600", "-offsets", str(SEED)],
                                    capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("identity codec=", result.stdout)


if __name__ == "__main__":
    unittest.main()
