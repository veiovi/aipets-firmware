"""The end of a reply is heard: the speaker stays open for a tail after the
final frame, the reply is reported done once its audio has drained, and the
microphone, a cue, a new reply or a stop ends the tail at once.

The real firmware/main/pet_audio.c runs on the host against POSIX-thread
stand-ins for FreeRTOS and a model of the board's I2S TX DMA ring
(tests/audio_host). No ESP-IDF build or device is needed. Each scenario runs in
its own process, in real time, with AddressSanitizer and UBSan.
test_mouth_timing.py runs the same harness.

A reply's statistics count the buffers the ring played empty (a click the
queue's underrun counter never sees), the ring is the board's own
(firmware/main/pet_audio_board.c, created as the Waveshare BSP creates it), and
playback outranks the face's renderer.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/audio_host"
BSP = (ROOT.parent / "vendor/waveshare/Examples/ESP-IDF-V5.5.3/01_comprehensive_example/components/"
       "waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c")
BOARD = ROOT / "main/pet_audio_board.c"
IDF_I2S = Path(os.environ.get("IDF_PATH", "")) / "components/esp_driver_i2s/include/driver/i2s_common.h"


def kconfig_option(name: str) -> str:
    kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
    return kconfig.split(f"config {name}\n", 1)[1].split("\nconfig ", 1)[0].split("\nendmenu", 1)[0]


def tail_default_ms() -> int:
    return int(re.search(r"^\s+default (\d+)$", kconfig_option("PET_PLAYBACK_TAIL_MS"), re.M).group(1))


def lvgl_refresh_ms() -> int:
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    return int(re.search(r"^CONFIG_LV_DEF_REFR_PERIOD=(\d+)$", defaults, re.M).group(1))


def dma_buffers_default() -> int:
    return int(re.search(r"^\s+default (\d+)$", kconfig_option("PET_AUDIO_DMA_BUFFERS"), re.M).group(1))


def build_audio_host(binary: str, dma_buffers: int | None = None) -> None:
    """Compiles pet_audio.c with the host stand-ins and the scenario runner:
    the board's ring as the Kconfig default builds it, or `dma_buffers`."""
    buffers = dma_buffers_default() if dma_buffers is None else dma_buffers
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        f"-DCONFIG_PET_PLAYBACK_TAIL_MS={tail_default_ms()}", f"-DCONFIG_LV_DEF_REFR_PERIOD={lvgl_refresh_ms()}",
        f"-DCONFIG_PET_AUDIO_DMA_BUFFERS={buffers}",
        "-I", str(HOST / "include"), "-I", str(ROOT / "main"),
        str(ROOT / "main/pet_audio.c"), str(ROOT / "main/pet_pcm_gain.c"),
        str(ROOT / "main/pet_speech_mouth.c"), str(HOST / "fake_rtos.c"),
        str(HOST / "pet_audio_host.c"), "-o", binary,
    ], check=True)


def run_scenario(test: unittest.TestCase, binary: str, name: str) -> None:
    result = subprocess.run([binary, name], capture_output=True, text=True, timeout=60)
    test.assertEqual(result.returncode, 0, f"{name}\n{result.stdout}{result.stderr}")


class PlaybackTailTests(unittest.TestCase):
    binary: str
    directory: tempfile.TemporaryDirectory

    @classmethod
    def setUpClass(cls) -> None:
        cls.directory = tempfile.TemporaryDirectory(prefix="pet-playback-tail-")
        cls.binary = str(Path(cls.directory.name) / "pet_audio_host")
        build_audio_host(cls.binary)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.directory.cleanup()

    def run_scenario(self, name: str) -> None:
        run_scenario(self, self.binary, name)

    def test_final_marker_drains_goes_idle_then_closes_after_the_tail(self) -> None:
        self.run_scenario("complete")

    def test_a_stop_during_the_tail_closes_the_speaker_at_once(self) -> None:
        self.run_scenario("stop-in-tail")

    def test_a_stop_while_the_reply_drains_reports_nothing(self) -> None:
        self.run_scenario("stop-while-draining")

    def test_a_new_reply_during_the_tail_opens_in_its_own_format(self) -> None:
        self.run_scenario("new-reply-in-tail")

    def test_tap_to_talk_during_the_tail_gets_the_microphone_at_once(self) -> None:
        self.run_scenario("capture-in-tail")

    def test_a_cue_during_the_tail_takes_the_speaker(self) -> None:
        self.run_scenario("cue-in-tail")

    def test_a_ring_that_runs_dry_is_counted_in_the_reply_statistics(self) -> None:
        self.run_scenario("dma-underflow")

    def test_speech_slower_than_real_time_pauses_instead_of_chopping(self) -> None:
        self.run_scenario("slow-link")


class DeeperRingTests(unittest.TestCase):
    """The drain and the statistics follow the board's ring, not a fixed 60 ms."""

    def test_a_ten_buffer_ring_drains_and_reports_its_own_depth(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pet-playback-ring-") as directory:
            binary = str(Path(directory) / "pet_audio_host")
            build_audio_host(binary, dma_buffers=10)
            run_scenario(self, binary, "complete")
            run_scenario(self, binary, "dma-underflow")


class PlaybackTailContractTests(unittest.TestCase):
    def test_the_tail_is_two_seconds_by_default_and_configurable(self) -> None:
        option = kconfig_option("PET_PLAYBACK_TAIL_MS")
        self.assertIn("int ", option)
        self.assertIn("range 0 10000", option)
        self.assertNotIn("depends on", option)
        self.assertEqual(tail_default_ms(), 2000)
        audio = (ROOT / "main/pet_audio.c").read_text(encoding="utf-8")
        self.assertIn("#define PLAYBACK_TAIL_MS CONFIG_PET_PLAYBACK_TAIL_MS", audio)
        self.assertIn("#define PLAYBACK_DRAIN_FLOOR_MS 300", audio)

    def test_the_drain_uses_the_board_ring(self) -> None:
        audio = (ROOT / "main/pet_audio.c").read_text(encoding="utf-8")
        latency = audio[audio.index("static uint32_t playback_output_latency_ms"):]
        latency = latency[:latency.index("\n}\n")]
        self.assertIn("const uint64_t frames = pet_audio_board_tx_ring_frames();", latency)
        self.assertNotIn("I2S_CHANNEL_DEFAULT_CONFIG", audio)
        self.assertNotIn("bsp_audio_codec", audio)
        self.assertIn("pet_audio_board_init(&s_speaker, &s_microphone)", audio)
        board = BOARD.read_text(encoding="utf-8")
        # The ring the board reports is the one it creates.
        self.assertIn("channels.dma_desc_num = CONFIG_PET_AUDIO_DMA_BUFFERS;", board)
        self.assertIn("channels.dma_frame_num = PET_AUDIO_DMA_FRAMES;", board)
        self.assertIn("#define BOARD_TX_RING_FRAMES ((uint32_t)CONFIG_PET_AUDIO_DMA_BUFFERS * PET_AUDIO_DMA_FRAMES)", board)
        self.assertIn("uint32_t pet_audio_board_tx_ring_frames(void) { return BOARD_TX_RING_FRAMES; }", board)
        header = (ROOT / "main/pet_audio_board.h").read_text(encoding="utf-8")
        self.assertIn("#define PET_AUDIO_DMA_FRAMES 240u", header)
        option = kconfig_option("PET_AUDIO_DMA_BUFFERS")
        self.assertIn("range 2 16", option)
        self.assertEqual(dma_buffers_default(), 6)

    @unittest.skipUnless(BSP.exists(), "the Waveshare BSP submodule is not checked out")
    def test_the_board_opens_the_audio_as_the_bsp_does(self) -> None:
        bsp = BSP.read_text(encoding="utf-8")
        board = BOARD.read_text(encoding="utf-8")

        def body(source: str, start: str) -> str:
            text = source[source.index(start):]
            return text[:text.index("\n}\n")]

        init = body(bsp, "esp_err_t bsp_audio_init")
        self.assertIn("I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER)", init)
        self.assertIn("chan_cfg.auto_clear = true;", init)
        self.assertNotIn("dma_desc_num", init)
        self.assertIn("BSP_I2S_DUPLEX_MONO_CFG(22050)", init)
        port = body(board, "static esp_err_t open_port")
        self.assertIn("I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER)", port)
        self.assertIn("channels.auto_clear = true;", port)
        self.assertIn("#define BOARD_START_RATE 22050", board)
        self.assertIn("I2S_STD_CLK_DEFAULT_CONFIG(BOARD_START_RATE)", port)
        # The BSP's slot macro is the deprecated alias of the same one.
        self.assertIn("I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)", bsp)
        self.assertIn("I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)", port)
        for pin in (".mclk = BSP_I2S_MCLK", ".bclk = BSP_I2S_SCLK", ".ws = BSP_I2S_LCLK",
                    ".dout = BSP_I2S_DOUT", ".din = BSP_I2S_DSIN"):
            self.assertIn(pin, bsp)
            self.assertIn(pin, port)
        for order in (init, port):
            self.assertLess(order.index("i2s_channel_enable(i2s_tx_chan" if order is init else "i2s_channel_enable(s_tx"),
                            order.index("i2s_channel_init_std_mode(i2s_rx_chan" if order is init
                                        else "i2s_channel_init_std_mode(s_rx"))
        self.assertIn(".port = CONFIG_BSP_I2S_NUM,", init)
        self.assertIn("audio_codec_new_i2s_data(&i2s_cfg)", init)
        self.assertIn("audio_codec_new_i2s_data(&data)", port)
        speaker = body(bsp, "esp_codec_dev_handle_t bsp_audio_codec_speaker_init")
        ours = body(board, "static esp_codec_dev_handle_t open_speaker")
        for field in (".addr = ES8311_CODEC_DEFAULT_ADDR", ".codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC",
                      ".pa_pin = BSP_POWER_AMP_IO", ".pa_reverted = false", ".master_mode = false",
                      ".use_mclk = true", ".digital_mic = false", ".invert_mclk = false", ".invert_sclk = false",
                      ".dev_type = ESP_CODEC_DEV_TYPE_OUT", "audio_codec_new_gpio()"):
            self.assertIn(field, speaker)
            self.assertIn(field, ours)
        self.assertIn(".pa_voltage = 5.0", speaker)
        self.assertIn(".codec_dac_voltage = 3.3", speaker)
        self.assertIn(".hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 }", ours)
        microphone = body(bsp, "esp_codec_dev_handle_t bsp_audio_codec_microphone_init")
        mine = body(board, "static esp_codec_dev_handle_t open_microphone")
        self.assertIn("#define BSP_ES7210_CODEC_ADDR ES7210_CODEC_DEFAULT_ADDR", bsp)
        self.assertIn(".addr = BSP_ES7210_CODEC_ADDR", microphone)
        self.assertIn(".addr = ES7210_CODEC_DEFAULT_ADDR", mine)
        self.assertIn("es7210_codec_cfg_t es7210_cfg = {\n        .ctrl_if = i2c_ctrl_if,\n    };", microphone)
        self.assertIn("es7210_codec_cfg_t codec = { .ctrl_if = control };", mine)
        self.assertIn(".dev_type = ESP_CODEC_DEV_TYPE_IN", microphone)
        self.assertIn(".dev_type = ESP_CODEC_DEV_TYPE_IN", mine)
        # pet_audio_init opened the microphone before the speaker.
        init_board = body(board, "esp_err_t pet_audio_board_init")
        self.assertLess(init_board.index("open_microphone(bus)"), init_board.index("open_speaker(bus)"))

    @unittest.skipUnless(IDF_I2S.exists(), "Set IDF_PATH to ESP-IDF 5.5.3")
    def test_the_default_ring_is_the_esp_idf_default(self) -> None:
        idf = IDF_I2S.read_text(encoding="utf-8")
        default = idf[idf.index("#define I2S_CHANNEL_DEFAULT_CONFIG"):]
        default = default[:default.index("}")]
        self.assertIn(f".dma_desc_num = {dma_buffers_default()},", default)
        self.assertIn(".dma_frame_num = 240,", default)


if __name__ == "__main__":
    unittest.main()
