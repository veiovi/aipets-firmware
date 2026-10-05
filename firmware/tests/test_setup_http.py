"""Real setup transport/parser code with simulated ESP-IDF HTTP/NVS APIs."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", ""))
CJSON = IDF / "components/json/cJSON"


@unittest.skipUnless((CJSON / "cJSON.c").exists(), "Set IDF_PATH for the pinned cJSON sources")
class SetupHttpTests(unittest.TestCase):
    def test_transport_and_parser(self):
        with tempfile.TemporaryDirectory(prefix="pet-setup-http-") as directory:
            binary = str(Path(directory) / "setup-http")
            vendor_object = str(Path(directory) / "cJSON.o")
            # Keep strict warnings on owned code; Apple's SDK deprecates the
            # sprintf calls in this pinned upstream dependency.
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11",
                "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
                "-c", str(CJSON / "cJSON.c"), "-o", vendor_object], check=True)
            # The default build, which never calls /v1/device/link/*, and the
            # build with CONFIG_PET_DEVICE_RELINK, which moves a linked device.
            for relink in ([], ["-DCONFIG_PET_DEVICE_RELINK=1"]):
                subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-DCONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=1", *relink,
                    "-fsanitize=address,undefined", "-I", str(ROOT / "tests/onboarding_host/include"),
                    "-I", str(CJSON), "-I", str(ROOT / "main"),
                    str(ROOT / "tests/onboarding_host/http_test.c"),
                    str(ROOT / "main/pet_enrollment.c"), str(ROOT / "main/pet_link.c"), vendor_object, "-o", binary], check=True)
                subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
