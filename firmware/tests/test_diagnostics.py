"""Host-only crash-summary lifecycle; no ESP-IDF build or physical device access."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CrashAcknowledgementTests(unittest.TestCase):
    def test_retained_crash_is_erased_only_after_an_exact_acknowledgement(self):
        with tempfile.TemporaryDirectory(prefix="pet-diagnostics-") as directory:
            binary = str(Path(directory) / "diagnostics")
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                "-I", str(ROOT / "tests/diagnostics_host"), "-I", str(ROOT / "main"),
                str(ROOT / "tests/diagnostics_test.c"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    def test_the_websocket_task_only_schedules_the_erase(self):
        network = (ROOT / "main/pet_network.c").read_text(encoding="utf-8")
        control = network[network.index("static void process_control("):network.index("static void process_binary(")]
        handler = control[control.index('"pet.crash.acknowledged"'):control.index('"pet.state"')]
        for check in ('cJSON_IsString(elf_sha)', 'cJSON_IsNumber(pc)', 'pc->valuedouble >= 0',
                      'pc->valuedouble <= UINT32_MAX',
                      'pc->valuedouble == (double)(uint32_t)pc->valuedouble'):
            self.assertIn(check, handler)
        self.assertIn("pet_diagnostics_acknowledge_crash(elf_sha->valuestring", handler)
        self.assertNotIn("esp_core_dump_image_erase", network)
        self.assertNotIn("pet_diagnostics_clear_acknowledged_crash", control)
        telemetry = network[network.index("static void telemetry_task("):network.index("static esp_err_t start_network(")]
        self.assertLess(telemetry.index("pet_diagnostics_clear_acknowledged_crash();"),
                        telemetry.index("pet_diagnostics_get_snapshot(&snapshot);"))
        diagnostics = (ROOT / "main/pet_diagnostics.c").read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"esp_core_dump_image_erase\(\)", diagnostics)), 1)


if __name__ == "__main__":
    unittest.main()
