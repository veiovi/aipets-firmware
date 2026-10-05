"""Host-only C execution; no ESP-IDF build or physical device access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EnrollmentTests(unittest.TestCase):
    def test_fault_injection(self):
        with tempfile.TemporaryDirectory(prefix="pet-enrollment-") as directory:
            binary = str(Path(directory) / "enrollment")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", "-I", str(ROOT / "main"),
                str(ROOT / "main/pet_enrollment.c"),
                str(ROOT / "tests/enrollment_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
