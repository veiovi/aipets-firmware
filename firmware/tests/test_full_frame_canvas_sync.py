import re
import unittest
from pathlib import Path


SOURCE = Path(__file__).parents[1] / "main" / "pet_face_pack.c"


class FullFrameCanvasSyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_psram_canvas_is_published_before_invalidation(self):
        update = re.search(
            r"esp_err_t pet_face_pack_update\(.*?\n}\n",
            self.source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(update)
        body = update.group(0)
        self.assertLess(body.index("expand_dirty("), body.index("sync_canvas_buffer("))
        self.assertLess(body.index("sync_canvas_buffer("), body.index("invalidate_dirty("))

    def test_sync_uses_lvgl_draw_buffer_cache_handler(self):
        helper = re.search(
            r"static void sync_canvas_buffer\(.*?\n}\n",
            self.source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(helper)
        body = helper.group(0)
        self.assertIn("lv_canvas_get_draw_buf", body)
        self.assertIn("lv_draw_buf_invalidate_cache", body)

    def test_canvas_source_is_not_rebound_each_frame(self):
        self.assertEqual(self.source.count("lv_canvas_set_buffer("), 1)


if __name__ == "__main__":
    unittest.main()
