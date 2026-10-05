import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class ControlHttpTests(unittest.TestCase):
    def test_trickling_transport_deadline(self):
        with tempfile.TemporaryDirectory(prefix="pet-tls-deadline-") as directory:
            binary=str(Path(directory)/"deadline")
            subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-I",str(ROOT/"tests/onboarding_host/include"),"-I",str(ROOT/"main"),
                str(ROOT/"main/pet_deadline_transport.c"),str(ROOT/"tests/deadline_transport_test.c"),"-o",binary],check=True)
            subprocess.run([binary],check=True)

    def test_native_transport(self):
        with tempfile.TemporaryDirectory(prefix="pet-control-http-") as directory:
            binary=str(Path(directory)/"http")
            subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-DCONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=1","-I",str(ROOT/"tests/onboarding_host/include"),"-I",str(ROOT/"main"),
                str(ROOT/"main/pet_control_http.c"),str(ROOT/"main/pet_enrollment.c"),
                str(ROOT/"tests/control_http_test.c"),"-o",binary],check=True)
            subprocess.run([binary],check=True)

    def test_keep_alive_downloads(self):
        with tempfile.TemporaryDirectory(prefix="pet-control-download-") as directory:
            binary=str(Path(directory)/"download")
            subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-DCONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=1","-I",str(ROOT/"tests/onboarding_host/include"),"-I",str(ROOT/"main"),
                str(ROOT/"main/pet_enrollment.c"),str(ROOT/"tests/control_download_test.c"),"-o",binary],check=True)
            subprocess.run([binary],check=True)

if __name__=="__main__":unittest.main()
