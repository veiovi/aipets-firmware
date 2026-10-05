"""The Pocket release build is byte-reproducible, so an app-only install equals
the release export's firmware.bin and its bootloader is the qualified one."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PocketReleaseDefaultsTests(unittest.TestCase):
    def test_release_builds_are_reproducible(self):
        defaults = (ROOT / "sdkconfig.pocket.release.defaults").read_text(encoding="utf-8")
        lines = [line.strip() for line in defaults.splitlines()]
        self.assertIn("CONFIG_APP_REPRODUCIBLE_BUILD=y", lines)
        self.assertIn("CONFIG_PET_VNEXT_BOOTLOADER_BYTES=22304", lines)


if __name__ == "__main__":
    unittest.main()
