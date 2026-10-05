#pragma once
/* The board's audio: one I2S port in full duplex with the ES8311 speaker codec
 * and the ES7210 microphone ADC (Waveshare ESP32-S3-Touch-LCD-1.85B). It is
 * created exactly as the Waveshare BSP's bsp_audio_codec_*_init() create it
 * (test_playback_tail.py compares the two), except that the firmware owns
 * the DMA ring and counts the speaker's DMA underflows. */
#include <stdint.h>
#include "esp_codec_dev.h"
#include "esp_err.h"

/* Frames in one I2S DMA buffer: 10 ms at 24 kHz, as the BSP's default. */
#define PET_AUDIO_DMA_FRAMES 240u

/* Creates the channels and both codecs; a later call returns the same ones. */
esp_err_t pet_audio_board_init(esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *microphone);
/* Frames the speaker's DMA ring holds (CONFIG_PET_AUDIO_DMA_BUFFERS buffers of
 * PET_AUDIO_DMA_FRAMES): how much written audio is still ahead of the
 * listener when a write returns. */
uint32_t pet_audio_board_tx_ring_frames(void);
/* Since boot: DMA buffers the speaker played although nothing new had been
 * written into them (the I2S driver's on_send_q_ovf). The BSP's ring clears
 * a buffer once played, so during a reply each one is PET_AUDIO_DMA_FRAMES of
 * silence inside the speech: a click. It also counts while the channel runs
 * with no writer (a reply priming, the microphone clocking the port), so a
 * reply counts from its first write. */
uint32_t pet_audio_board_tx_underflows(void);
