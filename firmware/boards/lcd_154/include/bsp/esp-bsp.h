#pragma once
/* Waveshare ESP32-S3-Touch-LCD-1.54: the part of a board support package the
 * AI Pet firmware uses. Waveshare publishes no BSP component for this board;
 * the pins come from its documentation and 01_factory example
 * (github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54). The board has an ST7789
 * 240x240 SPI panel, CST816S touch, an ES8311 speaker codec and an ES7210
 * microphone ADC on one I2S port, a QMI8658 IMU on the shared I2C bus, and a
 * battery read through the ADC behind a power latch. */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#define BSP_I2C_NUM CONFIG_BSP_I2C_NUM
#define BSP_I2C_SCL GPIO_NUM_41
#define BSP_I2C_SDA GPIO_NUM_42

#define BSP_I2S_MCLK GPIO_NUM_8
#define BSP_I2S_SCLK GPIO_NUM_9
#define BSP_I2S_LCLK GPIO_NUM_10
#define BSP_I2S_DSIN GPIO_NUM_11
#define BSP_I2S_DOUT GPIO_NUM_12
#define BSP_POWER_AMP_IO GPIO_NUM_7

#define BSP_LCD_H_RES 240
#define BSP_LCD_V_RES 240
#define BSP_LCD_PCLK GPIO_NUM_38
#define BSP_LCD_MOSI GPIO_NUM_39
#define BSP_LCD_CS GPIO_NUM_21
#define BSP_LCD_DC GPIO_NUM_45
#define BSP_LCD_RST GPIO_NUM_40
#define BSP_LCD_BACKLIGHT GPIO_NUM_46
#define BSP_LCD_TOUCH_INT GPIO_NUM_48
#define BSP_LCD_TOUCH_RST GPIO_NUM_47

/* On battery the board stays on only while this pin is high. */
#define BSP_POWER_HOLD GPIO_NUM_2
/* Battery voltage through a 1:3 divider on ADC1 channel 0. */
#define BSP_BATTERY_ADC GPIO_NUM_1
/* Low while the battery charges. */
#define BSP_BATTERY_CHARGING GPIO_NUM_3

#ifdef __cplusplus
extern "C" {
#endif

/* The shared I2C bus (codecs, touch, IMU), created on first use. */
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
esp_err_t bsp_display_backlight_on(void);

#ifdef __cplusplus
}
#endif
