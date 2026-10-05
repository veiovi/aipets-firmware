#include "pet_touch.h"
#include "pet_touch_filter.h"

#include <stdbool.h>
#include <stdint.h>
#include "pet_display.h"
#include "driver/gpio.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "pet_touch";

#define PET_TOUCH_READ_PERIOD_MS 8

/* Mirror the stable prefix of esp_lv_adapter's private touch context. The
 * adapter normally reads only after an IRQ. The CST816S occasionally misses
 * that edge after long runs, so also inspect the physical active-low IRQ line
 * on every LVGL input cycle. Read coordinates only while a touch is active;
 * the controller can legitimately NACK idle coordinate reads. */
typedef struct {
    esp_lcd_touch_handle_t handle;
    lv_indev_t *indev;
    struct {
        float x;
        float y;
    } scale;
    bool with_irq;
    SemaphoreHandle_t touch_sem;
} touch_adapter_context_prefix_t;

static void resilient_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static pet_touch_filter_t filter;
    static lv_point_t last_point;
    touch_adapter_context_prefix_t *context = lv_indev_get_driver_data(indev);
    if (!context || !context->handle) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    bool irq_signaled = false;
    if (context->touch_sem) {
        while (xSemaphoreTake(context->touch_sem, 0) == pdTRUE) irq_signaled = true;
    }
    const gpio_num_t irq_pin = context->handle->config.int_gpio_num;
    const bool irq_active = irq_pin != GPIO_NUM_NC &&
        gpio_get_level(irq_pin) == context->handle->config.levels.interrupt;
    if (!pet_touch_filter_wants_read(&filter, irq_signaled || irq_active)) {
        data->point = last_point;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t point = {0};
    uint8_t count = 0;
    esp_err_t err = esp_lcd_touch_read_data(context->handle);
    if (err == ESP_OK) err = esp_lcd_touch_get_data(context->handle, &point, &count, 1);
    const bool contact = err == ESP_OK && count;
    if (contact) {
        last_point.x = (lv_coord_t)(context->scale.x * point.x);
        last_point.y = (lv_coord_t)(context->scale.y * point.y);
    }
    data->point = last_point;
    data->state = pet_touch_filter_update(&filter, contact) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t pet_touch_enable_resilient(void)
{
    lv_indev_t *touch = pet_display_input();
    if (!touch) return ESP_ERR_INVALID_STATE;
    lv_indev_set_read_cb(touch, resilient_touch_read);
    /* LVGL otherwise reads input at the global 20 ms display refresh period.
     * The independent 8 ms timer catches short taps promptly. Idle cycles only
     * inspect the IRQ pin, so this does not create continuous I2C traffic. */
    lv_timer_set_period(lv_indev_get_read_timer(touch), PET_TOUCH_READ_PERIOD_MS);
    ESP_LOGI(TAG, "resilient touch tracking enabled period=%dms",
             PET_TOUCH_READ_PERIOD_MS);
    return ESP_OK;
}
