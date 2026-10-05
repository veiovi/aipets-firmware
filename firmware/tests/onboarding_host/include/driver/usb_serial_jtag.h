#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdbool.h>
typedef struct { size_t rx_buffer_size, tx_buffer_size; } usb_serial_jtag_driver_config_t;
bool usb_serial_jtag_is_driver_installed(void);
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);
int usb_serial_jtag_read_bytes(void *buffer, size_t length, TickType_t wait);
int usb_serial_jtag_write_bytes(const void *buffer, size_t length, TickType_t wait);
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t wait);
