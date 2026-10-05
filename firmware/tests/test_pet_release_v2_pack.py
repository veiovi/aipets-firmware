import os
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
MBED = Path(os.environ.get("IDF_PATH", "")) / "components/mbedtls/mbedtls"


def minimal_pack(canvas=240, codec=5, approved=True, bad_resource=False, pet_id="big-sal", move=False):
    """Independent minimal format fixture, not production artwork/compiler. A
    move pack has no roles, talk table or idle section and plays 60 ticks once."""
    pack = bytearray(128)
    def append(data):
        offset = len(pack)
        pack.extend(data)
        return offset
    def word(at, value):
        struct.pack_into("<H", pack, at, value)
    def dword(at, value):
        struct.pack_into("<I", pack, at, value)
    palette = append(b"\0\0\xff\xff")
    frames = append(bytes(24))
    clips = append(bytes(16))
    steps = append(struct.pack("<HH", 0, 60 if move else 8))
    roles = 0 if move else append(struct.pack("<8H", 0, *([65535] * 7)))
    talk = 0 if move else append(bytes([1, 0, 0, 0]) + bytes(108))
    idle = 0 if move else append(bytes(16))
    identity = pet_id.encode("ascii")
    assert 1 <= len(identity) <= 64
    strings = append(identity + b"Examplefixture-240-v2idle")
    raw = bytes(canvas * canvas)
    encoded = zlib.compress(raw) if codec == 5 else raw
    first = append(encoded)
    # Even an unreachable second frame must pass structural/frame decoding.
    second = append(zlib.compress(bytes([3 if bad_resource else 1]) * (canvas * canvas)) if codec == 5
                    else bytes([3 if bad_resource else 1]) * (canvas * canvas))
    for index, offset in enumerate([first, second]):
        dword(frames + index*12, offset)
        dword(frames + index*12 + 4, (second-first) if index == 0 else len(pack)-second)
        pack[frames + index*12 + 8] = codec
    dword(clips, strings + len(identity) + 21)
    word(clips + 4, 4)
    pack[clips + 6] = 0 if move else 2
    dword(clips + 8, steps)
    word(clips + 12, 1)
    pack[:8] = b"AIPFRAME"
    for offset, value in [(8, 2 if canvas == 240 else 1), (10, 128), (12, canvas), (14, canvas),
                          (28, int(approved) | (64 if move else 0)), (30, 33), (36, 2), (44, 2), (46, 1),
                          (104, len(identity)), (106, 7), (108, 14)]:
        word(offset, value)
    for offset, value in [(16, len(pack)), (32, palette), (40, frames), (48, clips), (52, roles),
                          (64, talk), (72, idle), (76, strings), (80, len(identity)+25), (92, strings),
                          (96, strings+len(identity)), (100, strings+len(identity)+7)]:
        dword(offset, value)
    dword(20, zlib.crc32(pack[128:]))
    dword(24, zlib.crc32(pack[:128]))
    return pack


@unittest.skipUnless((MBED / "library/sha256.c").exists(), "Set IDF_PATH for pinned crypto")
class PetReleaseV2PackTests(unittest.TestCase):
    def test_exact_hash_identity_and_all_frames(self):
        with tempfile.TemporaryDirectory(prefix="pet-release-pack-") as directory:
            binary = str(Path(directory) / "verify")
            frame = ROOT / "components/frame_player"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"',
                "-I", str(ROOT / "tests/onboarding_host/include"), "-I", str(MBED / "include"), "-I", str(MBED / "library"),
                "-I", str(ROOT / "main"), "-I", str(frame / "include"),
                str(ROOT / "main/pet_release_v2_pack.c"), str(ROOT / "main/pet_flash_layout.c"),
                *[str(frame / "src" / p) for p in ["frame_player.c", "frame_director.c", "frame_actions.c", "frame_codec.c"]],
                str(frame / "vendor/miniz/miniz_tinfl.c"), str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
                str(ROOT / "tests/pet_release_v2_pack_test.c"), "-o", binary], check=True)
            def check(pack, valid, structural, codecs, reviewed=None):
                result = subprocess.run([binary, "valid" if valid else "invalid", "structural" if structural else "invalid", str(codecs)] + ([reviewed] if reviewed else []),
                                        input=pack, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
            check(minimal_pack(), True, True, 32)
            reviewed=hashlib.sha256(minimal_pack(approved=False)).hexdigest()
            check(minimal_pack(), True, True, 32, reviewed)
            check(minimal_pack(), False, True, 32, "f"*64)
            changed=minimal_pack()
            changed[struct.unpack_from("<I",changed,96)[0]]=ord("X")
            struct.pack_into("<I",changed,20,zlib.crc32(changed[128:]))
            struct.pack_into("<I",changed,24,0)
            struct.pack_into("<I",changed,24,zlib.crc32(changed[:128]))
            check(changed, False, True, 32, reviewed)
            check(minimal_pack(codec=0), True, True, 1)
            check(minimal_pack(approved=False), False, True, 32)
            check(minimal_pack(canvas=120, codec=0), False, True, 1)
            # A valid move pack is never installed as a pet.
            check(minimal_pack(move=True), False, True, 32)
            # Release metadata listing another codec set never rejects the
            # exact bytes its SHA-256 pins; the validator checks the codecs.
            check(minimal_pack(), True, True, 33)
            check(minimal_pack(bad_resource=True), False, False, 32)
            damaged = minimal_pack()
            damaged[-1] ^= 1
            check(damaged, False, False, 32)
            # Even after repairing the outer CRC and expected hash, the zlib
            # checksum is validated for every resource.
            struct.pack_into("<I", damaged, 20, zlib.crc32(damaged[128:]))
            struct.pack_into("<I", damaged, 24, 0)
            struct.pack_into("<I", damaged, 24, zlib.crc32(damaged[:128]))
            check(damaged, False, False, 32)


if __name__ == "__main__":
    unittest.main()
