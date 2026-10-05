#include "pet_battery.h"

#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef PET_BOARD_LCD_154
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

/* No gauge: the cell's voltage through the board's 1:3 divider on ADC1
 * channel 0, rising from empty (3.30 V) to full (4.15 V). */
#define CELL_EMPTY_MV 3300
#define CELL_FULL_MV 4150
/* Below this the ADC reads no cell at all. */
#define CELL_ABSENT_MV 2500
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_calibration;
#endif

#define BQ27220_ADDRESS 0x55
#define BQ_REG_BATTERY_STATUS 0x0A
#define BQ_REG_REMAINING_CAPACITY 0x10
#define BQ_REG_FULL_CHARGE_CAPACITY 0x12
#define BQ_REG_AVERAGE_CURRENT 0x14
#define BQ_REG_TIME_TO_EMPTY 0x16
#define BQ_REG_TIME_TO_FULL 0x18
#define BQ_REG_STATE_OF_CHARGE 0x2C
#define BQ_VALUE_UNAVAILABLE 0xFFFF
#define BQ_STATUS_DSG (1u << 0)
#define BQ_STATUS_FULL (1u << 9)

static const char *TAG = "pet_battery";
#ifndef PET_BOARD_LCD_154
static i2c_master_dev_handle_t s_device;
#endif
static pet_battery_callback_t s_callback;
static volatile bool s_enabled;

#if defined(PET_BOARD_LCD_154)
static esp_err_t read_cell_mv(int *millivolts)
{
    int raw = 0;
    int measured = 0;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw), TAG, "read battery ADC");
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_calibration, raw, &measured), TAG, "calibrate battery ADC");
    *millivolts = measured * 3;
    return ESP_OK;
}
#elif defined(PET_BOARD_AMOLED_206)
static esp_err_t read_byte(uint8_t command, uint8_t *value)
{
    if (!s_device || !value) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_device, &command, 1, value, 1, 100);
}
#else
static esp_err_t read_word(uint8_t command, uint16_t *value)
{
    if (!s_device || !value) return ESP_ERR_INVALID_STATE;
    uint8_t bytes[2] = {0};
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_device, &command, 1,
                                                    bytes, sizeof(bytes), 100),
                        TAG, "read gauge register 0x%02x", command);
    *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return ESP_OK;
}
#endif

esp_err_t pet_battery_read(pet_battery_snapshot_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    memset(snapshot, 0, sizeof(*snapshot));
#if defined(PET_BOARD_LCD_154)
    int millivolts = 0;
    ESP_RETURN_ON_ERROR(read_cell_mv(&millivolts), TAG, "read battery voltage");
    if (millivolts < CELL_ABSENT_MV) return ESP_OK;
    int percent = (millivolts - CELL_EMPTY_MV) * 100 / (CELL_FULL_MV - CELL_EMPTY_MV);
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    snapshot->valid = true;
    snapshot->percent = (uint8_t)percent;
    snapshot->state = gpio_get_level(BSP_BATTERY_CHARGING) == 0 ? PET_BATTERY_CHARGING :
        percent == 100 ? PET_BATTERY_FULL : PET_BATTERY_DISCHARGING;
    return ESP_OK;
#elif defined(PET_BOARD_AMOLED_206)
    /* AXP2101 status/percentage only. Never change rails or charging settings. */
    uint8_t connected, state, percent;
    ESP_RETURN_ON_ERROR(read_byte(0x00, &connected), TAG, "read AXP2101 battery presence");
    if (!(connected & (1u << 3))) return ESP_OK;
    ESP_RETURN_ON_ERROR(read_byte(0x01, &state), TAG, "read AXP2101 charging state");
    ESP_RETURN_ON_ERROR(read_byte(0xa4, &percent), TAG, "read AXP2101 percentage");
    if (percent > 100) return ESP_ERR_INVALID_RESPONSE;
    snapshot->valid = true;
    snapshot->percent = percent;
    snapshot->state = percent == 100 ? PET_BATTERY_FULL :
        (state >> 5) == 1 ? PET_BATTERY_CHARGING :
        (state >> 5) == 2 ? PET_BATTERY_DISCHARGING : PET_BATTERY_IDLE;
    return ESP_OK;
#else
    uint16_t status;
    uint16_t remaining;
    uint16_t full;
    uint16_t average_current;
    uint16_t time_to_empty;
    uint16_t time_to_full;
    uint16_t percent;
    ESP_RETURN_ON_ERROR(read_word(BQ_REG_BATTERY_STATUS, &status), TAG,
                        "read battery status");
    ESP_RETURN_ON_ERROR(read_word(BQ_REG_REMAINING_CAPACITY, &remaining), TAG,
                        "read remaining capacity");
    ESP_RETURN_ON_ERROR(read_word(BQ_REG_FULL_CHARGE_CAPACITY, &full), TAG,
                        "read full capacity");
    ESP_RETURN_ON_ERROR(read_word(BQ_REG_AVERAGE_CURRENT, &average_current), TAG,
                        "read average current");
    ESP_RETURN_ON_ERROR(read_word(BQ_REG_TIME_TO_EMPTY, &time_to_empty), TAG,
                        "read time to empty");
    ESP_RETURN_ON_ERROR(read_word(BQ_REG_TIME_TO_FULL, &time_to_full), TAG,
                        "read time to full");
    ESP_RETURN_ON_ERROR(read_word(BQ_REG_STATE_OF_CHARGE, &percent), TAG,
                        "read state of charge");
    if (percent > 100 || remaining == BQ_VALUE_UNAVAILABLE ||
        full == 0 || full == BQ_VALUE_UNAVAILABLE) return ESP_ERR_INVALID_RESPONSE;
    snapshot->valid = true;
    snapshot->percent = (uint8_t)percent;
    snapshot->remaining_mah = remaining > full ? full : remaining;
    snapshot->full_mah = full;
    snapshot->average_current_ma = (int16_t)average_current;
    if (status & BQ_STATUS_FULL || percent == 100) {
        snapshot->state = PET_BATTERY_FULL;
    } else if (snapshot->average_current_ma > 10) {
        snapshot->state = PET_BATTERY_CHARGING;
    } else if ((status & BQ_STATUS_DSG) || snapshot->average_current_ma < -10) {
        snapshot->state = PET_BATTERY_DISCHARGING;
    } else {
        snapshot->state = PET_BATTERY_IDLE;
    }
    uint16_t eta = snapshot->state == PET_BATTERY_CHARGING ?
        time_to_full : time_to_empty;
    snapshot->eta_valid = (snapshot->state == PET_BATTERY_CHARGING ||
                           snapshot->state == PET_BATTERY_DISCHARGING) &&
                          eta != BQ_VALUE_UNAVAILABLE;
    snapshot->eta_minutes = snapshot->eta_valid ? eta : 0;
    return ESP_OK;
#endif
}

static void battery_task(void *arg)
{
    (void)arg;
    bool was_enabled = false;
    unsigned consecutive_errors = 0;
    for (;;) {
        if (!s_enabled) {
            was_enabled = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!was_enabled) was_enabled = true;
        pet_battery_snapshot_t snapshot;
        esp_err_t err = pet_battery_read(&snapshot);
        if (err != ESP_OK) {
            memset(&snapshot, 0, sizeof(snapshot));
            if (++consecutive_errors == 1 || consecutive_errors % 12 == 0) {
                ESP_LOGW(TAG, "battery read failed: %s (consecutive=%u)",
                         esp_err_to_name(err), consecutive_errors);
            }
        } else {
            consecutive_errors = 0;
        }
        if (s_callback) s_callback(&snapshot);
        for (unsigned elapsed = 0; elapsed < 5000 && s_enabled; elapsed += 250) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

esp_err_t pet_battery_start(pet_battery_callback_t callback)
{
    ESP_RETURN_ON_FALSE(callback, ESP_ERR_INVALID_ARG, TAG,
                        "battery callback required");
#ifdef PET_BOARD_LCD_154
    const adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    const adc_oneshot_chan_cfg_t channel = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    const adc_cali_curve_fitting_config_t calibration = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    const gpio_config_t charging = {
        .pin_bit_mask = 1ULL << BSP_BATTERY_CHARGING,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "battery ADC");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &channel), TAG, "battery ADC channel");
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&calibration, &s_adc_calibration), TAG,
                        "battery ADC calibration");
    ESP_RETURN_ON_ERROR(gpio_config(&charging), TAG, "charging pin");
    s_callback = callback;
    BaseType_t created = xTaskCreate(battery_task, "pet_battery", 3072, NULL, 2, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create battery task");
    ESP_LOGI(TAG, "battery voltage monitor ready");
    return ESP_OK;
#else
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "I2C bus unavailable");
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
#ifdef PET_BOARD_AMOLED_206
        .device_address = 0x34,
#else
        .device_address = BQ27220_ADDRESS,
#endif
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_device),
                        TAG, "add read-only battery client");
    s_callback = callback;
    BaseType_t created = xTaskCreate(battery_task, "pet_battery", 3072, NULL,
                                     2, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create battery task");
    ESP_LOGI(TAG, "read-only battery monitor ready address=0x%02x", config.device_address);
    return ESP_OK;
#endif
}

void pet_battery_set_enabled(bool enabled)
{
    s_enabled = enabled;
}
