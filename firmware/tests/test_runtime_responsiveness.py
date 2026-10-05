import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


class RuntimeResponsivenessTests(unittest.TestCase):
    def test_boot_button_task_has_transient_stack_headroom(self) -> None:
        app = (MAIN / "app_main.c").read_text(encoding="utf-8")
        match = re.search(
            r'xTaskCreate\(boot_button_task,\s*"pet_boot_btn",\s*(\d+)',
            app,
        )
        self.assertIsNotNone(match)
        self.assertGreaterEqual(int(match.group(1)), 4096)

    def test_touch_has_an_independent_high_frequency_read_timer(self) -> None:
        touch = (MAIN / "pet_touch.c").read_text(encoding="utf-8")
        period = re.search(r"#define PET_TOUCH_READ_PERIOD_MS\s+(\d+)", touch)
        self.assertIsNotNone(period)
        self.assertLessEqual(int(period.group(1)), 10)
        wiring = touch[
            touch.index("lv_indev_set_read_cb(touch, resilient_touch_read);") :
            touch.index('ESP_LOGI(TAG, "resilient touch tracking enabled')
        ]
        self.assertIn(
            "lv_timer_set_period(lv_indev_get_read_timer(touch), PET_TOUCH_READ_PERIOD_MS);",
            wiring,
        )
        for consumer in ("pet_face.c", "pet_onboarding.c"):
            source = (MAIN / consumer).read_text(encoding="utf-8")
            self.assertIn("pet_touch_enable_resilient()", source)

    def test_contact_noise_is_filtered_without_a_blanket_tap_debounce(self) -> None:
        # One lost read in a press must not split it into two taps; a real
        # second tap must not be thrown away.
        program = r"""
        #include <assert.h>
        #include "pet_touch_filter.h"
        int main(void) {
            pet_touch_filter_t f = {0};
            assert(!pet_touch_filter_wants_read(&f, false));          /* idle: no I2C traffic */
            assert(pet_touch_filter_wants_read(&f, true));
            assert(pet_touch_filter_update(&f, true));                /* pressed */
            assert(pet_touch_filter_wants_read(&f, false));
            assert(pet_touch_filter_update(&f, false));               /* one lost read: still pressed */
            assert(pet_touch_filter_update(&f, true));
            assert(pet_touch_filter_update(&f, false));
            assert(!pet_touch_filter_update(&f, false));              /* two in a row: released */
            assert(!pet_touch_filter_wants_read(&f, false));
            assert(pet_touch_filter_wants_read(&f, true) && pet_touch_filter_update(&f, true)); /* a new press */
            assert(!pet_touch_filter_update(&f, false) || !pet_touch_filter_update(&f, false));
            /* An IRQ whose first read finds no contact is not a press: holding
             * it would report the previous touch's coordinates. */
            pet_touch_filter_t g = {0};
            assert(pet_touch_filter_wants_read(&g, true) && !pet_touch_filter_update(&g, false));
            assert(!pet_touch_filter_wants_read(&g, false));
            assert(pet_touch_filter_wants_read(&g, true) && pet_touch_filter_update(&g, true));
            return 0;
        }
        """
        with tempfile.TemporaryDirectory(prefix="touch-filter-") as directory:
            source = Path(directory) / "filter.c"
            source.write_text(program, encoding="utf-8")
            binary = str(Path(directory) / "filter")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(MAIN), str(source), "-o", binary], check=True)
            subprocess.run([binary], check=True)
        face = (MAIN / "pet_face.c").read_text(encoding="utf-8")
        noise = re.search(r"#define TOUCH_TAP_NOISE_US\s+(\d+)", face)
        self.assertIsNotNone(noise)
        self.assertLessEqual(int(noise.group(1)), 80000)
        self.assertIn("now - s_face.last_tap_us >= TOUCH_TAP_NOISE_US", face)
        self.assertNotIn("last_tap_us >= 250000", face)

    def test_streamed_reply_does_not_reopen_codec_for_local_cues(self) -> None:
        app = (MAIN / "app_main.c").read_text(encoding="utf-8")
        start = app[
            app.index("static esp_err_t gateway_audio_start") :
            app.index("static esp_err_t gateway_audio_chunk")
        ]
        done = app[
            app.index("case APP_EVENT_PLAYBACK_DONE:") :
            app.index("case APP_EVENT_GATEWAY_ERROR:")
        ]
        self.assertNotIn("pet_sfx_play", start)
        self.assertNotIn("pet_sfx_play", done)


if __name__ == "__main__":
    unittest.main()
