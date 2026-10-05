import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class InstallTests(unittest.TestCase):
    def test_install_state_and_journal(self):
        with tempfile.TemporaryDirectory(prefix="pet-install-") as directory:
            binary=str(Path(directory)/"install")
            subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-I",str(ROOT/"main"),
                str(ROOT/"main/pet_journal.c"),str(ROOT/"main/pet_install.c"),
                str(ROOT/"tests/install_test.c"),"-o",binary],check=True)
            subprocess.run([binary],check=True)


if __name__=="__main__":
    unittest.main()
