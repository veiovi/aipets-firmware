from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NetworkEventStackTests(unittest.TestCase):
    def test_wifi_scan_processing_is_not_run_on_sys_evt(self) -> None:
        source = (ROOT / "main" / "pet_network.c").read_text(encoding="utf-8")
        event_start = source.index("static void wifi_event(")
        event_end = source.index("static void tx_task(", event_start)
        event_handler = source[event_start:event_end]

        self.assertIn("xTaskNotifyGive(s_scan_task)", event_handler)
        self.assertNotIn("finish_wifi_scan();", event_handler)
        self.assertIn('xTaskCreate(wifi_scan_task, "pet_wifi_scan"', source)
        self.assertIn("WIFI_SCAN_TASK_STACK_BYTES 4096", source)

    def test_default_system_event_stack_has_defensive_margin(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        self.assertIn("CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096", defaults)


if __name__ == "__main__":
    unittest.main()
