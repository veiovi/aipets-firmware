import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"

class SpeechMouthWiringTests(unittest.TestCase):
    def read(self, name: str) -> str:
        return (MAIN / name).read_text(encoding="utf-8")

    def test_stable_enum_nvs_and_console_contract(self) -> None:
        header = self.read("pet_speech_mouth.h")
        for name, value in [("CURRENT_SPRITES", 0), ("FULL_RANGE_SPRITES", 1),
                            ("SYNCED_SPRITES", 2), ("SMOOTH_24_STEP", 3),
                            ("CLASSIC_SHAPE", 4), ("EXPRESSIVE_SHAPE", 5)]:
            self.assertIn(f"PET_SPEECH_MOUTH_{name} = {value}", header)
        config = self.read("pet_config.c")
        self.assertIn('nvs_get_u8(nvs, "mouth_mode"', config)
        self.assertIn('store_u8("mouth_mode"', config)
        self.assertIn("set speech-mouth", config)

    def test_post_gain_pcm_is_aligned_before_each_codec_slice(self) -> None:
        # Each slice's value is computed before its write and queued for the
        # moment it is heard (test_mouth_timing.py).
        audio = self.read("pet_audio.c")
        gain = audio.index("pet_pcm_gain_apply")
        envelope = audio.index("pet_speech_mouth_envelope_process", gain)
        queued = audio.index("queue_speech_mouth(level, articulation)", envelope)
        write = audio.index("esp_codec_dev_write", queued)
        self.assertLess(gain, envelope)
        self.assertLess(envelope, queued)
        self.assertLess(queued, write)
        self.assertIn("s_playback_sample_rate / 50u", audio)
        self.assertIn("pet_speech_mouth_envelope_decay", audio)

    def test_full_frame_adapter_scales_the_255_envelope_without_clamping(self) -> None:
        adapter = self.read("pet_face_pack.c")
        self.assertIn("level_0_255 * 100u + 127u", adapter)
        self.assertIn("audio_level_to_percent(audio_level)", adapter)
        self.assertNotIn("audio_level > 100 ? 100 : audio_level", adapter)
        self.assertIn("#define SPEAKING_PRESENT_INTERVAL_US 20000LL", adapter)

    def test_legacy_setting_is_normalized_for_full_frame_and_non_speech_audio_is_untouched(self) -> None:
        audio = self.read("pet_audio.c")
        self.assertIn("s_playback_mouth_mode = pet_speech_mouth_full_frame_normalize(mouth_mode)", audio)
        self.assertIn("pet_face_begin_speech_stream(s_playback_mouth_mode)", audio)
        local = audio[audio.index("esp_err_t pet_audio_play_local"):]
        self.assertNotIn("pet_speech_mouth_envelope_process", local)
        face = self.read("pet_face.c")
        self.assertIn("active_speech_mouth_mode", face)
        self.assertIn("pet_speech_mouth_full_frame_normalize(initial_speech_mouth_mode)", face)

    def test_settings_report_the_effective_full_frame_authored_bank(self) -> None:
        face = self.read("pet_face.c")
        self.assertIn('lv_label_set_text(speech_mouth_label, "Speech animation")', face)
        self.assertIn('"Character-authored (7 stages)"', face)
        self.assertNotIn('"Current sprites\\nFull-range sprites', face)
        self.assertNotIn("speech_mouth_choice_event", face)
        self.assertIn("pet_face_speech_mouth_save_failed", face)
        self.assertEqual(face.count('create_button(s_face.settings_panel, "Done"'), 1)
        self.assertIn("settings_gesture_event, LV_EVENT_GESTURE", face)

    def test_frame_player_owns_stable_mouth_stage_selection(self) -> None:
        runtime = (ROOT / "components" / "frame_player" / "src" / "frame_director.c").read_text(encoding="utf-8")
        self.assertIn("talk_lut_offset", runtime)
        self.assertIn("talk_stage", runtime)
        self.assertIn("FP_TALK_STAGE_MIN_DWELL_TICKS", runtime)
        self.assertIn("FP_TALK_LOWER_STREAK_TICKS", runtime)

    def test_frame_crc_is_explicit_and_not_in_the_tick_hot_path(self) -> None:
        runtime = (ROOT / "components" / "frame_player" / "src" / "frame_player.c").read_text(encoding="utf-8")
        tick = runtime[runtime.index("FP_EXPORT(fp_tick)"):runtime.index("FP_EXPORT(fp_framebuffer)")]
        self.assertNotIn("fp_crc32_bytes", tick)
        crc = runtime[runtime.index("FP_EXPORT(fp_frame_crc32)"):runtime.index("FP_EXPORT(fp_motion_capabilities)")]
        self.assertIn("fp_crc32_bytes", crc)

if __name__ == "__main__":
    unittest.main()
