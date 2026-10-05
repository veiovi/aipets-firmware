#pragma once
/* Host stand-in for espressif/esp_codec_dev 1.5: the calls pet_audio.c makes.
 * The test implements them with a model of the I2S TX DMA. */
#include <stdint.h>
typedef struct fake_codec *esp_codec_dev_handle_t;
typedef struct {
    uint8_t bits_per_sample;
    uint8_t channel;
    uint16_t channel_mask;
    uint32_t sample_rate;
    int mclk_multiple;
} esp_codec_dev_sample_info_t;
int esp_codec_dev_open(esp_codec_dev_handle_t handle, esp_codec_dev_sample_info_t *fs);
int esp_codec_dev_close(esp_codec_dev_handle_t handle);
int esp_codec_dev_write(esp_codec_dev_handle_t handle, void *data, int len);
int esp_codec_dev_read(esp_codec_dev_handle_t handle, void *data, int len);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t handle, int volume);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t handle, float db_value);
