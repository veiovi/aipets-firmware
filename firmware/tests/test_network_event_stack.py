from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", ""))
MANAGED = Path(os.environ.get("MANAGED_COMPONENTS_DIR", ROOT / "managed_components"))


class NetworkEventStackTests(unittest.TestCase):
    @unittest.skipUnless((IDF / "components/json/cJSON/cJSON.c").exists() and
                         (MANAGED / "lvgl__lvgl/lvgl.h").exists() and
                         (MANAGED / "espressif__esp_websocket_client/include/esp_websocket_client.h").exists(),
                         "Set IDF_PATH and use pinned managed components")
    def test_socket_events_retire_the_real_session(self) -> None:
        idf = Path(os.environ["IDF_PATH"])
        managed = Path(os.environ.get("MANAGED_COMPONENTS_DIR", ROOT / "managed_components"))
        with tempfile.TemporaryDirectory(prefix="pet-network-") as directory:
            include = Path(directory)
            # Fake hardware APIs; compile real protocol/parsers/settings code.
            for header in ("esp_wifi.h", "esp_event.h", "esp_netif.h", "esp_check.h", "esp_log.h",
                           "freertos/event_groups.h", "freertos/idf_additions.h", "nvs_flash.h",
                           "driver/usb_serial_jtag_vfs.h"):
                path = include / header
                path.parent.mkdir(exist_ok=True)
                path.write_text('#include "network_host.h"\n')
            (include / "nvs.h").write_text(f'#include "{idf / "components/nvs_flash/include/nvs.h"}"\n')
            (include / "esp_attr.h").write_text("#pragma once\n#define IDF_DEPRECATED(message)\n")
            binary = str(include / "network")
            command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-Wno-deprecated-declarations", "-Wno-sign-compare", "-fsanitize=address,undefined", "-DLV_CONF_SKIP",
                       '-DCONFIG_PET_VNEXT_CONTROL_ORIGIN="https://fixture.invalid"',
                       "-DESP_ERR_INVALID_SIZE=0x104",
                       "-ffunction-sections", "-fdata-sections",
                       "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
            for path in (include, ROOT / "tests", ROOT / "tests/firmware_host",
                         ROOT / "tests/onboarding_host/include", ROOT / "main",
                         ROOT / "components/face_core/include", ROOT / "components/frame_player/include",
                         managed / "lvgl__lvgl", managed / "espressif__esp_websocket_client/include",
                         idf / "components/json/cJSON", idf / "components/esp_common/include",
                         idf / "components/tcp_transport/include"):
                command += ["-I", str(path)]
            if os.environ.get("PET_NETWORK_SOURCE"):
                command += [f'-DPET_NETWORK_SOURCE="{os.environ["PET_NETWORK_SOURCE"]}"']
            command += [str(ROOT / "tests/network_event_test.c"), str(idf / "components/json/cJSON/cJSON.c"),
                        *[str(ROOT / "main" / name) for name in ("pet_session_wire.c", "pet_ima_adpcm.c",
                          "pet_control_wire.c", "pet_protocol.c", "pet_face_profile.c", "pet_speech_mouth.c",
                          "pet_config.c")], "-o", binary]
            subprocess.run(command, check=True)
            subprocess.run([binary], check=True)

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
