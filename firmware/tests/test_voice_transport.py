"""Voice transport rules learned on hardware: a slow write must not abort the
session, and a stream's end must not overtake its queued audio."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def body(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


class VoiceTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.network = (ROOT / "main/pet_network.c").read_text(encoding="utf-8")
        cls.defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

    def test_stream_end_and_cancel_follow_queued_audio(self):
        for name in ("pet_network_input_end", "pet_network_cancel"):
            function = body(self.network, f"esp_err_t {name}(", "\n}\n")
            self.assertIn("queue_stream_text(json)", function)
            self.assertNotIn("send_text(", function.replace("queue_stream_text(", ""))
        queue = body(self.network, "static esp_err_t queue_stream_text(", "\n}\n")
        self.assertIn(".text = true", queue)
        self.assertIn("xQueueSend(s_audio_tx", queue)
        sender = body(self.network, "static void tx_task(", "\n}\n")
        self.assertIn("item.text?esp_websocket_client_send_text(", sender)
        self.assertIn("esp_websocket_client_send_bin(", sender)

    def test_microphone_audio_is_sent_binary(self):
        # An uninitialised flag sent audio as WebSocket text on hardware; the
        # gateway parsed each chunk as JSON and ended listening (PIPELINE_ERROR).
        function = body(self.network, "esp_err_t pet_network_send_microphone(", "\n}\n")
        self.assertRegex(function, r"item\.text\s*=\s*false;")
        self.assertLess(function.index("item.text"), function.index("xQueueSend(s_audio_tx"))

    def test_websocket_task_stack_fits_tls(self):
        # The client's 4 KiB default overflowed in esp_aes_dma_start while the
        # connected event sent the hello, rebooting the device on reconnect.
        config = body(self.network, "esp_websocket_client_config_t config = {", "};")
        stack = int(re.search(r"\.task_stack = (\d+)", config).group(1))
        self.assertGreaterEqual(stack, 8192)

    def test_sends_tolerate_slow_writes(self):
        timeout = int(re.search(r"#define WS_SEND_TIMEOUT pdMS_TO_TICKS\((\d+)\)", self.network).group(1))
        self.assertGreaterEqual(timeout, 1000)
        sends = re.findall(r"esp_websocket_client_send_(?:bin|text)\([^;]*;", self.network)
        self.assertTrue(sends)
        for send in sends:
            self.assertIn("WS_SEND_TIMEOUT", send)
        depth = int(re.search(r"#define AUDIO_TX_QUEUE_DEPTH (\d+)", self.network).group(1))
        self.assertGreaterEqual(depth * 20, 4000, "four seconds of 20 ms chunks: a Wi-Fi hiccup keeps the listen")
        self.assertIn("xQueueCreateWithCaps(AUDIO_TX_QUEUE_DEPTH, sizeof(audio_tx_item_t), MALLOC_CAP_SPIRAM)", self.network)

    def test_wifi_power_save_off_after_start(self):
        start = self.network.index("ESP_ERROR_CHECK(esp_wifi_start());")
        self.assertGreater(self.network.index("esp_wifi_set_ps(WIFI_PS_NONE)"), start)

    def test_tls_in_psram_and_send_buffer(self):
        self.assertIn("CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y", self.defaults)
        self.assertNotIn("CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y", self.defaults)
        buffer = int(re.search(r"CONFIG_LWIP_TCP_SND_BUF_DEFAULT=(\d+)", self.defaults).group(1))
        self.assertGreaterEqual(buffer, 16384)

    def test_receive_window_keeps_speech_flowing_on_a_slow_link(self):
        # TCP delivers at most one window per round trip. Speech needs 48,000 B/s;
        # the window must keep it flowing at a 500 ms round trip.
        window = int(re.search(r"CONFIG_LWIP_TCP_WND_DEFAULT=(\d+)", self.defaults).group(1))
        self.assertGreaterEqual(window / 0.5, 48000)
        self.assertLessEqual(window, 65535, "no window scaling needed")
        mailbox = int(re.search(r"CONFIG_LWIP_TCP_RECVMBOX_SIZE=(\d+)", self.defaults).group(1))
        self.assertGreaterEqual(mailbox * 1440, window, "the mailbox holds a whole window of 1,440 B segments")

    def test_session_errors_logged_with_code(self):
        handler = body(self.network, '"session.error"', "} else if")
        self.assertIn('ESP_LOGW(TAG, "session.error code=%s', handler)


if __name__ == "__main__":
    unittest.main()
