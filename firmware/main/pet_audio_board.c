#include "pet_audio_board.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

/* The BSP's defaults, unchanged: the port starts at 22,050 Hz, 16-bit mono;
 * esp_codec_dev sets each stream's own format when it opens. */
#define BOARD_START_RATE 22050
#define BOARD_TX_RING_FRAMES ((uint32_t)CONFIG_PET_AUDIO_DMA_BUFFERS * PET_AUDIO_DMA_FRAMES)

static const char *TAG = "pet_audio_board";
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_data_if_t *s_data;
static esp_codec_dev_handle_t s_speaker, s_microphone;
static volatile uint32_t s_tx_underflows;

/* ISR: the driver found its free-buffer queue full, so the buffer the DMA
 * plays next was never written. Kept in IRAM for an IRAM-safe driver build. */
static bool IRAM_ATTR tx_underflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle;
    (void)event;
    (void)context;
    s_tx_underflows = s_tx_underflows + 1u;
    return false;
}

/* The duplex port as bsp_audio_init() makes it, with the firmware's DMA ring.
 * Full duplex shares one channel configuration: the microphone's RX ring has
 * the same number of frames. */
static esp_err_t open_port(void)
{
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channels.dma_desc_num = CONFIG_PET_AUDIO_DMA_BUFFERS;
    channels.dma_frame_num = PET_AUDIO_DMA_FRAMES;
    /* A played buffer is cleared: with nothing written, the speaker gets silence. */
    channels.auto_clear = true;
    esp_err_t err = i2s_new_channel(&channels, &s_tx, &s_rx);
    if (err != ESP_OK) {
        s_tx = s_rx = NULL;
        return err;
    }
    const i2s_event_callbacks_t callbacks = { .on_send_q_ovf = tx_underflow };
    const i2s_std_config_t port = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_START_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    /* The callback must be registered before the channel runs. */
    err = i2s_channel_register_event_callback(s_tx, &callbacks, NULL);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_tx, &port);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &port);
    if (err == ESP_OK) err = i2s_channel_enable(s_rx);
    /* esp_codec_dev takes its configurations as non-const pointers. */
    audio_codec_i2s_cfg_t data = { .port = CONFIG_BSP_I2S_NUM, .rx_handle = s_rx, .tx_handle = s_tx };
    if (err == ESP_OK) s_data = audio_codec_new_i2s_data(&data);
    if (err == ESP_OK && s_data) {
        ESP_LOGI(TAG, "I2S%d duplex: %u DMA buffers of %u frames", CONFIG_BSP_I2S_NUM,
                 (unsigned)CONFIG_PET_AUDIO_DMA_BUFFERS, (unsigned)PET_AUDIO_DMA_FRAMES);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "I2S port failed: %s", esp_err_to_name(err == ESP_OK ? ESP_FAIL : err));
    i2s_chan_info_t info;
    if (i2s_channel_get_info(s_tx, &info) == ESP_OK && info.is_enabled) i2s_channel_disable(s_tx);
    if (i2s_channel_get_info(s_rx, &info) == ESP_OK && info.is_enabled) i2s_channel_disable(s_rx);
    i2s_del_channel(s_tx);
    i2s_del_channel(s_rx);
    s_tx = s_rx = NULL;
    s_data = NULL;
    return err == ESP_OK ? ESP_FAIL : err;
}

/* bsp_audio_codec_microphone_init(). */
static esp_codec_dev_handle_t open_microphone(i2c_master_bus_handle_t bus)
{
    audio_codec_i2c_cfg_t i2c = { .port = BSP_I2C_NUM, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&i2c);
    if (!control) return NULL;
    es7210_codec_cfg_t codec = { .ctrl_if = control };
    const audio_codec_if_t *adc = es7210_codec_new(&codec);
    if (!adc) return NULL;
    esp_codec_dev_cfg_t device = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = s_data };
    return esp_codec_dev_new(&device);
}

/* bsp_audio_codec_speaker_init(). */
static esp_codec_dev_handle_t open_speaker(i2c_master_bus_handle_t bus)
{
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c = { .port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&i2c);
    if (!control) return NULL;
    es8311_codec_cfg_t codec = {
        .ctrl_if = control,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&codec);
    if (!dac) return NULL;
    esp_codec_dev_cfg_t device = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = s_data };
    return esp_codec_dev_new(&device);
}

esp_err_t pet_audio_board_init(esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *microphone)
{
    if (!speaker || !microphone) return ESP_ERR_INVALID_ARG;
    if (!s_speaker || !s_microphone) {
        /* The display and touch share this bus; it is set up once. */
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        if (!bus) return ESP_FAIL;
        if (!s_data) {
            esp_err_t err = open_port();
            if (err != ESP_OK) return err;
        }
        /* The BSP's order: the microphone's ADC first, then the speaker's codec. */
        if (!s_microphone) s_microphone = open_microphone(bus);
        if (!s_speaker) s_speaker = open_speaker(bus);
        if (!s_speaker || !s_microphone) return ESP_FAIL;
    }
    *speaker = s_speaker;
    *microphone = s_microphone;
    return ESP_OK;
}

uint32_t pet_audio_board_tx_ring_frames(void) { return BOARD_TX_RING_FRAMES; }

uint32_t pet_audio_board_tx_underflows(void) { return s_tx_underflows; }
