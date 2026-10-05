"""The link code state machine: host C only, no ESP-IDF or cloud."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkTests(unittest.TestCase):
    def test_offer_poll_question_and_errors(self):
        with tempfile.TemporaryDirectory(prefix="pet-link-") as directory:
            binary = str(Path(directory) / "link")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", "-I", str(ROOT / "main"),
                str(ROOT / "main/pet_link.c"), str(ROOT / "tests/link_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
