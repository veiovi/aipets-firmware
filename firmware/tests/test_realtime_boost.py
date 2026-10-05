import math
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


def macro(name: str) -> int:
    header = (MAIN / "pet_pcm_gain.h").read_text(encoding="utf-8")
    match = re.search(rf"^#define {name} (\d+)U$", header, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing numeric macro {name}")
    return int(match.group(1))


GAINS = {
    1: macro("PET_PCM_GAIN_Q15_3_DB"),
    2: macro("PET_PCM_GAIN_Q15_6_DB"),
    3: macro("PET_PCM_GAIN_Q15_9_DB"),
}
THRESHOLD = macro("PET_PCM_LIMITER_THRESHOLD")
HEADROOM = macro("PET_PCM_LIMITER_HEADROOM")


def boost_sample(sample: int, boost: int) -> tuple[int, bool]:
    if boost == 0:
        return sample, False
    magnitude = abs(sample)
    boosted = (magnitude * GAINS[boost] + 16384) >> 15
    limited = boosted > THRESHOLD
    if limited:
        delta = boosted - THRESHOLD
        boosted = THRESHOLD + delta * HEADROOM // (delta + HEADROOM)
    boosted = min(boosted, 32767)
    return (-boosted if sample < 0 else boosted), limited


class RealtimeBoostDspTests(unittest.TestCase):
    def test_off_is_bit_exact_for_pcm16_extremes(self) -> None:
        samples = [-32768, -28000, -1, 0, 1, 28000, 32767]
        self.assertEqual([boost_sample(x, 0)[0] for x in samples], samples)

    def test_nominal_gains_below_limiter(self) -> None:
        expected = {1: math.sqrt(2), 2: 2.0, 3: math.sqrt(8)}
        for boost, ratio in expected.items():
            output, limited = boost_sample(1000, boost)
            self.assertFalse(limited)
            self.assertAlmostEqual(output / 1000, ratio, delta=0.002)

    def test_silence_and_sign_symmetry(self) -> None:
        for boost in range(4):
            self.assertEqual(boost_sample(0, boost), (0, False))
            for sample in (1, 999, 9000, 16000, 28000, 32767):
                positive, positive_limited = boost_sample(sample, boost)
                negative, negative_limited = boost_sample(-sample, boost)
                self.assertEqual(negative, -positive)
                self.assertEqual(negative_limited, positive_limited)

    def test_soft_knee_is_monotonic_and_pcm16_bounded(self) -> None:
        for boost in (1, 2, 3):
            outputs = [boost_sample(sample, boost)[0] for sample in range(0, 32768)]
            self.assertTrue(all(a <= b for a, b in zip(outputs, outputs[1:])))
            self.assertGreaterEqual(min(outputs), 0)
            self.assertLessEqual(max(outputs), 32767)
            self.assertTrue(boost_sample(32767, boost)[1])
            self.assertGreater(boost_sample(32767, boost)[0], THRESHOLD)

    def test_int16_min_never_overflows(self) -> None:
        for boost in (1, 2, 3):
            output, limited = boost_sample(-32768, boost)
            self.assertTrue(limited)
            self.assertGreaterEqual(output, -32768)
            self.assertLessEqual(output, -THRESHOLD)

    def test_limiter_boundary_is_continuous(self) -> None:
        output, limited = boost_sample(14000, 2)
        self.assertEqual(output, THRESHOLD)
        self.assertFalse(limited)
        next_output, next_limited = boost_sample(14001, 2)
        self.assertGreaterEqual(next_output, output)
        self.assertLessEqual(next_output - output, 2)
        self.assertTrue(next_limited)


class RealtimeBoostWiringTests(unittest.TestCase):
    def read(self, filename: str) -> str:
        return (MAIN / filename).read_text(encoding="utf-8")

    def test_realtime_only_playback_scope(self) -> None:
        app = self.read("app_main.c")
        self.assertIn("s_ai_mode == PET_AI_MODE_OPENAI_REALTIME ?", app)
        self.assertIn("s_realtime_boost : PET_REALTIME_BOOST_OFF", app)
        self.assertIn("pet_audio_playback_start(stream_id, sample_rate, boost, s_speech_mouth_mode)", app)

    def test_default_and_nvs_guard(self) -> None:
        config = self.read("pet_config.c")
        self.assertIn("config->realtime_boost = PET_REALTIME_BOOST_DEFAULT", config)
        self.assertIn('nvs_get_u8(nvs, "rt_boost", &realtime_boost)', config)
        self.assertIn("realtime_boost < PET_REALTIME_BOOST_COUNT", config)
        self.assertIn('store_u8("rt_boost", (uint8_t)boost)', config)
        self.assertIn('set realtime-boost <off|3|6|9>', config)
        for value in ('"off"', '"3"', '"+3"', '"6"', '"+6"', '"9"', '"+9"'):
            self.assertIn(value, config)

    def test_settings_controls_and_close_paths_remain(self) -> None:
        face = self.read("pet_face.c")
        self.assertIn('lv_label_set_text(realtime_boost_label, "Realtime boost")', face)
        self.assertIn('"Off\\n+3 dB\\n+6 dB\\n+9 dB"', face)
        self.assertEqual(face.count('create_button(s_face.settings_panel, "Done"'), 1)
        self.assertIn("settings_gesture_event, LV_EVENT_GESTURE", face)
        self.assertIn("LV_ALIGN_BOTTOM_MID, 0, -8", face)

    def test_dsp_is_applied_before_visual_level_and_speaker_write(self) -> None:
        audio = self.read("pet_audio.c")
        gain_at = audio.index("pet_pcm_gain_apply")
        # The visual level is queued for when its slice is heard (test_mouth_timing.py).
        level_at = audio.index("queue_speech_mouth(level, articulation)", gain_at)
        write_at = audio.index("esp_codec_dev_write", level_at)
        self.assertLess(gain_at, level_at)
        self.assertLess(level_at, write_at)
        self.assertIn('log_playback_stats("complete"', audio)
        self.assertIn('log_playback_stats("cancelled"', audio)
        self.assertIn('log_playback_stats("write-error"', audio)
        cmake = self.read("CMakeLists.txt")
        self.assertIn('"pet_pcm_gain.c"', cmake)


if __name__ == "__main__":
    unittest.main()


class PocketSpeechBoostTests(unittest.TestCase):
    def test_pocket_speech_plays_without_the_realtime_boost(self):
        # Cloud replies already peak near full scale; +6 dB drove the limiter
        # hard (crackle). The Pocket has no console to change rt_boost.
        vnext = (Path(__file__).resolve().parents[1] / "main/pet_vnext.c").read_text(encoding="utf-8")
        self.assertIn("#define VNEXT_SPEECH_BOOST PET_REALTIME_BOOST_OFF", vnext)
        self.assertNotIn("PET_REALTIME_BOOST_DEFAULT", vnext)
        self.assertEqual(vnext.count("VNEXT_SPEECH_BOOST"), 3)
