"""Real LVGL and C-player regression coverage for external flash detachment."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_pet_release_v2_pack import minimal_pack

ROOT = Path(__file__).resolve().parents[1]
LVGL = Path(os.environ.get("LVGL_SOURCE_DIR", ROOT / "managed_components/lvgl__lvgl"))
MBED = Path(os.environ.get("IDF_PATH", "")) / "components/mbedtls/mbedtls"


@unittest.skipUnless((LVGL / "lvgl.h").exists() and (MBED / "library/sha256.c").exists(),
                     "Set IDF_PATH and use the pinned LVGL dependency")
class FacePackDetachTests(unittest.TestCase):
    def test_live_renderer_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="pet-face-pack-") as directory:
            output = Path(directory)
            frame = ROOT / "components/frame_player"
            common = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-fsanitize=address,undefined",
                      "-DLV_CONF_SKIP", "-DLV_USE_STDLIB_MALLOC=1", "-DLV_USE_STDLIB_STRING=1",
                      "-DLV_USE_STDLIB_SPRINTF=1", "-DLV_DRAW_BUF_STRIDE_ALIGN=4", "-I", str(LVGL)]

            def compile_source(item):
                index, source = item
                obj = output / f"lvgl-{index}.o"
                subprocess.run(common + ["-c", str(source), "-o", str(obj)], check=True, capture_output=True)
                return str(obj)

            with ThreadPoolExecutor(max_workers=4) as pool:
                objects = list(pool.map(compile_source, enumerate(sorted((LVGL / "src").rglob("*.c")))))
            binaries = {"generic": output / "pack", "pocket": output / "pack-pocket"}
            for build, binary in binaries.items():
                subprocess.run(common + ["-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200112L",
                *(["-DCONFIG_PET_POCKET_TERMINAL=1"] if build == "pocket" else []),
                "-DCONFIG_PET_VNEXT_ENROLLMENT=1", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"',
                "-I", str(ROOT / "tests/face_pack_host/include"), "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(ROOT / "main"), "-I", str(frame / "include"),
                "-I", str(ROOT / "components/face_core/include"), "-I", str(MBED / "include"), "-I", str(MBED / "library"),
                str(ROOT / "tests/face_pack_host/pack_test.c"),
                str(ROOT / "main/pet_face_expression.c"),
                *[str(frame / "src" / p) for p in ["frame_player.c", "frame_director.c", "frame_actions.c", "frame_codec.c", "frame_display.c"]],
                str(frame / "vendor/miniz/miniz_tinfl.c"), str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
                *objects, "-lm", "-o", str(binary)], check=True)
            for canvas in (120, 240):
                for pet_id in ("big-sal", "luna", "another-pet"):
                    with self.subTest(canvas=canvas, pet_id=pet_id):
                        result = subprocess.run([str(binaries["generic"]), pet_id], input=minimal_pack(
                            canvas=canvas, codec=0 if canvas == 120 else 5, pet_id=pet_id), capture_output=True)
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
            # The Pocket Terminal idles naturally; other builds keep the showcase.
            result = subprocess.run([str(binaries["pocket"]), "luna"], input=minimal_pack(canvas=240, codec=5, pet_id="luna"),
                                    capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            # Square and rectangular panels use a centered cover canvas, clipped
            # to the display, and preserve it while a pet's flash is replaced.
            for side, height in ((240, 240), (320, 320), (480, 480), (410, 502), (502, 410)):
                with self.subTest(display_size=side):
                    result = subprocess.run([str(binaries["pocket"]), "luna", str(side), str(height)],
                        input=minimal_pack(canvas=240, codec=5, pet_id="luna"), capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
