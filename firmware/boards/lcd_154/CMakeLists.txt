# The Waveshare ESP32-S3-Touch-LCD-1.54's board support (AIPET_BOARD
# waveshare-esp32-s3-touch-lcd-1.54); see include/bsp/esp-bsp.h.
idf_component_register(
    SRCS "lcd_154.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd
    PRIV_REQUIRES esp_driver_ledc
)
