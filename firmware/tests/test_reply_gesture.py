import unittest
from pathlib import Path


APP_MAIN = (Path(__file__).resolve().parents[1] / "main" / "app_main.c").read_text(encoding="utf-8")
PET_FACE = (Path(__file__).resolve().parents[1] / "main" / "pet_face.c").read_text(encoding="utf-8")


class ReplyGestureWiringTests(unittest.TestCase):
    def test_gateway_reply_gestures_are_ignored(self) -> None:
        gateway = APP_MAIN[APP_MAIN.index("static void gateway_gesture"):]
        gateway = gateway[:gateway.index("static esp_err_t gateway_audio_start")]
        self.assertIn("reply gesture %u ignored", gateway)
        self.assertNotIn("post_event", gateway)
        self.assertNotIn("APP_EVENT_REPLY_GESTURE", APP_MAIN)

    def test_playback_completion_returns_directly_to_idle(self) -> None:
        playback = APP_MAIN[APP_MAIN.index("case APP_EVENT_PLAYBACK_DONE:"):]
        playback = playback[:playback.index("case APP_EVENT_GATEWAY_ERROR:")]
        self.assertIn("pet_face_set_state(PET_FACE_IDLE);", playback)
        self.assertNotIn("gesture", playback)
        self.assertNotIn("s_pending_reply_gesture", APP_MAIN)

    def test_whole_face_transforms_are_blocked_during_conversation_states(self) -> None:
        trigger = PET_FACE[PET_FACE.index("uint8_t pet_face_trigger_gesture"):]
        trigger = trigger[:trigger.index("bool pet_face_can_trigger_ambient_gesture")]
        self.assertIn("s_face.state != PET_FACE_IDLE", trigger)
        self.assertIn("s_face.state != PET_FACE_OFFLINE", trigger)

    def test_pack_faces_use_native_reaction_instead_of_canvas_transform(self) -> None:
        trigger = PET_FACE[PET_FACE.index("uint8_t pet_face_trigger_gesture"):]
        trigger = trigger[:trigger.index("bool pet_face_can_trigger_ambient_gesture")]
        self.assertIn("pet_face_pack_trigger_gesture(gesture)", trigger)
        self.assertIn("return active;", trigger)
        self.assertNotIn("fc_trigger_gesture", trigger)
        self.assertNotIn("lv_obj_set_style_transform", trigger)
        self.assertNotIn("lv_image_set_scale", trigger)


if __name__ == "__main__":
    unittest.main()
