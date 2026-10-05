#include "pet_face.h"
#include "pet_face_ai.h"
#include "pet_face_pack.h"
#include "pet_config.h"
#include "pet_touch.h"
#include "face_core.h"
#include "frame_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "pet_display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#if CONFIG_PET_POCKET_TERMINAL
#include "pet_menu_assets.h"
#endif

static const char *TAG = "pet_face";

enum {
    SETTINGS_BG = 0x17191c,
    SETTINGS_SURFACE = 0x24272b,
    SETTINGS_CONTROL = 0x30343a,
    SETTINGS_BORDER = 0x484d54,
    SETTINGS_TEXT = 0xf2f3f5,
    SETTINGS_MUTED = 0xb6bac1,
    SETTINGS_ACCENT = 0xd2d5d9,
    /* The menu's palette (pet_onboarding.c): glass, card edge, lime. */
    TERMINAL_BG = 0x0a0e07,
    TERMINAL_EDGE = 0x587a3c,
    TERMINAL_TEXT = 0xc8ef8f,
};

#define TOUCH_LONG_PRESS_US 800000
/* Two taps closer than this are contact noise, not a person tapping twice. */
#define TOUCH_TAP_NOISE_US 60000
#define TOUCH_LONG_PRESS_MAX_DRIFT 40
#define DISPLAY_DRAW_ROWS 12U

typedef struct {
    uint16_t display_width;
    uint16_t display_height;
    lv_obj_t *screen;
    lv_obj_t *ai_face;
    lv_obj_t *pack_face;
    lv_obj_t *conversation_status;
    lv_obj_t *conversation_status_label;
    lv_obj_t *tap_surface;
    lv_obj_t *settings_panel;
    lv_obj_t *volume_slider;
    lv_obj_t *volume_value;
    lv_obj_t *brightness_slider;
    lv_obj_t *brightness_value;
    lv_obj_t *shake_sensitivity_slider;
    lv_obj_t *shake_sensitivity_value;
    lv_obj_t *battery_card;
    lv_obj_t *battery_percent;
    lv_obj_t *battery_bar;
    lv_obj_t *battery_detail;
    lv_obj_t *style_dropdown;
    lv_obj_t *speech_mouth_dropdown;
    lv_obj_t *voice_dropdown;
    lv_obj_t *animation_profile_dropdown;
    lv_obj_t *ai_mode_dropdown;
    lv_obj_t *speech_profile_dropdown;
    lv_obj_t *cartesia_voice_gender_dropdown;
    lv_obj_t *realtime_model_dropdown;
    lv_obj_t *realtime_voice_dropdown;
    lv_obj_t *realtime_boost_dropdown;
    lv_obj_t *recording_timeout_dropdown;
    lv_obj_t *wifi_dropdown;
    lv_obj_t *wifi_status;
    lv_obj_t *wifi_password_panel;
    lv_obj_t *wifi_password_title;
    lv_obj_t *wifi_password;
    lv_obj_t *keyboard;
    void *display_draw_buffer_a;
    void *display_draw_buffer_b;
    lv_timer_t *timer;
    pet_face_callbacks_t callbacks;
    volatile pet_face_state_t state;
    volatile pet_expression_t expression;
    volatile uint8_t audio_level;
    volatile uint8_t brightness;
    volatile uint8_t shake_sensitivity;
    volatile uint8_t speech_articulation;
    char selected_face_id[PET_FACE_ID_MAX];
    volatile pet_face_gender_t gender;
    volatile pet_voice_t voice;
    volatile pet_ai_mode_t ai_mode;
    volatile pet_speech_profile_t speech_profile;
    bool speech_allow_cartesia_batch;
    bool speech_allow_cartesia_realtime;
    bool speech_allow_fish;
    volatile pet_cartesia_voice_gender_t cartesia_voice_gender;
    volatile pet_realtime_model_t realtime_model;
    volatile pet_realtime_voice_t realtime_voice;
    volatile pet_realtime_boost_t realtime_boost;
    volatile pet_speech_mouth_mode_t speech_mouth_mode;
    volatile pet_speech_mouth_mode_t active_speech_mouth_mode;
    volatile uint16_t recording_timeout_seconds;
    volatile pet_animation_profile_t animation_profile;
    char face_options[PET_FACE_CATALOG_CAPACITY][PET_FACE_ID_MAX];
    uint8_t face_option_count;
    pet_voice_t voice_options[PET_VOICE_COUNT];
    uint8_t voice_option_count;
    pet_realtime_voice_t realtime_voice_options[PET_REALTIME_VOICE_COUNT];
    uint8_t realtime_voice_option_count;
    pet_cartesia_voice_gender_t cartesia_gender_options[PET_CARTESIA_VOICE_GENDER_COUNT];
    uint8_t cartesia_gender_option_count;
    uint32_t tick;
    uint32_t last_render_tick;
    uint32_t mouth_changed_tick;
    uint32_t interaction_boost_until_tick;
    uint32_t perf_ticks;
    uint32_t perf_rendered;
    uint32_t perf_skipped;
    uint32_t perf_max_us;
    uint64_t perf_total_us;
    uint8_t displayed_audio_level;
    uint8_t mouth_bucket;
    int8_t conversation_status_state;
    int64_t last_tap_us;
    int64_t touch_started_us;
    lv_point_t touch_started_at;
    lv_point_t touch_current_at;
    uint16_t touch_max_drift_x;
    uint16_t touch_max_drift_y;
    bool settings_open;
    bool touch_pressed;
    bool long_press_fired;
    bool pack_active;
    volatile bool force_render;
    bool conversation_status_visible;
#if CONFIG_PET_POCKET_TERMINAL
    lv_obj_t *terminal_line;
    lv_obj_t *terminal_label;
    lv_timer_t *terminal_timer;
    char terminal_text[PET_FACE_TERMINAL_MAX + 1];
    uint8_t terminal_length;
    uint8_t terminal_typed;
#endif
    char current_ssid[PET_SSID_MAX];
    char wifi_names[PET_WIFI_SCAN_MAX][PET_SSID_MAX];
    bool wifi_saved[PET_WIFI_SCAN_MAX];
    size_t wifi_count;
} face_scene_t;

static face_scene_t s_face;

static void request_adjacent_face(int direction);

static const char *conversation_status_text(pet_face_state_t state)
{
    switch (state) {
        case PET_FACE_LISTENING: return "Listening";
        case PET_FACE_THINKING: return "Thinking";
        case PET_FACE_SPEAKING: return "Answering";
        default: return NULL;
    }
}

static void update_conversation_status(void)
{
    if (!s_face.conversation_status || !s_face.conversation_status_label) return;
    const char *text = NULL;
    if (s_face.pack_active && !s_face.settings_open) {
        text = conversation_status_text(s_face.state);
    }
    if (!text) {
        if (!s_face.conversation_status_visible) return;
        lv_obj_add_flag(s_face.conversation_status, LV_OBJ_FLAG_HIDDEN);
        s_face.conversation_status_visible = false;
        s_face.conversation_status_state = -1;
        return;
    }
    if (s_face.conversation_status_visible &&
        s_face.conversation_status_state == (int8_t)s_face.state) return;
    lv_label_set_text(s_face.conversation_status_label, text);
    lv_obj_remove_flag(s_face.conversation_status, LV_OBJ_FLAG_HIDDEN);
    s_face.conversation_status_visible = true;
    s_face.conversation_status_state = (int8_t)s_face.state;
}

static void open_settings(void)
{
#if CONFIG_PET_VNEXT_ENROLLMENT
    s_face.settings_open=true;
    if(s_face.callbacks.settings_opened)s_face.callbacks.settings_opened();
    return; /* Cloud-managed firmware uses the asset-independent native menu. */
#endif
    s_face.settings_open = true;
    /* Full-frame action clocks continue behind settings. Only native-canvas
     * presentation is suppressed until the overlay closes. */
    update_conversation_status();
    lv_obj_add_flag(s_face.tap_surface, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_face.settings_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_face.wifi_status, "scanning...");
    if (s_face.callbacks.settings_opened) s_face.callbacks.settings_opened();
    if (s_face.callbacks.wifi_scan_requested) s_face.callbacks.wifi_scan_requested();
}

static void close_settings(bool user_initiated)
{
    bool was_open = s_face.settings_open;
    s_face.settings_open = false;
    s_face.force_render = true;
    lv_obj_add_flag(s_face.wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_face.settings_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_face.tap_surface, LV_OBJ_FLAG_HIDDEN);
    update_conversation_status();
    if (was_open && s_face.callbacks.settings_closed) {
        s_face.callbacks.settings_closed(user_initiated);
    }
}

static void touch_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        s_face.touch_pressed = true;
        s_face.force_render = true;
        s_face.interaction_boost_until_tick = s_face.tick + 396;
        s_face.touch_started_at = point;
        s_face.touch_current_at = point;
        s_face.touch_started_us = esp_timer_get_time();
        s_face.touch_max_drift_x = 0;
        s_face.touch_max_drift_y = 0;
        s_face.long_press_fired = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        s_face.touch_current_at = point;
        uint16_t drift_x = (uint16_t)abs(point.x - s_face.touch_started_at.x);
        uint16_t drift_y = (uint16_t)abs(point.y - s_face.touch_started_at.y);
        if (drift_x > s_face.touch_max_drift_x) s_face.touch_max_drift_x = drift_x;
        if (drift_y > s_face.touch_max_drift_y) s_face.touch_max_drift_y = drift_y;
        int64_t held_us = esp_timer_get_time() - s_face.touch_started_us;
        if (!s_face.long_press_fired && held_us >= TOUCH_LONG_PRESS_US &&
            s_face.touch_max_drift_x <= TOUCH_LONG_PRESS_MAX_DRIFT &&
            s_face.touch_max_drift_y <= TOUCH_LONG_PRESS_MAX_DRIFT) {
            s_face.long_press_fired = true;
            ESP_LOGI(TAG, "long press spin requested duration=%lldms drift=[%u,%u]",
                     held_us / 1000, s_face.touch_max_drift_x, s_face.touch_max_drift_y);
            if (s_face.callbacks.gesture_requested) {
                s_face.callbacks.gesture_requested(FC_GESTURE_SPIN_CW);
            }
        }
        return;
    }
    if (code != LV_EVENT_RELEASED) return;
    s_face.touch_pressed = false;
    s_face.force_render = true;
    s_face.interaction_boost_until_tick = s_face.tick + 396;

    int64_t now = esp_timer_get_time();
    int dx = point.x - s_face.touch_started_at.x;
    int dy = point.y - s_face.touch_started_at.y;
    uint16_t drift_x = (uint16_t)abs(dx);
    uint16_t drift_y = (uint16_t)abs(dy);
    if (drift_x > s_face.touch_max_drift_x) s_face.touch_max_drift_x = drift_x;
    if (drift_y > s_face.touch_max_drift_y) s_face.touch_max_drift_y = drift_y;
    int64_t duration = now - s_face.touch_started_us;
    if (s_face.long_press_fired) return;
#if !CONFIG_PET_VNEXT_ENROLLMENT
    if (s_face.touch_started_at.y >= 288 && dy <= -72 && abs(dy) > abs(dx) &&
        duration < 1200000) {
        ESP_LOGW(TAG, "legacy bottom-edge recovery gesture; restarting");
        esp_restart();
        return;
    }
#endif
    if (abs(dx) >= 48 && abs(dx) > abs(dy) && duration < 1500000) {
        request_adjacent_face(dx < 0 ? 1 : -1);
        return;
    }
    if (dy >= 48 && abs(dy) > abs(dx) && duration < 1500000) {
        open_settings();
        return;
    }
    if (duration >= TOUCH_LONG_PRESS_US &&
        s_face.touch_max_drift_x <= TOUCH_LONG_PRESS_MAX_DRIFT &&
        s_face.touch_max_drift_y <= TOUCH_LONG_PRESS_MAX_DRIFT) {
        s_face.long_press_fired = true;
        ESP_LOGI(TAG, "long press spin requested on release duration=%lldms drift=[%u,%u]",
                 duration / 1000, s_face.touch_max_drift_x, s_face.touch_max_drift_y);
        if (s_face.callbacks.gesture_requested) {
            s_face.callbacks.gesture_requested(FC_GESTURE_SPIN_CW);
        }
        return;
    }
    if (abs(dx) <= 24 && abs(dy) <= 24 && duration < TOUCH_LONG_PRESS_US &&
        now - s_face.last_tap_us >= TOUCH_TAP_NOISE_US) {
        s_face.last_tap_us = now;
        if (s_face.callbacks.tapped) s_face.callbacks.tapped();
    }
}

static void close_settings_event(lv_event_t *event)
{
    (void)event;
    close_settings(true);
}

static void settings_gesture_event(lv_event_t *event)
{
    (void)event;
    lv_indev_t *input = lv_indev_active();
    if (input && lv_indev_get_gesture_dir(input) == LV_DIR_TOP) close_settings(true);
}

static void request_scan_event(lv_event_t *event)
{
    (void)event;
    lv_label_set_text(s_face.wifi_status, "scanning...");
    if (s_face.callbacks.wifi_scan_requested) s_face.callbacks.wifi_scan_requested();
}

static void volume_event(lv_event_t *event)
{
    int32_t volume = lv_slider_get_value(s_face.volume_slider);
    char label[16];
    snprintf(label, sizeof(label), "%ld%%", (long)volume);
    lv_label_set_text(s_face.volume_value, label);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED && s_face.callbacks.volume_changed) {
        s_face.callbacks.volume_changed((uint8_t)volume);
    }
}

static void brightness_event(lv_event_t *event)
{
    int32_t brightness = lv_slider_get_value(s_face.brightness_slider);
    char label[16];
    snprintf(label, sizeof(label), "%ld%%", (long)brightness);
    lv_label_set_text(s_face.brightness_value, label);
    esp_err_t err = bsp_display_brightness_set((int)brightness);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "brightness preview failed: %s", esp_err_to_name(err));
    }
    if (lv_event_get_code(event) == LV_EVENT_RELEASED &&
        s_face.callbacks.brightness_changed) {
        s_face.callbacks.brightness_changed((uint8_t)brightness);
    }
}

static const char *shake_sensitivity_label(uint8_t sensitivity)
{
    if (!sensitivity) return "Off";
    if (sensitivity <= 33) return "Low";
    if (sensitivity <= 66) return "Medium";
    return "High";
}

static void shake_sensitivity_event(lv_event_t *event)
{
    int32_t sensitivity = lv_slider_get_value(s_face.shake_sensitivity_slider);
    char label[24];
    if (!sensitivity) snprintf(label, sizeof(label), "Off");
    else snprintf(label, sizeof(label), "%ld%% %s", (long)sensitivity,
                  shake_sensitivity_label((uint8_t)sensitivity));
    lv_label_set_text(s_face.shake_sensitivity_value, label);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED &&
        s_face.callbacks.shake_sensitivity_changed) {
        s_face.callbacks.shake_sensitivity_changed((uint8_t)sensitivity);
    }
}

static void show_wifi_password_event(lv_event_t *event)
{
    (void)event;
    if (!s_face.wifi_count) return;
    uint32_t selected = lv_dropdown_get_selected(s_face.wifi_dropdown);
    if (selected >= s_face.wifi_count) return;
    if (s_face.current_ssid[0] &&
        !strcmp(s_face.current_ssid, s_face.wifi_names[selected])) {
        lv_label_set_text(s_face.wifi_status, "already connected");
        return;
    }
    if (s_face.wifi_saved[selected] &&
        s_face.callbacks.wifi_join_requested) {
        lv_label_set_text(s_face.wifi_status,
                          "using saved password; restarting...");
        s_face.callbacks.wifi_join_requested(s_face.wifi_names[selected], "");
        return;
    }
    char title[48];
    snprintf(title, sizeof(title), "Join %.32s", s_face.wifi_names[selected]);
    lv_label_set_text(s_face.wifi_password_title, title);
    lv_textarea_set_text(s_face.wifi_password, "");
    lv_obj_add_flag(s_face.settings_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_face.wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_face.keyboard, s_face.wifi_password);
}

static void close_wifi_password_panel(void)
{
    lv_keyboard_set_textarea(s_face.keyboard, NULL);
    lv_obj_add_flag(s_face.wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_face.settings_panel, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_password_back_event(lv_event_t *event)
{
    (void)event;
    close_wifi_password_panel();
}

static void wifi_password_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        close_wifi_password_panel();
        return;
    }
    if (code != LV_EVENT_READY) return;
    uint32_t selected = lv_dropdown_get_selected(s_face.wifi_dropdown);
    if (selected >= s_face.wifi_count || !s_face.callbacks.wifi_join_requested) {
        close_wifi_password_panel();
        lv_label_set_text(s_face.wifi_status, "select a network and try again");
        return;
    }

    /* LVGL sends the OK key's READY event to both the keyboard and its text
     * area, but the one-line Enter key sends READY only to the text area.
     * Listening on the text area makes both confirmation keys submit exactly
     * once. Return to settings immediately so the tap has visible feedback
     * while the app task persists Wi-Fi and restarts. */
    const char *password = lv_textarea_get_text(s_face.wifi_password);
    close_wifi_password_panel();
    lv_label_set_text(s_face.wifi_status, "saving; restarting...");
    s_face.callbacks.wifi_join_requested(s_face.wifi_names[selected],
                                         password);
}

static void animation_perf_record(bool rendered, uint32_t elapsed_us)
{
    s_face.perf_ticks++;
    if (rendered) {
        s_face.perf_rendered++;
        s_face.perf_total_us += elapsed_us;
        if (elapsed_us > s_face.perf_max_us) s_face.perf_max_us = elapsed_us;
    } else {
        s_face.perf_skipped++;
    }
    if (s_face.perf_ticks < 303) return;
    uint32_t average = s_face.perf_rendered ?
        (uint32_t)(s_face.perf_total_us / s_face.perf_rendered) : 0;
    ESP_LOGI(TAG, "animation profile=%s state=%d frames=%lu skipped=%lu avg=%luus max=%luus",
             pet_animation_profile_name(s_face.animation_profile), s_face.state,
             (unsigned long)s_face.perf_rendered, (unsigned long)s_face.perf_skipped,
             (unsigned long)average, (unsigned long)s_face.perf_max_us);
    s_face.perf_ticks = 0;
    s_face.perf_rendered = 0;
    s_face.perf_skipped = 0;
    s_face.perf_total_us = 0;
    s_face.perf_max_us = 0;
}

static void animation_tick(lv_timer_t *timer)
{
    (void)timer;
    /* The LVGL timer is only a wake-up source.  Animation clocks must follow
     * real monotonic time so a delayed UI tick does not stretch authored
     * clips or desynchronise the speech envelope. The frame player handles
     * uint32 wraparound explicitly. */
    s_face.tick = (uint32_t)(esp_timer_get_time() / 1000);
    s_face.displayed_audio_level =
        s_face.state == PET_FACE_SPEAKING ? s_face.audio_level : 0;
    bool rendered = false;
    int64_t started = esp_timer_get_time();
    if (s_face.pack_active) {
        uint16_t canvas_side = s_face.display_width > s_face.display_height ?
            s_face.display_width : s_face.display_height;
        int16_t touch_x = fp_display_touch(s_face.touch_current_at.x -
            (s_face.display_width - canvas_side) / 2, canvas_side);
        int16_t touch_y = fp_display_touch(s_face.touch_current_at.y -
            (s_face.display_height - canvas_side) / 2, canvas_side);
        if (pet_face_pack_update(
                s_face.state, s_face.expression, s_face.displayed_audio_level,
                UINT8_MAX, s_face.touch_pressed, (int16_t)touch_x,
                (int16_t)touch_y, 0, 0, s_face.animation_profile, s_face.tick,
                !s_face.settings_open, &rendered) != ESP_OK) {
            s_face.pack_active = false;
            if (s_face.pack_face) {
                lv_obj_add_flag(s_face.pack_face, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(s_face.ai_face, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGE(TAG, "frame player failed; switched to emergency face");
        }
    }
    if (!s_face.pack_active && !s_face.settings_open) {
        pet_face_ai_render(s_face.state, s_face.expression,
                           s_face.displayed_audio_level, s_face.tick,
                           s_face.touch_pressed);
        rendered = true;
    }
    s_face.last_render_tick = s_face.tick;
    s_face.force_render = false;
    animation_perf_record(rendered, rendered ? (uint32_t)(esp_timer_get_time() - started) : 0);
    update_conversation_status();
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text, int width,
                               lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 42);
    lv_obj_set_style_bg_color(button, lv_color_hex(SETTINGS_CONTROL), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(SETTINGS_BORDER), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return button;
}

static void center_open_dropdown(void *user_data)
{
    lv_obj_t *dropdown = user_data;
    if (!dropdown || !lv_dropdown_is_open(dropdown)) return;
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    if (!list) return;
    lv_obj_set_width(list, 284);
    if (lv_obj_get_height(list) > 224) lv_obj_set_height(list, 224);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_center(list);
}

static void circular_dropdown_ready_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_READY) return;
    lv_async_call(center_open_dropdown, lv_event_get_target(event));
}

static void style_settings_dropdown(lv_obj_t *dropdown)
{
    if (!dropdown) return;
    lv_obj_set_style_text_align(dropdown, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(SETTINGS_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(SETTINGS_TEXT), LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(SETTINGS_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(dropdown, 1, LV_PART_MAIN);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    if (!list) return;
    lv_obj_set_style_bg_color(list, lv_color_hex(SETTINGS_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(SETTINGS_TEXT), LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(SETTINGS_BORDER), LV_PART_MAIN);
    lv_obj_set_style_max_width(list, 284, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 224, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(SETTINGS_CONTROL), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(SETTINGS_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_text_align(list, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(list, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(dropdown, circular_dropdown_ready_event, LV_EVENT_READY,
                        NULL);
}

static void style_settings_slider(lv_obj_t *slider)
{
    if (!slider) return;
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_TEXT), LV_PART_KNOB);
}

static void set_dropdown_enabled(lv_obj_t *dropdown, bool enabled)
{
    if (!dropdown) return;
    if (enabled) lv_obj_remove_state(dropdown, LV_STATE_DISABLED);
    else lv_obj_add_state(dropdown, LV_STATE_DISABLED);
}

static uint32_t face_option_index(const char *face_id)
{
    for (uint8_t i = 0; i < s_face.face_option_count; ++i) {
        if (face_id && !strcmp(s_face.face_options[i], face_id)) return i;
    }
    return 0;
}

static void request_adjacent_face(int direction)
{
    if (s_face.callbacks.pet_swiped) {
        s_face.callbacks.pet_swiped(direction);
        return;
    }
    if (!s_face.face_option_count || !s_face.callbacks.face_changed) return;
    uint32_t current = face_option_index(s_face.selected_face_id);
    uint32_t next = direction > 0 ?
        (current + 1u) % s_face.face_option_count :
        (current + s_face.face_option_count - 1u) % s_face.face_option_count;
    const char *face_id = s_face.face_options[next];
    if (!strcmp(face_id, s_face.selected_face_id)) return;
    ESP_LOGI(TAG, "horizontal swipe requesting face %s -> %s",
             s_face.selected_face_id, face_id);
    s_face.callbacks.face_changed(face_id, PET_FACE_CHANGE_SWIPE);
}

static void add_face_option(char *options, size_t capacity, size_t *used,
                            const char *face_id, const char *label)
{
    if (!options || !used || !face_id || !label ||
        s_face.face_option_count >= PET_FACE_CATALOG_CAPACITY) return;
    int written = snprintf(options + *used, capacity - *used, "%s%s",
                           *used ? "\n" : "", label);
    if (written < 0 || (size_t)written >= capacity - *used) return;
    *used += (size_t)written;
    strlcpy(s_face.face_options[s_face.face_option_count++], face_id,
            PET_FACE_ID_MAX);
}

static void style_choice_event(lv_event_t *event)
{
    (void)event;
    uint32_t selected = lv_dropdown_get_selected(s_face.style_dropdown);
    if (selected >= s_face.face_option_count) return;
    const char *face_id = s_face.face_options[selected];
    if (!strcmp(face_id, s_face.selected_face_id)) return;
    set_dropdown_enabled(s_face.style_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving face...");
    if (s_face.callbacks.face_changed) {
        s_face.callbacks.face_changed(face_id, PET_FACE_CHANGE_SETTINGS);
    }
    else set_dropdown_enabled(s_face.style_dropdown, true);
}

static void append_dropdown_option(char *options, size_t capacity,
                                   const char *name, bool *first)
{
    if (!options || !capacity || !name || !first) return;
    strlcat(options, *first ? "" : "\n", capacity);
    strlcat(options, name, capacity);
    *first = false;
}

static uint32_t voice_option_index(pet_voice_t voice)
{
    for (uint8_t i = 0; i < s_face.voice_option_count; ++i) {
        if (s_face.voice_options[i] == voice) return i;
    }
    return 0;
}

static uint32_t realtime_voice_option_index(pet_realtime_voice_t voice)
{
    for (uint8_t i = 0; i < s_face.realtime_voice_option_count; ++i) {
        if (s_face.realtime_voice_options[i] == voice) return i;
    }
    return 0;
}

static uint32_t cartesia_gender_option_index(
    pet_cartesia_voice_gender_t gender)
{
    for (uint8_t i = 0; i < s_face.cartesia_gender_option_count; ++i) {
        if (s_face.cartesia_gender_options[i] == gender) return i;
    }
    return 0;
}

static void rebuild_voice_dropdowns(void)
{
    if (!s_face.voice_dropdown || !s_face.realtime_voice_dropdown ||
        !s_face.cartesia_voice_gender_dropdown) return;
    char voice_options[96] = {0};
    bool first = true;
    s_face.voice_option_count = 0;
    for (pet_voice_t voice = PET_VOICE_PUCK; voice < PET_VOICE_COUNT; ++voice) {
        if (!pet_face_voice_allowed(s_face.gender, voice)) continue;
        s_face.voice_options[s_face.voice_option_count++] = voice;
        append_dropdown_option(voice_options, sizeof(voice_options),
                               pet_voice_name(voice), &first);
    }
    lv_dropdown_set_options(s_face.voice_dropdown, voice_options);
    lv_dropdown_set_selected(s_face.voice_dropdown,
                             voice_option_index(s_face.voice));

    char realtime_options[160] = {0};
    first = true;
    s_face.realtime_voice_option_count = 0;
    for (pet_realtime_voice_t voice = PET_REALTIME_VOICE_CEDAR;
         voice < PET_REALTIME_VOICE_COUNT; ++voice) {
        if (!pet_face_realtime_voice_allowed(s_face.gender, voice)) continue;
        s_face.realtime_voice_options[s_face.realtime_voice_option_count++] = voice;
        append_dropdown_option(realtime_options, sizeof(realtime_options),
                               pet_realtime_voice_name(voice), &first);
    }
    lv_dropdown_set_options(s_face.realtime_voice_dropdown, realtime_options);
    lv_dropdown_set_selected(s_face.realtime_voice_dropdown,
                             realtime_voice_option_index(s_face.realtime_voice));

    char cartesia_options[40] = {0};
    first = true;
    s_face.cartesia_gender_option_count = 0;
    for (pet_cartesia_voice_gender_t gender = PET_CARTESIA_VOICE_NEUTRAL;
         gender < PET_CARTESIA_VOICE_GENDER_COUNT; ++gender) {
        if (!pet_face_cartesia_gender_allowed(s_face.gender, gender)) continue;
        s_face.cartesia_gender_options[s_face.cartesia_gender_option_count++] = gender;
        const char *name = gender == PET_CARTESIA_VOICE_MALE ? "Male" :
            gender == PET_CARTESIA_VOICE_FEMALE ? "Female" : "Neutral";
        append_dropdown_option(cartesia_options, sizeof(cartesia_options), name,
                               &first);
    }
    lv_dropdown_set_options(s_face.cartesia_voice_gender_dropdown,
                            cartesia_options);
    lv_dropdown_set_selected(s_face.cartesia_voice_gender_dropdown,
        cartesia_gender_option_index(s_face.cartesia_voice_gender));
}

static void voice_choice_event(lv_event_t *event)
{
    (void)event;
    uint32_t selected = lv_dropdown_get_selected(s_face.voice_dropdown);
    if (selected >= s_face.voice_option_count) return;
    pet_voice_t voice = s_face.voice_options[selected];
    if (voice == s_face.voice) return;
    set_dropdown_enabled(s_face.voice_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving voice...");
    if (s_face.callbacks.voice_changed) s_face.callbacks.voice_changed(voice);
    else set_dropdown_enabled(s_face.voice_dropdown, true);
}

static void animation_profile_choice_event(lv_event_t *event)
{
    (void)event;
    pet_animation_profile_t profile =
        (pet_animation_profile_t)lv_dropdown_get_selected(s_face.animation_profile_dropdown);
    if (!pet_animation_profile_valid(profile)) return;
    if (profile == s_face.animation_profile) {
        close_settings(true);
        return;
    }
    set_dropdown_enabled(s_face.animation_profile_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving animation profile...");
    if (s_face.callbacks.animation_profile_changed) {
        s_face.callbacks.animation_profile_changed(profile);
    } else {
        set_dropdown_enabled(s_face.animation_profile_dropdown, true);
    }
}

static void ai_mode_choice_event(lv_event_t *event)
{
    (void)event;
    pet_ai_mode_t mode = (pet_ai_mode_t)lv_dropdown_get_selected(s_face.ai_mode_dropdown);
    if (mode == s_face.ai_mode) return;
    set_dropdown_enabled(s_face.ai_mode_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving AI mode...");
    if (s_face.callbacks.ai_mode_changed) s_face.callbacks.ai_mode_changed(mode);
    else set_dropdown_enabled(s_face.ai_mode_dropdown, true);
}

static bool speech_profile_for_index(uint32_t selected, pet_speech_profile_t *profile)
{
    uint32_t index=0;
    for(pet_speech_profile_t candidate=0;candidate<PET_SPEECH_PROFILE_COUNT;candidate++){
        bool allowed=candidate==PET_SPEECH_PROFILE_CARTESIA_BATCH?s_face.speech_allow_cartesia_batch:
                     candidate==PET_SPEECH_PROFILE_CARTESIA_REALTIME?s_face.speech_allow_cartesia_realtime:s_face.speech_allow_fish;
        if(allowed&&index++==selected){*profile=candidate;return true;}
    }
    return false;
}

static uint32_t speech_profile_index(pet_speech_profile_t profile)
{
    uint32_t index=0;
    for(pet_speech_profile_t candidate=0;candidate<profile;candidate++){
        if((candidate==PET_SPEECH_PROFILE_CARTESIA_BATCH&&s_face.speech_allow_cartesia_batch)||
           (candidate==PET_SPEECH_PROFILE_CARTESIA_REALTIME&&s_face.speech_allow_cartesia_realtime)||
           (candidate==PET_SPEECH_PROFILE_FISH_DIRECT&&s_face.speech_allow_fish))index++;
    }
    return index;
}

static void speech_profile_choice_event(lv_event_t *event)
{
    (void)event;
    uint32_t selected = lv_dropdown_get_selected(s_face.speech_profile_dropdown);
    pet_speech_profile_t mode;
    if(!speech_profile_for_index(selected,&mode))return;
    if (mode == s_face.speech_profile) return;
    set_dropdown_enabled(s_face.speech_profile_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving speech profile...");
    if (s_face.callbacks.speech_profile_changed) s_face.callbacks.speech_profile_changed(mode);
    else set_dropdown_enabled(s_face.speech_profile_dropdown, true);
}

static void realtime_model_choice_event(lv_event_t *event)
{
    (void)event;
    pet_realtime_model_t model = (pet_realtime_model_t)lv_dropdown_get_selected(s_face.realtime_model_dropdown);
    if (model == s_face.realtime_model) return;
    set_dropdown_enabled(s_face.realtime_model_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving realtime model...");
    if (s_face.callbacks.realtime_model_changed) s_face.callbacks.realtime_model_changed(model);
    else set_dropdown_enabled(s_face.realtime_model_dropdown, true);
}

static void cartesia_voice_gender_choice_event(lv_event_t *event)
{
    (void)event;
    uint32_t selected = lv_dropdown_get_selected(
        s_face.cartesia_voice_gender_dropdown);
    if (selected >= s_face.cartesia_gender_option_count) return;
    pet_cartesia_voice_gender_t gender =
        s_face.cartesia_gender_options[selected];
    if (gender == s_face.cartesia_voice_gender) return;
    set_dropdown_enabled(s_face.cartesia_voice_gender_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving Cartesia voice...");
    if (s_face.callbacks.cartesia_voice_gender_changed) {
        s_face.callbacks.cartesia_voice_gender_changed(gender);
    } else {
        set_dropdown_enabled(s_face.cartesia_voice_gender_dropdown, true);
    }
}

static void realtime_voice_choice_event(lv_event_t *event)
{
    (void)event;
    uint32_t selected = lv_dropdown_get_selected(s_face.realtime_voice_dropdown);
    if (selected >= s_face.realtime_voice_option_count) return;
    pet_realtime_voice_t voice = s_face.realtime_voice_options[selected];
    if (voice == s_face.realtime_voice) return;
    set_dropdown_enabled(s_face.realtime_voice_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving realtime voice...");
    if (s_face.callbacks.realtime_voice_changed) s_face.callbacks.realtime_voice_changed(voice);
    else set_dropdown_enabled(s_face.realtime_voice_dropdown, true);
}

static void realtime_boost_choice_event(lv_event_t *event)
{
    (void)event;
    pet_realtime_boost_t boost =
        (pet_realtime_boost_t)lv_dropdown_get_selected(s_face.realtime_boost_dropdown);
    if (boost == s_face.realtime_boost) return;
    set_dropdown_enabled(s_face.realtime_boost_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving realtime boost...");
    if (s_face.callbacks.realtime_boost_changed) s_face.callbacks.realtime_boost_changed(boost);
    else set_dropdown_enabled(s_face.realtime_boost_dropdown, true);
}

static void recording_timeout_choice_event(lv_event_t *event)
{
    (void)event;
    uint32_t selected = lv_dropdown_get_selected(s_face.recording_timeout_dropdown);
    uint16_t seconds = pet_config_recording_timeout_for_index((uint8_t)selected);
    if (seconds == s_face.recording_timeout_seconds) return;
    set_dropdown_enabled(s_face.recording_timeout_dropdown, false);
    lv_label_set_text(s_face.wifi_status, "saving recording limit...");
    if (s_face.callbacks.recording_timeout_changed) {
        s_face.callbacks.recording_timeout_changed(seconds);
    } else {
        set_dropdown_enabled(s_face.recording_timeout_dropdown, true);
    }
}

static void create_settings_ui(uint8_t initial_volume, uint8_t initial_brightness,
                               uint8_t initial_shake_sensitivity,
                               const char *current_ssid,
                               pet_voice_t initial_voice, uint16_t initial_recording_timeout,
                               pet_animation_profile_t initial_animation_profile,
                               pet_ai_mode_t initial_ai_mode,
                               pet_speech_profile_t initial_speech_profile,
                               pet_realtime_model_t initial_realtime_model,
                               pet_realtime_voice_t initial_realtime_voice,
                               pet_realtime_boost_t initial_realtime_boost,
                               pet_cartesia_voice_gender_t initial_cartesia_voice_gender)
{
    s_face.settings_panel = lv_obj_create(s_face.screen);
    lv_obj_remove_style_all(s_face.settings_panel);
    lv_obj_set_size(s_face.settings_panel, 360, 360);
    lv_obj_center(s_face.settings_panel);
    lv_obj_set_style_bg_color(s_face.settings_panel, lv_color_hex(SETTINGS_BG), 0);
    lv_obj_set_style_bg_opa(s_face.settings_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_face.settings_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_face.settings_panel, settings_gesture_event, LV_EVENT_GESTURE, NULL);

    lv_obj_t *done_bottom = create_button(s_face.settings_panel, "Done", 96, close_settings_event);
    lv_obj_align(done_bottom, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_t *content = lv_obj_create(s_face.settings_panel);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 300, 258);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    /* Extra tail room lets the final controls scroll into the wide center of
     * the round screen instead of remaining trapped against its lower edge. */
    lv_obj_set_style_pad_bottom(content, 86, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(SETTINGS_BORDER), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_SCROLLBAR);

    s_face.battery_card = lv_obj_create(content);
    lv_obj_set_size(s_face.battery_card, 272, 92);
    lv_obj_align(s_face.battery_card, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(s_face.battery_card, lv_color_hex(SETTINGS_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_face.battery_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_face.battery_card, 0, 0);
    lv_obj_set_style_radius(s_face.battery_card, 18, 0);
    lv_obj_remove_flag(s_face.battery_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *battery_label = lv_label_create(s_face.battery_card);
    lv_label_set_text(battery_label, "Battery");
    lv_obj_set_style_text_color(battery_label, lv_color_hex(SETTINGS_TEXT), 0);
    lv_obj_align(battery_label, LV_ALIGN_TOP_LEFT, 4, -2);
    s_face.battery_percent = lv_label_create(s_face.battery_card);
    lv_label_set_text(s_face.battery_percent, "--%");
    lv_obj_set_style_text_color(s_face.battery_percent, lv_color_hex(SETTINGS_TEXT), 0);
    lv_obj_align(s_face.battery_percent, LV_ALIGN_TOP_RIGHT, -4, -2);
    s_face.battery_bar = lv_bar_create(s_face.battery_card);
    lv_obj_set_size(s_face.battery_bar, 250, 12);
    lv_obj_align(s_face.battery_bar, LV_ALIGN_TOP_MID, 0, 24);
    lv_bar_set_range(s_face.battery_bar, 0, 100);
    lv_bar_set_value(s_face.battery_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_face.battery_bar, lv_color_hex(SETTINGS_CONTROL),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_face.battery_bar, lv_color_hex(SETTINGS_ACCENT),
                              LV_PART_INDICATOR);
    s_face.battery_detail = lv_label_create(s_face.battery_card);
    lv_obj_set_size(s_face.battery_detail, 260, 38);
    lv_obj_set_style_text_align(s_face.battery_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_face.battery_detail, lv_color_hex(SETTINGS_MUTED), 0);
    lv_label_set_long_mode(s_face.battery_detail, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_face.battery_detail, "Battery unavailable");
    lv_obj_align(s_face.battery_detail, LV_ALIGN_TOP_MID, 0, 43);

    lv_obj_t *face_label = lv_label_create(content);
    lv_label_set_text(face_label, "Face");
    lv_obj_set_style_text_color(face_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(face_label, LV_ALIGN_TOP_LEFT, 20, 116);
    s_face.style_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.style_dropdown, 158, 34);
    char face_options[160] = {0};
    size_t face_options_used = 0;
    s_face.face_option_count = 0;
    pet_face_catalog_item_t catalog[PET_FACE_CATALOG_CAPACITY];
    size_t catalog_count = 0;
    if (pet_face_pack_list(catalog, PET_FACE_CATALOG_CAPACITY,
                           &catalog_count) == ESP_OK) {
        for (size_t index = 0;
             index < catalog_count && index < PET_FACE_CATALOG_CAPACITY; ++index) {
            add_face_option(face_options, sizeof(face_options),
                            &face_options_used, catalog[index].id,
                            catalog[index].name);
        }
    }
    if (!s_face.face_option_count) {
        /* The procedural renderer is a boot-safety fallback, not a catalog
         * character. Keep it hidden when no full-frame pack validates. */
        strlcpy(face_options, "No validated faces", sizeof(face_options));
    }
    lv_dropdown_set_options(s_face.style_dropdown, face_options);
    lv_dropdown_set_selected(s_face.style_dropdown,
                             face_option_index(s_face.selected_face_id));
    lv_obj_align(s_face.style_dropdown, LV_ALIGN_TOP_RIGHT, -12, 104);
    lv_obj_add_event_cb(s_face.style_dropdown, style_choice_event, LV_EVENT_VALUE_CHANGED, NULL);
    if (!s_face.face_option_count) {
        set_dropdown_enabled(s_face.style_dropdown, false);
    }

    lv_obj_t *speech_mouth_label = lv_label_create(content);
    lv_label_set_text(speech_mouth_label, "Speech animation");
    lv_obj_set_style_text_color(speech_mouth_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(speech_mouth_label, LV_ALIGN_TOP_LEFT, 20, 155);
    s_face.speech_mouth_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.speech_mouth_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.speech_mouth_dropdown,
                            "Character-authored (7 stages)");
    lv_dropdown_set_selected(s_face.speech_mouth_dropdown, 0u);
    lv_obj_align(s_face.speech_mouth_dropdown, LV_ALIGN_TOP_RIGHT, -12, 143);
    /* Stage count and geometry belong to the selected full-frame pack. Keep this
     * read-only row so users can see the effective behavior without exposing
     * the obsolete six-option V1 renderer selector. */
    set_dropdown_enabled(s_face.speech_mouth_dropdown, false);

    lv_obj_t *ai_mode_label = lv_label_create(content);
    lv_label_set_text(ai_mode_label, "AI mode");
    lv_obj_set_style_text_color(ai_mode_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(ai_mode_label, LV_ALIGN_TOP_LEFT, 20, 194);
    s_face.ai_mode_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.ai_mode_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.ai_mode_dropdown, "OpenRouter classic\nOpenAI Realtime\nCloud speech pipeline");
    lv_dropdown_set_selected(s_face.ai_mode_dropdown, initial_ai_mode);
    lv_obj_align(s_face.ai_mode_dropdown, LV_ALIGN_TOP_RIGHT, -12, 182);
    lv_obj_add_event_cb(s_face.ai_mode_dropdown, ai_mode_choice_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *speech_profile_label = lv_label_create(content);
    lv_label_set_text(speech_profile_label, "Speech profile");
    lv_obj_set_style_text_color(speech_profile_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(speech_profile_label, LV_ALIGN_TOP_LEFT, 20, 506);
    s_face.speech_profile_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.speech_profile_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.speech_profile_dropdown, "Cartesia - economical\nCartesia - realtime\nFish Audio - S2 Pro");
    lv_dropdown_set_selected(s_face.speech_profile_dropdown, initial_speech_profile);
    lv_obj_align(s_face.speech_profile_dropdown, LV_ALIGN_TOP_RIGHT, -12, 494);
    lv_obj_add_event_cb(s_face.speech_profile_dropdown, speech_profile_choice_event, LV_EVENT_VALUE_CHANGED, NULL);
    s_face.speech_allow_cartesia_batch = true;
    s_face.speech_allow_cartesia_realtime = true;
    s_face.speech_allow_fish = true;

    lv_obj_t *cartesia_gender_label = lv_label_create(content);
    lv_label_set_text(cartesia_gender_label, "Cartesia voice");
    lv_obj_set_style_text_color(cartesia_gender_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(cartesia_gender_label, LV_ALIGN_TOP_LEFT, 20, 233);
    s_face.cartesia_voice_gender_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.cartesia_voice_gender_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.cartesia_voice_gender_dropdown, "Neutral\nMale\nFemale");
    lv_dropdown_set_selected(s_face.cartesia_voice_gender_dropdown, initial_cartesia_voice_gender);
    lv_obj_align(s_face.cartesia_voice_gender_dropdown, LV_ALIGN_TOP_RIGHT, -12, 221);
    lv_obj_add_event_cb(s_face.cartesia_voice_gender_dropdown,
                        cartesia_voice_gender_choice_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *realtime_model_label = lv_label_create(content);
    lv_label_set_text(realtime_model_label, "RT model");
    lv_obj_set_style_text_color(realtime_model_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(realtime_model_label, LV_ALIGN_TOP_LEFT, 20, 272);
    s_face.realtime_model_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.realtime_model_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.realtime_model_dropdown, "2.1 mini (fast)\n2.1 (smart)\n1.5 (voice)");
    lv_dropdown_set_selected(s_face.realtime_model_dropdown, initial_realtime_model);
    lv_obj_align(s_face.realtime_model_dropdown, LV_ALIGN_TOP_RIGHT, -12, 260);
    lv_obj_add_event_cb(s_face.realtime_model_dropdown, realtime_model_choice_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *realtime_voice_label = lv_label_create(content);
    lv_label_set_text(realtime_voice_label, "RT voice");
    lv_obj_set_style_text_color(realtime_voice_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(realtime_voice_label, LV_ALIGN_TOP_LEFT, 20, 311);
    s_face.realtime_voice_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.realtime_voice_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.realtime_voice_dropdown, "cedar\nmarin\nalloy\nash\nballad\ncoral\necho\nsage\nshimmer\nverse");
    lv_dropdown_set_selected(s_face.realtime_voice_dropdown, initial_realtime_voice);
    lv_obj_align(s_face.realtime_voice_dropdown, LV_ALIGN_TOP_RIGHT, -12, 299);
    lv_obj_add_event_cb(s_face.realtime_voice_dropdown, realtime_voice_choice_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *realtime_boost_label = lv_label_create(content);
    lv_label_set_text(realtime_boost_label, "Realtime boost");
    lv_obj_set_style_text_color(realtime_boost_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(realtime_boost_label, LV_ALIGN_TOP_LEFT, 20, 350);
    s_face.realtime_boost_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.realtime_boost_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.realtime_boost_dropdown, "Off\n+3 dB\n+6 dB\n+9 dB");
    lv_dropdown_set_selected(s_face.realtime_boost_dropdown, initial_realtime_boost);
    lv_obj_align(s_face.realtime_boost_dropdown, LV_ALIGN_TOP_RIGHT, -12, 338);
    lv_obj_add_event_cb(s_face.realtime_boost_dropdown, realtime_boost_choice_event,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *voice_label = lv_label_create(content);
    lv_label_set_text(voice_label, "Classic voice");
    lv_obj_set_style_text_color(voice_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(voice_label, LV_ALIGN_TOP_LEFT, 20, 389);
    s_face.voice_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.voice_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.voice_dropdown, "Puck\nKore\nCharon\nFenrir\nAoede\nLeda");
    lv_dropdown_set_selected(s_face.voice_dropdown, initial_voice);
    lv_obj_align(s_face.voice_dropdown, LV_ALIGN_TOP_RIGHT, -12, 377);
    lv_obj_add_event_cb(s_face.voice_dropdown, voice_choice_event, LV_EVENT_VALUE_CHANGED, NULL);
    rebuild_voice_dropdowns();

    lv_obj_t *animation_label = lv_label_create(content);
    lv_label_set_text(animation_label, "Animation");
    lv_obj_set_style_text_color(animation_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(animation_label, LV_ALIGN_TOP_LEFT, 20, 428);
    s_face.animation_profile_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.animation_profile_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.animation_profile_dropdown, "Full\nBalanced\nReduced");
    lv_dropdown_set_selected(s_face.animation_profile_dropdown, initial_animation_profile);
    lv_obj_align(s_face.animation_profile_dropdown, LV_ALIGN_TOP_RIGHT, -12, 416);
    lv_obj_add_event_cb(s_face.animation_profile_dropdown,
                        animation_profile_choice_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *recording_label = lv_label_create(content);
    lv_label_set_text(recording_label, "Record max");
    lv_obj_set_style_text_color(recording_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(recording_label, LV_ALIGN_TOP_LEFT, 20, 467);
    s_face.recording_timeout_dropdown = lv_dropdown_create(content);
    lv_obj_set_size(s_face.recording_timeout_dropdown, 158, 34);
    lv_dropdown_set_options(s_face.recording_timeout_dropdown,
                            "8 seconds\n15 seconds\n30 seconds\n60 seconds\n180 seconds\n600 seconds");
    lv_dropdown_set_selected(s_face.recording_timeout_dropdown,
                             pet_config_recording_timeout_index(initial_recording_timeout));
    lv_obj_align(s_face.recording_timeout_dropdown, LV_ALIGN_TOP_RIGHT, -12, 455);
    lv_obj_add_event_cb(s_face.recording_timeout_dropdown,
                        recording_timeout_choice_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *volume_label = lv_label_create(content);
    lv_label_set_text(volume_label, "Volume");
    lv_obj_set_style_text_color(volume_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(volume_label, LV_ALIGN_TOP_LEFT, 20, 545);
    s_face.volume_value = lv_label_create(content);
    lv_obj_set_style_text_color(s_face.volume_value, lv_color_hex(SETTINGS_TEXT), 0);
    lv_obj_align(s_face.volume_value, LV_ALIGN_TOP_RIGHT, -18, 545);

    s_face.volume_slider = lv_slider_create(content);
    lv_obj_set_width(s_face.volume_slider, 250);
    lv_slider_set_range(s_face.volume_slider, 0, 100);
    lv_slider_set_value(s_face.volume_slider, initial_volume, LV_ANIM_OFF);
    lv_obj_align(s_face.volume_slider, LV_ALIGN_TOP_MID, 0, 565);
    lv_obj_add_event_cb(s_face.volume_slider, volume_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_face.volume_slider, volume_event, LV_EVENT_RELEASED, NULL);
    char volume_text[16];
    snprintf(volume_text, sizeof(volume_text), "%u%%", initial_volume);
    lv_label_set_text(s_face.volume_value, volume_text);

    lv_obj_t *brightness_label = lv_label_create(content);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_set_style_text_color(brightness_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 20, 595);
    s_face.brightness_value = lv_label_create(content);
    lv_obj_set_style_text_color(s_face.brightness_value, lv_color_hex(SETTINGS_TEXT), 0);
    lv_obj_align(s_face.brightness_value, LV_ALIGN_TOP_RIGHT, -18, 595);
    s_face.brightness_slider = lv_slider_create(content);
    lv_obj_set_width(s_face.brightness_slider, 250);
    lv_slider_set_range(s_face.brightness_slider, PET_BRIGHTNESS_MIN, 100);
    lv_slider_set_value(s_face.brightness_slider, initial_brightness, LV_ANIM_OFF);
    lv_obj_align(s_face.brightness_slider, LV_ALIGN_TOP_MID, 0, 615);
    lv_obj_add_event_cb(s_face.brightness_slider, brightness_event,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_face.brightness_slider, brightness_event,
                        LV_EVENT_RELEASED, NULL);
    char brightness_text[16];
    snprintf(brightness_text, sizeof(brightness_text), "%u%%", initial_brightness);
    lv_label_set_text(s_face.brightness_value, brightness_text);

    lv_obj_t *shake_label = lv_label_create(content);
    lv_label_set_text(shake_label, "Shake sensitivity");
    lv_obj_set_style_text_color(shake_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(shake_label, LV_ALIGN_TOP_LEFT, 20, 645);
    s_face.shake_sensitivity_value = lv_label_create(content);
    lv_obj_set_style_text_color(s_face.shake_sensitivity_value,
                                lv_color_hex(SETTINGS_TEXT), 0);
    lv_obj_align(s_face.shake_sensitivity_value, LV_ALIGN_TOP_RIGHT, -18, 645);
    s_face.shake_sensitivity_slider = lv_slider_create(content);
    lv_obj_set_width(s_face.shake_sensitivity_slider, 250);
    lv_slider_set_range(s_face.shake_sensitivity_slider, 0, 100);
    lv_slider_set_value(s_face.shake_sensitivity_slider,
                        initial_shake_sensitivity, LV_ANIM_OFF);
    lv_obj_align(s_face.shake_sensitivity_slider, LV_ALIGN_TOP_MID, 0, 665);
    lv_obj_add_event_cb(s_face.shake_sensitivity_slider, shake_sensitivity_event,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_face.shake_sensitivity_slider, shake_sensitivity_event,
                        LV_EVENT_RELEASED, NULL);
    char shake_text[24];
    if (!initial_shake_sensitivity) snprintf(shake_text, sizeof(shake_text), "Off");
    else snprintf(shake_text, sizeof(shake_text), "%u%% %s",
                  initial_shake_sensitivity,
                  shake_sensitivity_label(initial_shake_sensitivity));
    lv_label_set_text(s_face.shake_sensitivity_value, shake_text);

    lv_obj_t *wifi_label = lv_label_create(content);
    lv_label_set_text(wifi_label, "Wi-Fi");
    lv_obj_set_width(wifi_label, 250);
    lv_obj_set_style_text_align(wifi_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(SETTINGS_MUTED), 0);
    lv_obj_align(wifi_label, LV_ALIGN_TOP_MID, 0, 695);

    s_face.wifi_dropdown = lv_dropdown_create(content);
    lv_obj_set_width(s_face.wifi_dropdown, 250);
    lv_obj_align(s_face.wifi_dropdown, LV_ALIGN_TOP_MID, 0, 715);
    if (current_ssid && current_ssid[0]) {
        strlcpy(s_face.current_ssid, current_ssid, sizeof(s_face.current_ssid));
        strlcpy(s_face.wifi_names[0], current_ssid, sizeof(s_face.wifi_names[0]));
        s_face.wifi_saved[0] = true;
        s_face.wifi_count = 1;
        char current_option[PET_SSID_MAX + 13];
        snprintf(current_option, sizeof(current_option), "%s (connected)",
                 current_ssid);
        lv_dropdown_set_options(s_face.wifi_dropdown, current_option);
    } else {
        lv_dropdown_set_options(s_face.wifi_dropdown, "scanning...");
    }

    lv_obj_t *scan = create_button(content, "Scan", 96, request_scan_event);
    lv_obj_align(scan, LV_ALIGN_TOP_MID, -57, 755);
    lv_obj_t *join = create_button(content, "Join", 96, show_wifi_password_event);
    lv_obj_align(join, LV_ALIGN_TOP_MID, 57, 755);

    s_face.wifi_status = lv_label_create(content);
    lv_obj_set_width(s_face.wifi_status, 250);
    lv_obj_set_style_text_align(s_face.wifi_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_face.wifi_status, lv_color_hex(SETTINGS_MUTED), 0);
    lv_label_set_text(s_face.wifi_status, "Done or swipe up to close");
    lv_obj_align(s_face.wifi_status, LV_ALIGN_TOP_MID, 0, 799);

    lv_obj_t *dropdowns[] = {
        s_face.style_dropdown,
        s_face.speech_mouth_dropdown,
        s_face.ai_mode_dropdown,
        s_face.speech_profile_dropdown,
        s_face.cartesia_voice_gender_dropdown,
        s_face.realtime_model_dropdown,
        s_face.realtime_voice_dropdown,
        s_face.realtime_boost_dropdown,
        s_face.voice_dropdown,
        s_face.animation_profile_dropdown,
        s_face.recording_timeout_dropdown,
        s_face.wifi_dropdown,
    };
    for (size_t i = 0; i < sizeof(dropdowns) / sizeof(dropdowns[0]); ++i) {
        style_settings_dropdown(dropdowns[i]);
    }
    style_settings_slider(s_face.volume_slider);
    style_settings_slider(s_face.brightness_slider);
    style_settings_slider(s_face.shake_sensitivity_slider);
    lv_obj_add_flag(s_face.settings_panel, LV_OBJ_FLAG_HIDDEN);

    s_face.wifi_password_panel = lv_obj_create(s_face.screen);
    lv_obj_remove_style_all(s_face.wifi_password_panel);
    lv_obj_set_size(s_face.wifi_password_panel, 360, 360);
    lv_obj_center(s_face.wifi_password_panel);
    lv_obj_set_style_bg_color(s_face.wifi_password_panel, lv_color_hex(SETTINGS_BG), 0);
    lv_obj_set_style_bg_opa(s_face.wifi_password_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_face.wifi_password_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_face.wifi_password_title = lv_label_create(s_face.wifi_password_panel);
    lv_obj_set_width(s_face.wifi_password_title, 270);
    lv_obj_set_style_text_align(s_face.wifi_password_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_face.wifi_password_title, lv_color_hex(SETTINGS_TEXT), 0);
    lv_label_set_text(s_face.wifi_password_title, "Join Wi-Fi");
    lv_obj_align(s_face.wifi_password_title, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t *wifi_password_back = create_button(
        s_face.wifi_password_panel, "Back", 96, wifi_password_back_event);
    lv_obj_set_height(wifi_password_back, 36);
    lv_obj_align(wifi_password_back, LV_ALIGN_TOP_MID, 0, 8);

    s_face.wifi_password = lv_textarea_create(s_face.wifi_password_panel);
    lv_obj_set_size(s_face.wifi_password, 270, 40);
    lv_obj_align(s_face.wifi_password, LV_ALIGN_TOP_MID, 0, 82);
    lv_textarea_set_one_line(s_face.wifi_password, true);
    lv_textarea_set_password_mode(s_face.wifi_password, true);
    lv_textarea_set_max_length(s_face.wifi_password, PET_PASSWORD_MAX - 1);
    lv_textarea_set_placeholder_text(s_face.wifi_password, "password (blank for open)");
    lv_obj_set_style_bg_color(s_face.wifi_password, lv_color_hex(SETTINGS_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_face.wifi_password, lv_color_hex(SETTINGS_TEXT), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_face.wifi_password, lv_color_hex(SETTINGS_BORDER), LV_PART_MAIN);

    s_face.keyboard = lv_keyboard_create(s_face.wifi_password_panel);
    /* The original edge-to-edge keyboard put its outer keys beyond the round
     * panel's usable chord. This centered keyboard keeps every key tappable. */
    lv_obj_set_size(s_face.keyboard, 270, 170);
    lv_obj_align(s_face.keyboard, LV_ALIGN_TOP_MID, 0, 128);
    lv_keyboard_set_mode(s_face.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_face.keyboard, s_face.wifi_password);
    lv_obj_set_style_bg_color(s_face.keyboard, lv_color_hex(SETTINGS_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_face.keyboard, lv_color_hex(SETTINGS_CONTROL), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_face.keyboard, lv_color_hex(SETTINGS_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_face.keyboard, lv_color_hex(SETTINGS_BORDER), LV_PART_ITEMS);
    lv_obj_add_event_cb(s_face.wifi_password, wifi_password_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_face.keyboard, wifi_password_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_face.wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t pet_face_init(const pet_face_callbacks_t *callbacks, uint8_t initial_volume,
                        uint8_t initial_brightness, uint8_t initial_shake_sensitivity,
                        const char *current_ssid, const char *initial_face_id,
                        pet_voice_t initial_voice, uint16_t initial_recording_timeout,
                        pet_animation_profile_t initial_animation_profile,
                        pet_ai_mode_t initial_ai_mode, pet_realtime_model_t initial_realtime_model,
                        pet_speech_profile_t initial_speech_profile,
                        pet_realtime_voice_t initial_realtime_voice,
                        pet_realtime_boost_t initial_realtime_boost,
                        pet_cartesia_voice_gender_t initial_cartesia_voice_gender,
                        pet_face_gender_t initial_face_gender,
                        pet_speech_mouth_mode_t initial_speech_mouth_mode)
{
    if (!callbacks) return ESP_ERR_INVALID_ARG;
    s_face.callbacks = *callbacks;
    s_face.state = PET_FACE_BOOTING;
    s_face.expression = PET_EXPRESSION_IDLE;
    if (initial_face_id && pet_face_id_valid(initial_face_id)) {
        strlcpy(s_face.selected_face_id, initial_face_id,
                sizeof(s_face.selected_face_id));
    } else {
        strlcpy(s_face.selected_face_id, "pablo",
                sizeof(s_face.selected_face_id));
    }
    s_face.gender = pet_face_gender_valid(initial_face_gender) ?
        initial_face_gender : PET_FACE_GENDER_NEUTRAL;
    s_face.brightness = initial_brightness >= PET_BRIGHTNESS_MIN &&
        initial_brightness <= 100 ? initial_brightness : PET_BRIGHTNESS_DEFAULT;
    s_face.shake_sensitivity = initial_shake_sensitivity <= 100 ?
        initial_shake_sensitivity : PET_SHAKE_SENSITIVITY_DEFAULT;
    s_face.voice = initial_voice < PET_VOICE_COUNT ? initial_voice : PET_VOICE_PUCK;
    s_face.ai_mode = initial_ai_mode < PET_AI_MODE_COUNT ? initial_ai_mode : PET_AI_MODE_OPENROUTER;
    s_face.speech_profile = initial_speech_profile < PET_SPEECH_PROFILE_COUNT ?
        initial_speech_profile : PET_SPEECH_PROFILE_CARTESIA_BATCH;
    s_face.realtime_model = initial_realtime_model < PET_REALTIME_MODEL_COUNT ? initial_realtime_model : PET_REALTIME_MODEL_2_1_MINI;
    s_face.realtime_voice = initial_realtime_voice < PET_REALTIME_VOICE_COUNT ? initial_realtime_voice : PET_REALTIME_VOICE_CEDAR;
    s_face.realtime_boost = initial_realtime_boost < PET_REALTIME_BOOST_COUNT ?
        initial_realtime_boost : PET_REALTIME_BOOST_DEFAULT;
    s_face.cartesia_voice_gender =
        initial_cartesia_voice_gender < PET_CARTESIA_VOICE_GENDER_COUNT ?
        initial_cartesia_voice_gender : PET_CARTESIA_VOICE_NEUTRAL;
    s_face.speech_mouth_mode =
        pet_speech_mouth_full_frame_normalize(initial_speech_mouth_mode);
    s_face.active_speech_mouth_mode = s_face.speech_mouth_mode;
    s_face.recording_timeout_seconds =
        pet_config_recording_timeout_valid(initial_recording_timeout) ?
        initial_recording_timeout : PET_RECORDING_TIMEOUT_DEFAULT_SECONDS;
    s_face.animation_profile = pet_animation_profile_valid(initial_animation_profile) ?
        initial_animation_profile : PET_ANIMATION_PROFILE_DEFAULT;
    s_face.conversation_status_state = -1;
    s_face.force_render = true;
    /* vNext already initialized the display for the asset-independent wizard. */
    lv_display_t *display = lv_display_get_default();
    if(!display)display = pet_display_start();
    if (!display) return ESP_FAIL;
    int32_t width = lv_display_get_horizontal_resolution(display);
    int32_t height = lv_display_get_vertical_resolution(display);
    if (width < 2 || height < 2 || width > INT16_MAX || height > INT16_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_face.display_width = (uint16_t)width;
    s_face.display_height = (uint16_t)height;
    size_t draw_buffer_bytes = lv_draw_buf_width_to_stride((uint32_t)width, LV_COLOR_FORMAT_RGB565) *
                               DISPLAY_DRAW_ROWS;
    bsp_display_backlight_on();
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(s_face.brightness), TAG,
                        "apply display brightness");
    ESP_RETURN_ON_ERROR(pet_display_lock(-1), TAG, "display lock");
    ESP_RETURN_ON_ERROR(pet_touch_enable_resilient(), TAG,
                        "enable responsive touch polling");
    /* The default PSRAM draw buffers make the SPI driver allocate temporary
     * internal DMA copies, which can fail after microphone capture starts.
     * Bind two permanent DMA-safe strips instead. TE is disabled in sdkconfig
     * so all strips belonging to one invalidation drain continuously; with TE
     * enabled, each strip waited another panel frame and produced a visible
     * top-to-bottom wave. */
    s_face.display_draw_buffer_a = heap_caps_aligned_alloc(
        64, draw_buffer_bytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_face.display_draw_buffer_b = heap_caps_aligned_alloc(
        64, draw_buffer_bytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_face.display_draw_buffer_a || !s_face.display_draw_buffer_b) {
        pet_display_unlock();
        ESP_LOGE(TAG, "could not reserve two %u-byte internal display DMA strips",
                 (unsigned)draw_buffer_bytes);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(display,
                           s_face.display_draw_buffer_a,
                           s_face.display_draw_buffer_b,
                           draw_buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    ESP_LOGI(TAG, "continuous double display DMA strips rows=%u bytes=%u each",
             (unsigned)DISPLAY_DRAW_ROWS,
             (unsigned)draw_buffer_bytes);
    s_face.screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_face.screen);
    lv_obj_set_style_bg_color(s_face.screen, lv_color_hex(0xf9eefe), 0);
    lv_obj_set_style_bg_opa(s_face.screen, LV_OPA_COVER, 0);
    esp_err_t ai_err = pet_face_ai_create(s_face.screen, &s_face.ai_face);
    if (ai_err != ESP_OK) {
        pet_display_unlock();
        return ai_err;
    }
    esp_err_t pack_err = pet_face_pack_create(
        s_face.screen, s_face.selected_face_id, &s_face.pack_face);
    s_face.pack_active = pack_err == ESP_OK;
    if (s_face.pack_active) {
        pet_face_pack_current_id(s_face.selected_face_id,
                                 sizeof(s_face.selected_face_id));
        lv_obj_add_flag(s_face.ai_face, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_face.pack_face, LV_OBJ_FLAG_HIDDEN);
    } else {
#if CONFIG_PET_VNEXT_ENROLLMENT
        pet_display_unlock();return pack_err;
#endif
        ESP_LOGE(TAG, "no validated full-frame face; showing hidden emergency renderer");
        if (s_face.pack_face) lv_obj_add_flag(s_face.pack_face, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_face.ai_face, LV_OBJ_FLAG_HIDDEN);
    }

    s_face.conversation_status = lv_obj_create(s_face.screen);
    lv_obj_remove_style_all(s_face.conversation_status);
    lv_obj_remove_flag(s_face.conversation_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_face.conversation_status, 116, 36);
    lv_obj_set_style_bg_color(s_face.conversation_status, lv_color_hex(0xf9eefe), 0);
    lv_obj_set_style_bg_opa(s_face.conversation_status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_face.conversation_status, 1, 0);
    lv_obj_set_style_border_color(s_face.conversation_status, lv_color_hex(0xd8c8df), 0);
    lv_obj_set_style_radius(s_face.conversation_status, 18, 0);
    lv_obj_align(s_face.conversation_status, LV_ALIGN_BOTTOM_MID, 0, -16);
    s_face.conversation_status_label = lv_label_create(s_face.conversation_status);
    lv_obj_remove_flag(s_face.conversation_status_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_face.conversation_status_label, lv_color_hex(0x342b35), 0);
    lv_obj_set_style_text_font(s_face.conversation_status_label, &lv_font_montserrat_14, 0);
    lv_obj_center(s_face.conversation_status_label);
    lv_obj_add_flag(s_face.conversation_status, LV_OBJ_FLAG_HIDDEN);

    s_face.tap_surface = lv_obj_create(s_face.screen);
    lv_obj_remove_style_all(s_face.tap_surface);
    lv_obj_set_size(s_face.tap_surface, s_face.display_width, s_face.display_height);
    lv_obj_center(s_face.tap_surface);
    lv_obj_add_flag(s_face.tap_surface, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_face.tap_surface, touch_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_face.tap_surface, touch_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_face.tap_surface, touch_event, LV_EVENT_RELEASED, NULL);

    create_settings_ui(initial_volume, s_face.brightness,
                       s_face.shake_sensitivity, current_ssid, s_face.voice,
                       s_face.recording_timeout_seconds, s_face.animation_profile, s_face.ai_mode,
                       s_face.speech_profile, s_face.realtime_model, s_face.realtime_voice,
                       s_face.realtime_boost, s_face.cartesia_voice_gender);

    lv_screen_load(s_face.screen);
    s_face.timer = lv_timer_create(animation_tick, PET_FACE_TICK_MS, NULL);
    if (!s_face.timer) {
        pet_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (s_face.pack_active) {
        bool rendered = false;
        pet_face_pack_update(s_face.state, s_face.expression, 0, UINT8_MAX,
                             false, 0, 0, 0, 0,
                             s_face.animation_profile, s_face.tick, true,
                             &rendered);
    } else {
        pet_face_ai_render(s_face.state, s_face.expression, 0, 0, false);
    }
    pet_display_unlock();
    return ESP_OK;
}

void pet_face_set_state(pet_face_state_t state)
{
    if (s_face.state != state) s_face.force_render = true;
    s_face.state = state;
}

void pet_face_set_expression(pet_expression_t expression)
{
    if (s_face.expression != expression) s_face.force_render = true;
    s_face.expression = expression;
}
void pet_face_set_audio_level(uint8_t level) { s_face.audio_level = level; }
void pet_face_set_speech_articulation(uint8_t level)
{
    if (s_face.speech_articulation != level) {
        s_face.speech_articulation = level;
        s_face.force_render = true;
    }
}

void pet_face_begin_speech_stream(pet_speech_mouth_mode_t mode)
{
    s_face.active_speech_mouth_mode = pet_speech_mouth_full_frame_normalize(mode);
    s_face.speech_articulation = 0;
    s_face.force_render = true;
}
uint8_t pet_face_trigger_gesture(uint8_t gesture)
{
    /* Full-frame playback never transforms the whole canvas. Semantic gestures
     * resolve to the character's declared touch/reaction action. */
    if (!s_face.pack_active || s_face.settings_open ||
        (s_face.state != PET_FACE_IDLE && s_face.state != PET_FACE_OFFLINE) ||
        pet_display_lock(-1) != ESP_OK) {
        return FC_GESTURE_NONE;
    }
    if (gesture == FC_GESTURE_NONE || gesture >= FC_GESTURE_COUNT) {
        pet_display_unlock();
        return FC_GESTURE_NONE;
    }
    uint8_t active = pet_face_pack_trigger_gesture(gesture);
    if (active != FC_GESTURE_NONE) {
        s_face.force_render = true;
        s_face.interaction_boost_until_tick = s_face.tick + 396;
    }
    pet_display_unlock();
    return active;
}

#if CONFIG_PET_POCKET_TERMINAL
bool pet_face_touch_reaction(void)
{
    if (pet_display_lock(-1) != ESP_OK) return false;
    bool shown = s_face.pack_active && !s_face.settings_open &&
        (s_face.state == PET_FACE_IDLE || s_face.state == PET_FACE_OFFLINE) &&
        pet_face_pack_touch();
    if (shown) {
        s_face.force_render = true;
        s_face.interaction_boost_until_tick = s_face.tick + 396;
    }
    pet_display_unlock();
    return shown;
}

#define TERMINAL_TYPE_MS 24
#define TERMINAL_HOLD_MS 1400

/* LVGL timer: one more character with a cursor, then a hold, then gone. */
static void terminal_step(lv_timer_t *timer)
{
    if (s_face.terminal_typed < s_face.terminal_length) {
        char shown[PET_FACE_TERMINAL_MAX + 2];
        uint8_t typed = ++s_face.terminal_typed;
        bool done = typed == s_face.terminal_length;
        memcpy(shown, s_face.terminal_text, typed);
        shown[typed] = done ? '\0' : '_';
        shown[typed + 1] = '\0';
        lv_label_set_text(s_face.terminal_label, shown);
        if (done) lv_timer_set_period(timer, TERMINAL_HOLD_MS);
        return;
    }
    lv_obj_add_flag(s_face.terminal_line, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

void pet_face_announce(const char *text)
{
    if (!text || !s_face.screen || pet_display_lock(-1) != ESP_OK) return;
    if (!s_face.terminal_line) {
        lv_obj_t *line = lv_obj_create(s_face.screen);
        lv_obj_remove_style_all(line);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(line, lv_color_hex(TERMINAL_BG), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_80, 0);
        lv_obj_set_style_border_width(line, 1, 0);
        lv_obj_set_style_border_color(line, lv_color_hex(TERMINAL_EDGE), 0);
        lv_obj_set_style_radius(line, 3, 0);
        lv_obj_set_style_pad_hor(line, 10, 0);
        lv_obj_set_style_pad_ver(line, 5, 0);
        lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, -64);
        lv_obj_t *label = lv_label_create(line);
        lv_obj_set_style_text_font(label, &pet_menu_font_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(TERMINAL_TEXT), 0);
        lv_timer_t *timer = lv_timer_create(terminal_step, TERMINAL_TYPE_MS, NULL);
        if (!timer) {
            lv_obj_delete(line);
            pet_display_unlock();
            return;
        }
        lv_timer_pause(timer);
        s_face.terminal_line = line;
        s_face.terminal_label = label;
        s_face.terminal_timer = timer;
    }
    strlcpy(s_face.terminal_text, text, sizeof(s_face.terminal_text));
    s_face.terminal_length = (uint8_t)strlen(s_face.terminal_text);
    s_face.terminal_typed = 0;
    lv_label_set_text(s_face.terminal_label, "_");
    lv_obj_remove_flag(s_face.terminal_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_face.terminal_line);
    lv_timer_set_period(s_face.terminal_timer, TERMINAL_TYPE_MS);
    lv_timer_reset(s_face.terminal_timer);
    lv_timer_resume(s_face.terminal_timer);
    pet_display_unlock();
}
#endif

bool pet_face_can_trigger_ambient_gesture(void)
{
    if (pet_display_lock(-1) != ESP_OK) return false;
    bool allowed = !s_face.settings_open &&
        (s_face.state == PET_FACE_IDLE || s_face.state == PET_FACE_OFFLINE) &&
        s_face.pack_active &&
        !pet_face_pack_gesture_active();
    pet_display_unlock();
    return allowed;
}

bool pet_face_shake_detection_enabled(void)
{
    return !s_face.settings_open && s_face.state == PET_FACE_IDLE;
}

void pet_face_show(void)
{
    if(!s_face.screen||pet_display_lock(-1)!=ESP_OK)return;
    close_settings(false);lv_screen_load(s_face.screen);pet_display_unlock();
}

pet_face_state_t pet_face_get_state(void) { return s_face.state; }
uint32_t pet_face_render_heartbeat(void) { return s_face.tick; }
bool pet_face_pack_ready(void)
{
    if(pet_display_lock(-1)!=ESP_OK)return false;
    char id[PET_FACE_ID_MAX];
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    bool ready=s_face.screen&&lv_screen_active()==s_face.screen&&s_face.pack_active&&
        s_face.tick&&(uint32_t)(now-s_face.tick)<2000&&
        pet_face_pack_current_id(id,sizeof(id))==ESP_OK&&!strcmp(id,s_face.selected_face_id);
    pet_display_unlock();return ready;
}

esp_err_t pet_face_set_id(const char *face_id)
{
    if (!pet_face_id_valid(face_id)) return ESP_ERR_INVALID_ARG;
    if (!s_face.ai_face || !s_face.pack_face ||
        pet_display_lock(-1) != ESP_OK) return ESP_FAIL;
    esp_err_t err = pet_face_pack_select(face_id);
    if (err != ESP_OK) {
        pet_display_unlock();
        return err;
    }
    strlcpy(s_face.selected_face_id, face_id,
            sizeof(s_face.selected_face_id));
    s_face.pack_active = true;
    lv_obj_add_flag(s_face.ai_face, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_face.pack_face, LV_OBJ_FLAG_HIDDEN);
    bool rendered = false;
    pet_face_pack_update(
        s_face.state, s_face.expression, s_face.displayed_audio_level,
        UINT8_MAX, false, 0, 0, 0, 0, s_face.animation_profile, s_face.tick,
        true, &rendered);
    s_face.force_render = true;
    lv_dropdown_set_selected(s_face.style_dropdown,
                             face_option_index(s_face.selected_face_id));
    set_dropdown_enabled(s_face.style_dropdown, true);
    close_settings(false);
    pet_display_unlock();
    ESP_LOGI(TAG, "full-frame face changed to %s", face_id);
    return ESP_OK;
}

esp_err_t pet_face_get_id(char *face_id, size_t capacity)
{
    if (!face_id || !capacity) return ESP_ERR_INVALID_ARG;
    if (!s_face.selected_face_id[0]) {
        face_id[0] = 0;
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(face_id, s_face.selected_face_id, capacity);
    return ESP_OK;
}

esp_err_t pet_face_get_agent_id(char *id, size_t capacity)
{
    return pet_face_get_id(id, capacity);
}

void pet_face_id_save_failed(void)
{
    if (!s_face.wifi_status || pet_display_lock(-1) != ESP_OK) return;
    set_dropdown_enabled(s_face.style_dropdown, true);
    lv_dropdown_set_selected(s_face.style_dropdown,
                             face_option_index(s_face.selected_face_id));
    lv_label_set_text(s_face.wifi_status, "could not save face");
    pet_display_unlock();
}

esp_err_t pet_face_set_gender(pet_face_gender_t gender)
{
    if (!pet_face_gender_valid(gender)) return ESP_ERR_INVALID_ARG;
    if (!s_face.settings_panel || pet_display_lock(-1) != ESP_OK) return ESP_FAIL;
    s_face.gender = gender;
    rebuild_voice_dropdowns();
    pet_display_unlock();
    ESP_LOGI(TAG, "face gender changed to %s", pet_face_gender_name(gender));
    return ESP_OK;
}

pet_face_gender_t pet_face_get_gender(void) { return s_face.gender; }

esp_err_t pet_face_set_brightness(uint8_t brightness)
{
    if (brightness < PET_BRIGHTNESS_MIN || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_face.brightness_slider || pet_display_lock(-1) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = bsp_display_brightness_set(brightness);
    if (err != ESP_OK) {
        pet_display_unlock();
        return err;
    }
    s_face.brightness = brightness;
    lv_slider_set_value(s_face.brightness_slider, brightness, LV_ANIM_OFF);
    char label[16];
    snprintf(label, sizeof(label), "%u%%", brightness);
    lv_label_set_text(s_face.brightness_value, label);
    pet_display_unlock();
    return ESP_OK;
}

void pet_face_brightness_save_failed(void)
{
    if (!s_face.brightness_slider || pet_display_lock(-1) != ESP_OK) return;
    bsp_display_brightness_set(s_face.brightness);
    lv_slider_set_value(s_face.brightness_slider, s_face.brightness, LV_ANIM_OFF);
    char label[16];
    snprintf(label, sizeof(label), "%u%%", s_face.brightness);
    lv_label_set_text(s_face.brightness_value, label);
    lv_label_set_text(s_face.wifi_status, "could not save brightness");
    pet_display_unlock();
}

esp_err_t pet_face_set_shake_sensitivity(uint8_t sensitivity)
{
    if (sensitivity > 100) return ESP_ERR_INVALID_ARG;
    if (!s_face.shake_sensitivity_slider || pet_display_lock(-1) != ESP_OK) {
        return ESP_FAIL;
    }
    s_face.shake_sensitivity = sensitivity;
    lv_slider_set_value(s_face.shake_sensitivity_slider, sensitivity, LV_ANIM_OFF);
    char label[24];
    if (!sensitivity) snprintf(label, sizeof(label), "Off");
    else snprintf(label, sizeof(label), "%u%% %s", sensitivity,
                  shake_sensitivity_label(sensitivity));
    lv_label_set_text(s_face.shake_sensitivity_value, label);
    pet_display_unlock();
    return ESP_OK;
}

void pet_face_shake_sensitivity_save_failed(void)
{
    if (!s_face.shake_sensitivity_slider || pet_display_lock(-1) != ESP_OK) return;
    lv_slider_set_value(s_face.shake_sensitivity_slider,
                        s_face.shake_sensitivity, LV_ANIM_OFF);
    char label[24];
    if (!s_face.shake_sensitivity) snprintf(label, sizeof(label), "Off");
    else snprintf(label, sizeof(label), "%u%% %s", s_face.shake_sensitivity,
                  shake_sensitivity_label(s_face.shake_sensitivity));
    lv_label_set_text(s_face.shake_sensitivity_value, label);
    lv_label_set_text(s_face.wifi_status, "could not save shake sensitivity");
    pet_display_unlock();
}

void pet_face_set_battery(const pet_battery_snapshot_t *snapshot)
{
    if (!s_face.battery_percent || !snapshot || pet_display_lock(-1) != ESP_OK) return;
    if (!snapshot->valid) {
        lv_label_set_text(s_face.battery_percent, "--%");
        lv_bar_set_value(s_face.battery_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_face.battery_detail, "Battery unavailable");
    } else {
        char percent[12];
        snprintf(percent, sizeof(percent), "%u%%", snapshot->percent);
        lv_label_set_text(s_face.battery_percent, percent);
        lv_bar_set_value(s_face.battery_bar, snapshot->percent, LV_ANIM_ON);
        char detail[96];
        pet_battery_format_detail(snapshot, detail, sizeof(detail));
        lv_label_set_text(s_face.battery_detail, detail);
    }
    pet_display_unlock();
}

esp_err_t pet_face_set_recording_timeout(uint16_t seconds)
{
    if (!pet_config_recording_timeout_valid(seconds)) return ESP_ERR_INVALID_ARG;
    if (!s_face.recording_timeout_dropdown || pet_display_lock(-1) != ESP_OK) return ESP_FAIL;
    s_face.recording_timeout_seconds = seconds;
    lv_dropdown_set_selected(s_face.recording_timeout_dropdown,
                             pet_config_recording_timeout_index(seconds));
    set_dropdown_enabled(s_face.recording_timeout_dropdown, true);
    pet_display_unlock();
    ESP_LOGI(TAG, "maximum recording changed to %u seconds", seconds);
    return ESP_OK;
}

void pet_face_recording_timeout_save_failed(void)
{
    if (!s_face.wifi_status || pet_display_lock(-1) != ESP_OK) return;
    set_dropdown_enabled(s_face.recording_timeout_dropdown, true);
    lv_dropdown_set_selected(s_face.recording_timeout_dropdown,
                             pet_config_recording_timeout_index(
                                 s_face.recording_timeout_seconds));
    lv_label_set_text(s_face.wifi_status, "could not save recording limit");
    pet_display_unlock();
}

esp_err_t pet_face_set_animation_profile(pet_animation_profile_t profile)
{
    if (!pet_animation_profile_valid(profile)) return ESP_ERR_INVALID_ARG;
    if (!s_face.animation_profile_dropdown || pet_display_lock(-1) != ESP_OK) return ESP_FAIL;
    s_face.animation_profile = profile;
    s_face.displayed_audio_level = 0;
    s_face.mouth_bucket = 0;
    s_face.mouth_changed_tick = s_face.tick;
    s_face.force_render = true;
    lv_dropdown_set_selected(s_face.animation_profile_dropdown, profile);
    set_dropdown_enabled(s_face.animation_profile_dropdown, true);
    close_settings(false);
    pet_display_unlock();
    ESP_LOGI(TAG, "animation profile changed to %s", pet_animation_profile_name(profile));
    return ESP_OK;
}

pet_animation_profile_t pet_face_get_animation_profile(void)
{
    return s_face.animation_profile;
}

void pet_face_animation_profile_save_failed(void)
{
    if (!s_face.wifi_status || pet_display_lock(-1) != ESP_OK) return;
    set_dropdown_enabled(s_face.animation_profile_dropdown, true);
    lv_dropdown_set_selected(s_face.animation_profile_dropdown, s_face.animation_profile);
    lv_label_set_text(s_face.wifi_status, "could not save animation profile");
    pet_display_unlock();
}

esp_err_t pet_face_set_voice(pet_voice_t voice)
{
    if (!pet_face_voice_allowed(s_face.gender, voice)) return ESP_ERR_INVALID_ARG;
    if (!s_face.voice_dropdown || pet_display_lock(-1) != ESP_OK) return ESP_FAIL;
    s_face.voice = voice;
    lv_dropdown_set_selected(s_face.voice_dropdown, voice_option_index(voice));
    set_dropdown_enabled(s_face.voice_dropdown, true);
    close_settings(false);
    pet_display_unlock();
    ESP_LOGI(TAG, "voice changed to %s", pet_voice_name(voice));
    return ESP_OK;
}

pet_voice_t pet_face_get_voice(void) { return s_face.voice; }

void pet_face_voice_save_failed(void)
{
    if (!s_face.wifi_status || pet_display_lock(-1) != ESP_OK) return;
    set_dropdown_enabled(s_face.voice_dropdown, true);
    lv_dropdown_set_selected(s_face.voice_dropdown, voice_option_index(s_face.voice));
    lv_label_set_text(s_face.wifi_status, "could not save voice");
    pet_display_unlock();
}

esp_err_t pet_face_set_ai_mode(pet_ai_mode_t mode)
{
    if (mode >= PET_AI_MODE_COUNT || !s_face.ai_mode_dropdown || pet_display_lock(-1) != ESP_OK) return ESP_ERR_INVALID_ARG;
    s_face.ai_mode = mode;
    lv_dropdown_set_selected(s_face.ai_mode_dropdown, mode);
    set_dropdown_enabled(s_face.ai_mode_dropdown, true);
    lv_label_set_text(s_face.wifi_status, "AI mode saved");
    pet_display_unlock();
    return ESP_OK;
}

esp_err_t pet_face_set_speech_profile(pet_speech_profile_t mode,bool allow_cartesia_batch,bool allow_cartesia_realtime,bool allow_fish)
{
    if(mode>=PET_SPEECH_PROFILE_COUNT||(!allow_cartesia_batch&&!allow_cartesia_realtime&&!allow_fish)||
       (mode==PET_SPEECH_PROFILE_CARTESIA_BATCH&&!allow_cartesia_batch)||(mode==PET_SPEECH_PROFILE_CARTESIA_REALTIME&&!allow_cartesia_realtime)||(mode==PET_SPEECH_PROFILE_FISH_DIRECT&&!allow_fish)||
       !s_face.speech_profile_dropdown||pet_display_lock(-1)!=ESP_OK)return ESP_ERR_INVALID_ARG;
    s_face.speech_profile=mode;s_face.speech_allow_cartesia_batch=allow_cartesia_batch;s_face.speech_allow_cartesia_realtime=allow_cartesia_realtime;s_face.speech_allow_fish=allow_fish;
    char options[96]={0};if(allow_cartesia_batch)strlcat(options,"Cartesia - economical",sizeof(options));if(allow_cartesia_realtime){if(options[0])strlcat(options,"\n",sizeof(options));strlcat(options,"Cartesia - realtime",sizeof(options));}if(allow_fish){if(options[0])strlcat(options,"\n",sizeof(options));strlcat(options,"Fish Audio - S2 Pro",sizeof(options));}
    lv_dropdown_set_options(s_face.speech_profile_dropdown,options);lv_dropdown_set_selected(s_face.speech_profile_dropdown,speech_profile_index(mode));
    set_dropdown_enabled(s_face.speech_profile_dropdown,true);lv_label_set_text(s_face.wifi_status,"speech profile synchronized");pet_display_unlock();return ESP_OK;
}

void pet_face_speech_profile_save_failed(void)
{
    if (!s_face.speech_profile_dropdown || pet_display_lock(-1) != ESP_OK) return;
    lv_dropdown_set_selected(s_face.speech_profile_dropdown,
                             speech_profile_index(s_face.speech_profile));
    set_dropdown_enabled(s_face.speech_profile_dropdown, true);
    lv_label_set_text(s_face.wifi_status, "could not save speech profile");
    pet_display_unlock();
}

esp_err_t pet_face_set_cartesia_voice_gender(pet_cartesia_voice_gender_t gender)
{
    if (!pet_face_cartesia_gender_allowed(s_face.gender, gender) ||
        !s_face.cartesia_voice_gender_dropdown ||
        pet_display_lock(-1) != ESP_OK) return ESP_ERR_INVALID_ARG;
    s_face.cartesia_voice_gender = gender;
    lv_dropdown_set_selected(s_face.cartesia_voice_gender_dropdown,
                             cartesia_gender_option_index(gender));
    set_dropdown_enabled(s_face.cartesia_voice_gender_dropdown, true);
    lv_label_set_text(s_face.wifi_status, "Cartesia voice saved");
    pet_display_unlock();
    return ESP_OK;
}

esp_err_t pet_face_set_realtime_model(pet_realtime_model_t model)
{
    if (model >= PET_REALTIME_MODEL_COUNT || !s_face.realtime_model_dropdown || pet_display_lock(-1) != ESP_OK) return ESP_ERR_INVALID_ARG;
    s_face.realtime_model = model;
    lv_dropdown_set_selected(s_face.realtime_model_dropdown, model);
    set_dropdown_enabled(s_face.realtime_model_dropdown, true);
    lv_label_set_text(s_face.wifi_status, "realtime model saved");
    pet_display_unlock();
    return ESP_OK;
}

esp_err_t pet_face_set_realtime_voice(pet_realtime_voice_t voice)
{
    if (!pet_face_realtime_voice_allowed(s_face.gender, voice) ||
        !s_face.realtime_voice_dropdown || pet_display_lock(-1) != ESP_OK) return ESP_ERR_INVALID_ARG;
    s_face.realtime_voice = voice;
    lv_dropdown_set_selected(s_face.realtime_voice_dropdown,
                             realtime_voice_option_index(voice));
    set_dropdown_enabled(s_face.realtime_voice_dropdown, true);
    lv_label_set_text(s_face.wifi_status, "realtime voice saved");
    pet_display_unlock();
    return ESP_OK;
}

esp_err_t pet_face_set_realtime_boost(pet_realtime_boost_t boost)
{
    if (boost < PET_REALTIME_BOOST_OFF || boost >= PET_REALTIME_BOOST_COUNT ||
        !s_face.realtime_boost_dropdown ||
        pet_display_lock(-1) != ESP_OK) return ESP_ERR_INVALID_ARG;
    s_face.realtime_boost = boost;
    lv_dropdown_set_selected(s_face.realtime_boost_dropdown, boost);
    set_dropdown_enabled(s_face.realtime_boost_dropdown, true);
    lv_label_set_text(s_face.wifi_status, "realtime boost saved");
    pet_display_unlock();
    return ESP_OK;
}

esp_err_t pet_face_set_speech_mouth_mode(pet_speech_mouth_mode_t mode)
{
    if (!pet_speech_mouth_mode_valid(mode) || !s_face.speech_mouth_dropdown ||
        pet_display_lock(-1) != ESP_OK) return ESP_ERR_INVALID_ARG;
    s_face.speech_mouth_mode = pet_speech_mouth_full_frame_normalize(mode);
    s_face.force_render = true;
    lv_dropdown_set_selected(s_face.speech_mouth_dropdown, 0u);
    set_dropdown_enabled(s_face.speech_mouth_dropdown, false);
    lv_label_set_text(s_face.wifi_status,
                      "character-authored 7-stage speech active");
    pet_display_unlock();
    return ESP_OK;
}

pet_speech_mouth_mode_t pet_face_get_speech_mouth_mode(void)
{
    return s_face.speech_mouth_mode;
}

void pet_face_speech_mouth_save_failed(void)
{
    if (!s_face.wifi_status || !s_face.speech_mouth_dropdown ||
        pet_display_lock(-1) != ESP_OK) return;
    set_dropdown_enabled(s_face.speech_mouth_dropdown, false);
    lv_dropdown_set_selected(s_face.speech_mouth_dropdown, 0u);
    lv_label_set_text(s_face.wifi_status,
                      "7-stage speech active; legacy setting not saved");
    pet_display_unlock();
}

void pet_face_ai_setting_save_failed(void)
{
    if (!s_face.wifi_status || pet_display_lock(-1) != ESP_OK) return;
    set_dropdown_enabled(s_face.ai_mode_dropdown, true);
    set_dropdown_enabled(s_face.cartesia_voice_gender_dropdown, true);
    set_dropdown_enabled(s_face.realtime_model_dropdown, true);
    set_dropdown_enabled(s_face.realtime_voice_dropdown, true);
    set_dropdown_enabled(s_face.realtime_boost_dropdown, true);
    lv_dropdown_set_selected(s_face.ai_mode_dropdown, s_face.ai_mode);
    lv_dropdown_set_selected(s_face.cartesia_voice_gender_dropdown,
                             cartesia_gender_option_index(
                                 s_face.cartesia_voice_gender));
    lv_dropdown_set_selected(s_face.realtime_model_dropdown, s_face.realtime_model);
    lv_dropdown_set_selected(s_face.realtime_voice_dropdown,
                             realtime_voice_option_index(s_face.realtime_voice));
    lv_dropdown_set_selected(s_face.realtime_boost_dropdown, s_face.realtime_boost);
    lv_label_set_text(s_face.wifi_status, "could not save realtime setting");
    pet_display_unlock();
}

void pet_face_set_wifi_networks(const pet_wifi_network_t *networks, size_t count,
                                esp_err_t result)
{
    if (!s_face.wifi_dropdown || pet_display_lock(-1) != ESP_OK) return;
    if (result != ESP_OK) {
        char status[64];
        snprintf(status, sizeof(status), "scan failed: %s", esp_err_to_name(result));
        lv_label_set_text(s_face.wifi_status, status);
        pet_display_unlock();
        return;
    }
    if (!networks || !count) {
        s_face.wifi_count = 0;
        lv_dropdown_set_options(s_face.wifi_dropdown, "no networks found");
        lv_label_set_text(s_face.wifi_status, "tap scan to try again");
        pet_display_unlock();
        return;
    }

    if (count > PET_WIFI_SCAN_MAX) count = PET_WIFI_SCAN_MAX;
    char options[PET_WIFI_SCAN_MAX * (PET_SSID_MAX + 13)] = {0};
    s_face.wifi_count = count;
    size_t used = 0;
    uint32_t selected = 0;
    for (size_t i = 0; i < count; ++i) {
        strlcpy(s_face.wifi_names[i], networks[i].ssid, sizeof(s_face.wifi_names[i]));
        bool connected = s_face.current_ssid[0] &&
                         !strcmp(s_face.current_ssid, networks[i].ssid);
        s_face.wifi_saved[i] = networks[i].saved || connected;
        if (connected) selected = i;
        const char *suffix = connected ? " (connected)" :
                             s_face.wifi_saved[i] ? " (saved)" : "";
        int written = snprintf(options + used, sizeof(options) - used, "%s%s%s",
                               i ? "\n" : "", networks[i].ssid, suffix);
        if (written < 0 || (size_t)written >= sizeof(options) - used) break;
        used += (size_t)written;
    }
    lv_dropdown_set_options(s_face.wifi_dropdown, options);
    lv_dropdown_set_selected(s_face.wifi_dropdown, selected);
    lv_label_set_text(s_face.wifi_status,
                      "saved networks reconnect without a password");
    pet_display_unlock();
}

void pet_face_set_wifi_status(const char *status)
{
    if (!status || !s_face.wifi_status || pet_display_lock(-1) != ESP_OK) return;
    lv_label_set_text(s_face.wifi_status, status);
    pet_display_unlock();
}
