#include "pet_display.h"

#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"

#if defined(PET_BOARD_AMOLED_206) || defined(PET_BOARD_LCD_154)
#include "bsp/touch.h"
#include "pet_board.h"
#define PET_DISPLAY_FROM_BSP_PANEL 1
static lv_indev_t *s_input;
#endif

#ifdef PET_BOARD_AMOLED_206
/* CO5300 requires even origins and an even number of pixels in each axis. */
static void round_area(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}
#endif

lv_display_t *pet_display_start(void)
{
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_priority = 7;
    adapter.task_max_delay_ms = 8;
#ifdef PET_DISPLAY_FROM_BSP_PANEL
    /* Use the board's panel commands and touch driver (the AMOLED's 22-column
     * gap and FT3168; the 1.54's ST7789 and CST816S), but retain our existing
     * adapter. The vendor high-level port has a different lock return type
     * and private touch structure. */
    const pet_board_profile_t *board = pet_board_current();
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    const bsp_display_config_t panel_config = { .max_transfer_sz = (size_t)board->width * board->height * 2 };
    if (esp_lv_adapter_init(&adapter) != ESP_OK ||
        bsp_display_new(&panel_config, &panel, &io) != ESP_OK ||
        bsp_touch_new(NULL, &touch) != ESP_OK)
    {
        return NULL;
    }
    const esp_lv_adapter_display_config_t config = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = board->width,
            .ver_res = board->height,
            .buffer_height = 32,
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *display = esp_lv_adapter_register_display(&config);
    if (!display) return NULL;
#ifdef PET_BOARD_AMOLED_206
    lv_display_add_event_cb(display, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
#endif
    const esp_lv_adapter_touch_config_t input = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch);
    s_input = esp_lv_adapter_register_touch(&input);
    if (!s_input || esp_lv_adapter_start() != ESP_OK) return NULL;
    ESP_LOGI("pet_display", "%s %ux%u: %s, %s, native RGB565", board->hardware, board->width, board->height,
             board->display_controller, board->touch_controller);
    return display;
#else
    bsp_display_cfg_t config = {
        .lv_adapter_cfg = adapter,
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    return bsp_display_start_with_config(&config);
#endif
}

lv_indev_t *pet_display_input(void)
{
#ifdef PET_DISPLAY_FROM_BSP_PANEL
    return s_input;
#else
    return bsp_display_get_input_dev();
#endif
}

esp_err_t pet_display_lock(int32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void pet_display_unlock(void)
{
    esp_lv_adapter_unlock();
}
