#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"

#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define BACKLIGHT_TIMER LEDC_TIMER_1
#define BACKLIGHT_CHANNEL CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH

static const char *TAG = "bsp_lcd_154";
static i2c_master_bus_handle_t s_i2c;
static bool s_backlight_ready;

/* Before app_main, as the vendor firmware does first: a board started from its
 * power button on battery turns off again unless the latch is held. */
__attribute__((constructor)) static void hold_power(void)
{
    gpio_set_level(BSP_POWER_HOLD, 1);
    gpio_set_direction(BSP_POWER_HOLD, GPIO_MODE_OUTPUT);
}

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c) return ESP_OK;
    const i2c_master_bus_config_t bus = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus, &s_i2c);
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return bsp_i2c_init() == ESP_OK ? s_i2c : NULL;
}

static esp_err_t backlight_init(void)
{
    if (s_backlight_ready) return ESP_OK;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BACKLIGHT_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    if (brightness_percent < 0) brightness_percent = 0;
    if (brightness_percent > 100) brightness_percent = 100;
    const uint32_t duty = 1023u * (uint32_t)brightness_percent / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL, duty), TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *panel,
                          esp_lcd_panel_io_handle_t *io)
{
    ESP_RETURN_ON_FALSE(config && panel && io, ESP_ERR_INVALID_ARG, TAG, "display arguments");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    const spi_bus_config_t bus = {
        .sclk_io_num = BSP_LCD_PCLK,
        .mosi_io_num = BSP_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = (int)config->max_transfer_sz,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = BSP_LCD_DC,
        .spi_mode = 3,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, io),
                        TAG, "panel IO");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(*io, &panel_config, panel), TAG, "ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*panel, true), TAG, "panel colors");
    return esp_lcd_panel_disp_on_off(*panel, true);
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *touch)
{
    (void)config;
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_INVALID_ARG, TAG, "touch argument");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "I2C bus");
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_config.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, &io), TAG, "touch IO");
    const esp_lcd_touch_config_t touch_config = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
    };
    return esp_lcd_touch_new_i2c_cst816s(io, &touch_config, touch);
}
