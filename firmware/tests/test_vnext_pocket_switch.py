"""Pocket Terminal pet switching over the open voice session (session-rebind-v1),
and listening on it: taps wait for control steps, a follow-up tap listens at
once, nothing listens without a tap, and a listen that cannot be sent says so."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from test_pet_release_v2_pack import minimal_pack

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", ""))
MBED = IDF / "components/mbedtls/mbedtls"
LVGL = Path(os.environ.get("LVGL_SOURCE_DIR", ROOT / "managed_components/lvgl__lvgl"))


@unittest.skipUnless((MBED / "library/sha256.c").exists() and (LVGL / "lvgl.h").exists(), "Set IDF_PATH and use pinned LVGL")
class VnextPocketSwitchTests(unittest.TestCase):
    def test_switching_pets_keeps_the_session_and_waits_calmly(self):
        with tempfile.TemporaryDirectory(prefix="pet-pocket-switch-") as directory:
            binary = str(Path(directory) / "switch")
            # A story event's pack as the compiler makes them: an unapproved candidate whose id starts "event-".
            event = Path(directory) / "event.aipetframes"
            event.write_bytes(minimal_pack(approved=False, pet_id="event-wolves"))
            frame = ROOT / "components/frame_player"
            common = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                "-DLV_CONF_SKIP", '-DMBEDTLS_CONFIG_FILE="pet_test_mbedtls.h"', "-DCONFIG_PET_POCKET_TERMINAL=1",
                '-DCONFIG_PET_VNEXT_RELEASE_KEY_ID=""', '-DCONFIG_PET_VNEXT_RELEASE_PUBLIC_KEY=""',
                "-DCONFIG_PET_VNEXT_FIRMWARE_EPOCH=8", "-DCONFIG_PET_VNEXT_BOOTLOADER_BYTES=0", '-DCONFIG_PET_VNEXT_BOOTLOADER_SHA256=""',
                f'-DSTORY_EVENT_PACK="{event}"',
                "-I", str(ROOT / "tests/firmware_host"), "-I", str(ROOT / "tests/onboarding_host/include"),
                "-I", str(ROOT / "main"), "-I", str(LVGL), "-I", str(frame / "include"),
                "-I", str(ROOT / "components/face_core/include"),
                "-I", str(MBED / "include"), "-I", str(MBED / "library"), "-I", str(IDF / "components/json/cJSON"),
                "-I", str(IDF / "components/esp_rom/include")]
            release_object = str(Path(directory) / "release.o")
            # The actual compatibility rules, with only the signed-record seam
            # replaced; its real cryptography has its own suite.
            subprocess.run(common + ["-Dpet_release_v2_record_verify=unused_record_verify", "-c",
                str(ROOT / "main/pet_release_v2.c"), "-o", release_object], check=True)
            subprocess.run(common + ["-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                str(IDF / "components/esp_rom/linux/esp_rom_crc.c"), release_object,
                *[str(ROOT / "main" / p) for p in ["pet_ota_selection.c", "pet_replace.c", "pet_flash_layout.c",
                    "pet_release_v2_pack.c", "pet_release_v2_protection.c", "pet_slot_protection.c", "pet_slot_inventory.c",
                    "pet_firmware_release.c", "pet_session_wire.c", "pet_control_wire.c",
                    "pet_replace_journal.c", "pet_journal.c", "pet_story_events.c"]],
                *[str(frame / "src" / p) for p in ["frame_player.c", "frame_director.c", "frame_actions.c", "frame_codec.c", "frame_stream.c"]],
                str(frame / "vendor/miniz/miniz_tinfl.c"), str(MBED / "library/sha256.c"), str(MBED / "library/platform_util.c"),
                str(IDF / "components/json/cJSON/cJSON.c"),
                str(ROOT / "tests/vnext_pocket_switch_test.c"), "-o", binary], check=True)
            result = subprocess.run([binary], input=minimal_pack(), capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            event.write_bytes(minimal_pack(codec=0, approved=False, pet_id="event-wolves"))
            partial = subprocess.run([binary, "partial-story-event"], input=minimal_pack(), capture_output=True)
            self.assertEqual(partial.returncode, 0, partial.stderr.decode())
            # Listening on the open session (the same binary runs every scenario).
            for scenario in ("tap_waits_for_the_session", "management_answer_before_the_session_waits",
                             "tap_waits_for_a_control_step", "control_yields_to_a_waiting_tap",
                             "a_follow_up_tap_listens_at_once", "a_stalled_listen_says_so",
                             "story_screens_keep_the_turn",
                             "a_story_pause_lets_the_reply_play",
                             "story_events_download_between_turns",
                             "a_wrong_event_pack_is_refused", "a_failed_download_is_tried_again",
                             "a_story_event_waits_for_the_speech", "a_tap_ends_the_story_event",
                             "the_microphone_or_a_rebind_ends_it", "a_story_tap_holds_the_reply",
                             "a_double_tap_stops_the_reply", "a_story_tap_listens_over_the_reply",
                             "a_silent_listen_plays_the_reply_on", "a_double_tap_skips_the_held_reply",
                             "the_boot_button_talks"):
                self.assertIn(f"ok {scenario}", result.stdout.decode())


if __name__ == "__main__":
    unittest.main()
