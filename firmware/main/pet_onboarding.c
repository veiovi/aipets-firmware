#include "pet_onboarding.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "bsp/esp-bsp.h"
#include "pet_display.h"
#include "pet_board.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pet_menu_assets.h"
#include "pet_network.h"
#include "pet_setup.h"
#include "pet_diagnostics.h"
#include "pet_touch.h"
#if CONFIG_PET_VNEXT_ENROLLMENT && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "pet_usb_wifi.h"
static pet_usb_wifi_t s_usb_wifi;
static bool s_usb_wifi_ready;
static uint32_t s_usb_wifi_last_input;
#endif

/* The control deck uses the board's glass: circular on the 1.85B, rectangular
 * on the AMOLED. Round-page coordinates keep their original safe area; the
 * rectangle gives controls more room without scaling fonts or artwork.
 * Swiping up leaves a page (on the deck: from the bottom handle, or by pulling
 * past the end of the log). No pack, frame_player or pet_face dependency. */
enum {
    BG = 0x0a0e07,    /* the glass */
    CARD = 0x141b0e,  /* card faces */
    UNLIT = 0x28331e, /* unlit segments */
    EDGE = 0x587a3c,  /* card edges and corner marks */
    DIM = 0x86a566,   /* secondary text */
    LABEL = 0xb4d68b, /* card labels */
    LIME = 0xc8ef8f,  /* values, lit segments, a pressed face */
    DARK = 0x0c1108,  /* text on lime */
    AMBER = 0xffc15a, /* needs a look */
    RED = 0xff6b57,   /* failed */
};
/* The 360px layout's type sizes. A screen under 300px wide (the 1.54's 240)
 * takes each one size down; 12px is the smallest. */
#define SMALL_SCREEN (s_width > 0 && s_width < 300)
#define FONT_SMALL (&pet_menu_font_12)
#define FONT_TEXT (SMALL_SCREEN ? &pet_menu_font_12 : &pet_menu_font_16)
#define FONT_TITLE (SMALL_SCREEN ? &pet_menu_font_16 : &pet_menu_font_20)
#define FONT_BIG (SMALL_SCREEN ? &pet_menu_font_20 : &pet_menu_font_28)
#if CONFIG_PET_POCKET_TERMINAL
#define HOST "pocket"
#define SYSTEM "aipet-pocket"
#else
#define HOST "aipet"
#define SYSTEM "aipet"
#endif
#define CHAMFER 9
#define CARD_X 28
#define CARD_W 304
#define HALF_W 148
#define LOG_X 40
#define LOG_LINES 22
#define OVERSCROLL_CLOSE 44
/* Keyboards as large as the circle allows, so keys stay a fingertip wide. */
#define TEXT_KEYBOARD_W 280
#define TEXT_KEYBOARD_H 168
#define HIDDEN_KEYBOARD_W 270
#define HIDDEN_KEYBOARD_H 140
#define CODE_KEYBOARD_W 264
#define CODE_KEYBOARD_H 168
static atomic_uint s_ui_last_tick;
static atomic_bool s_ui_visible_live;

/* Seven/eight columns keep ordinary keys around a fingertip wide. The LVGL
 * default packs up to thirteen controls into a row; making only its rectangle
 * taller leaves those keys frustratingly narrow on this display. */
#define KEY 1
#define MODE (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 1)
/* Checked only so it is drawn in inverse video. */
#define ENTER (MODE | LV_BUTTONMATRIX_CTRL_CHECKED)
static const char keyboard_mode_lower[] = "abc";
static const char keyboard_mode_upper[] = "ABC";
static const char keyboard_mode_special[] = "1#";
/* Caps the menu face can draw; key_pressed() gives them their meaning. */
static const char key_delete[] = "del";
static const char key_space[] = "spc";
static const char key_escape[] = "esc";
static const char key_enter[] = "ok";
static const char *const keyboard_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "\n",
    "o", "p", "a", "s", "d", "f", "g", "h", "\n",
    "j", "k", "l", "z", "x", "c", "v", "b", "\n",
    "n", "m", keyboard_mode_special,
    keyboard_mode_upper, key_delete, key_space,
    key_escape, key_enter, ""
};
static const char *const keyboard_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "\n",
    "O", "P", "A", "S", "D", "F", "G", "H", "\n",
    "J", "K", "L", "Z", "X", "C", "V", "B", "\n",
    "N", "M", keyboard_mode_special,
    keyboard_mode_lower, key_delete, key_space,
    key_escape, key_enter, ""
};
static const char *const keyboard_special[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "\n",
    "9", "0", "!", "\"", "#", "$", "%", "&", "\n",
    "'", "(", ")", "*", "+", ",", "-", ".", "\n",
    "/", ":", ";", "<", "=", ">", "?", "@", "\n",
    "[", "\\", "]", "^", "_", "`", "{", "|", "\n",
    "}", "~", keyboard_mode_lower,
    keyboard_mode_upper, key_delete, key_space,
    key_escape, key_enter, ""
};
static const char *const keyboard_code[] = {
    "A", "B", "C", "D", "E", "F", "G", "\n",
    "H", "J", "K", "L", "M", "N", "P", "\n",
    "Q", "R", "S", "T", "U", "V", "W", "\n",
    "X", "Y", "Z", "2", "3", "4", "5", "\n",
    "6", "7", "8", "9", key_delete, key_escape,
    key_enter, ""
};
static const lv_buttonmatrix_ctrl_t keyboard_text_ctrl[32] = {
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, MODE, MODE, MODE, KEY, MODE, ENTER
};
static const lv_buttonmatrix_ctrl_t keyboard_special_ctrl[48] = {
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, MODE, MODE, MODE, KEY, MODE, ENTER
};
static const lv_buttonmatrix_ctrl_t keyboard_code_ctrl[35] = {
    KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, KEY, KEY, KEY,
    KEY, KEY, KEY, KEY, MODE, MODE, ENTER
};
#undef ENTER
#undef MODE
#undef KEY
typedef enum {
    PAGE_BOOT,
    PAGE_WIFI,
    PAGE_PASSWORD,
    PAGE_CODE,
    PAGE_HOME,
    PAGE_LIBRARY,
    PAGE_CONFIRM,
    PAGE_RESTART_CONFIRM,
    PAGE_ABOUT,
    PAGE_LINK,
} page_t;
typedef enum { CARD_PLAIN = 0, CARD_PRESS = 1, CARD_FILLED = 2, CARD_BUTTON = 4 } card_style_t;
typedef enum { SETTING_VOLUME, SETTING_BRIGHTNESS } setting_t;
typedef struct { bool scan; char ssid[PET_SSID_MAX]; char password[PET_PASSWORD_MAX]; } command_t;
static QueueHandle_t s_commands;
static lv_obj_t *s_display_screen;
static int32_t s_width, s_height;
static lv_obj_t *s_screen, *s_status_label, *s_status_cursor, *s_network_list, *s_ssid, *s_password, *s_code;
static lv_obj_t *s_keyboard, *s_password_reveal, *s_code_cell[4];
/* The deck: its scrolling body, and the values the status snapshot keeps current. */
static lv_obj_t *s_menu, *s_progress, *s_wifi_icon, *s_wifi_name, *s_wifi_state, *s_battery_icon, *s_battery_value;
static lv_obj_t *s_voice_value, *s_pets_label, *s_pets_value, *s_log[LOG_LINES];
static lv_obj_t *s_slider[2], *s_slider_value[2];
static lv_obj_t *s_cursor, *s_bezel, *s_tube[3];
static lv_timer_t *s_type_timer, *s_tick_timer, *s_status_timer, *s_setting_timer, *s_splash_timer, *s_leave_timer;
static uint32_t s_leave_tick;
static char s_typing[40];
static uint8_t s_typing_length, s_typing_shown;
static const char *s_hint;   /* the status line's resting text; NULL on the deck */
static bool s_message;       /* a message stands on the status line */
static bool s_animate;       /* this page prints itself in */
static unsigned s_reveal_order;
static bool s_leaving;
static uint32_t s_boot_tick;
static bool s_status_known;
static bool s_scanning;
static uint8_t s_ticks;
/* Volume and brightness: shown, heard at once, kept once settled. */
static uint8_t s_setting[2], s_volume_before_mute;
static bool s_setting_unsaved[2];
static pet_wifi_network_t s_networks[PET_WIFI_SCAN_MAX];
static size_t s_network_count;
static page_t s_page = PAGE_BOOT;
static pet_enrollment_result_t s_status = PET_ENROLL_RETRY;
static bool s_setup_available;
static bool s_last_online;
static char s_selected_ssid[PET_SSID_MAX];
static bool s_hidden;
static bool s_password_visible;
static pet_onboarding_control_t s_control;
static pet_control_library_t s_library;
static lv_obj_t *s_library_dropdown;
static size_t s_selected_pet;
static pet_onboarding_status_t s_device_status;
/* The deck is home once the device is paired or has a pet; pairing itself
 * stays reachable until the cloud knows the device. */
static bool s_paired, s_enrolled;
/* The setup worker's last word on the link code (pet_setup.h). */
static pet_link_t s_link;

static void show(page_t page);
static void show_page(page_t page, bool animate);
static void swipe_up(void);
static void render_status(void);

#if CONFIG_PET_VNEXT_DEVELOPER_LIBRARY
static void center_pet_list(void *unused)
{
    (void)unused;
    if(!s_library_dropdown||!lv_dropdown_is_open(s_library_dropdown))return;
    lv_obj_t *list=lv_dropdown_get_list(s_library_dropdown);
    lv_obj_set_width(list,240);lv_obj_set_style_max_height(list,180,0);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_obj_center(list);
    lv_obj_set_style_bg_color(list,lv_color_hex(CARD),0);lv_obj_set_style_border_color(list,lv_color_hex(EDGE),0);
    lv_obj_set_style_text_color(list,lv_color_hex(LIME),0);lv_obj_set_style_text_font(list,FONT_TEXT,0);
    lv_obj_set_style_bg_color(list,lv_color_hex(LIME),LV_PART_SELECTED|LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list,lv_color_hex(DARK),LV_PART_SELECTED|LV_STATE_CHECKED);
}
static void pet_list_opened(lv_event_t *event)
{(void)event;lv_async_call(center_pet_list,NULL);}
#endif

/* Printable ASCII the menu face can draw, optionally in capitals: controls
 * become spaces and each other character one '?'. At most `max_chars`. */
static void display_text(char *out, size_t capacity, const char *in, size_t max_chars, bool capitals)
{
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && length + 1 < capacity && length < max_chars; ++p) {
        if ((*p & 0xc0) == 0x80) continue;
        char c = *p >= 0x80 ? '?' : *p < 0x20 || *p == 0x7f ? ' ' : (char)*p;
        /* Typographic dashes and quotes (U+2013, U+2014, U+2018 to U+201D),
         * as in an account name with a curly apostrophe, read as ASCII. */
        if (p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x93 && p[2] <= 0x94) || (p[2] >= 0x98 && p[2] <= 0x9d)))
            c = p[2] <= 0x94 ? '-' : p[2] <= 0x9b ? '\'' : '"';
        out[length++] = capitals && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
    out[length] = 0;
}

static int32_t text_width(const lv_font_t *font, const char *text, int32_t spacing)
{
    int32_t width = 0;
    for (const char *p = text; *p; ++p)
        width += lv_font_get_glyph_width(font, (uint8_t)p[0], (uint8_t)p[1]) + spacing;
    return width > 0 ? width - spacing : 0;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* Page geometry is expressed in the original 360px layout. Expand controls
 * on larger glass, but keep the scrolling deck's row heights and text native
 * so a taller screen shows more rows. Nested card contents use native pixels. */
static int32_t page_x(int32_t value)
{
    return value * s_width / 360;
}

static int32_t page_y(int32_t value)
{
    return value * s_height / 360;
}

static void page_box(lv_obj_t *obj, int x, int y, int width, int height)
{
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (parent == s_screen || parent == s_menu)
    {
        width = page_x(x + width) - page_x(x);
        x = page_x(x);
        if (parent == s_screen)
        {
            height = page_y(y + height) - page_y(y);
            y = page_y(y);
        }
    }
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
}

static void fade(void *object, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)object, (lv_opa_t)value, 0);
}

/* A page prints itself in: each part fades up a moment after the last. */
static void reveal_at(lv_obj_t *object, uint32_t delay)
{
    if (!s_animate) return;
    lv_obj_set_style_opa(object, LV_OPA_TRANSP, 0);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_values(&animation, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&animation, 110);
    lv_anim_set_delay(&animation, delay);
    lv_anim_set_exec_cb(&animation, fade);
    lv_anim_start(&animation);
}
static void reveal(lv_obj_t *object)
{
    reveal_at(object, 180 + 45 * s_reveal_order++);
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, const char *content, int x, int y,
                      uint32_t color, int32_t spacing)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_letter_space(obj, spacing, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, content);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

/* A line centred in `width` across the screen. */
static lv_obj_t *centered(lv_obj_t *parent, const lv_font_t *font, const char *content, int y, int width,
                          uint32_t color, int32_t spacing)
{
    lv_obj_t *obj = text(parent, font, content, 0, 0, color, spacing);
    /* Widths are the 360px layout's; a smaller screen is the limit. */
    lv_obj_set_width(obj, LV_MIN(width, s_width));
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, parent == s_screen ? page_y(y) : y);
    return obj;
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_image_dsc_t *image, int x, int y, uint32_t color)
{
    lv_obj_t *obj = lv_image_create(parent);
    lv_image_set_src(obj, image);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_image_recolor(obj, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static void set_text(lv_obj_t *label_obj, const char *content, uint32_t color)
{
    if (!label_obj) return;
    if (strcmp(lv_label_get_text(label_obj), content)) lv_label_set_text(label_obj, content);
    lv_color_t shade = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(label_obj, LV_PART_MAIN), shade))
        lv_obj_set_style_text_color(label_obj, shade, 0);
}

static void set_icon_color(lv_obj_t *image, uint32_t color)
{
    lv_color_t shade = lv_color_hex(color);
    if (image && !lv_color_eq(lv_obj_get_style_image_recolor(image, LV_PART_MAIN), shade))
        lv_obj_set_style_image_recolor(image, shade, 0);
}

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (!obj || visible != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void triangle(lv_layer_t *layer, uint32_t color, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                     int32_t cx, int32_t cy)
{
    lv_draw_triangle_dsc_t shape;
    lv_draw_triangle_dsc_init(&shape);
    shape.p[0].x = ax;
    shape.p[0].y = ay;
    shape.p[1].x = bx;
    shape.p[1].y = by;
    shape.p[2].x = cx;
    shape.p[2].y = cy;
    shape.color = lv_color_hex(color);
    shape.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &shape);
}

static void edge(lv_layer_t *layer, uint32_t color, int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.p1.x = ax;
    line.p1.y = ay;
    line.p2.x = bx;
    line.p2.y = by;
    line.color = lv_color_hex(color);
    line.width = 1;
    lv_draw_line(layer, &line);
}

/* A card: its top-left and bottom-right corners cut away, a thin rim, and
 * marks in the other two corners. Pressed, a button lights up lime. */
static void draw_card(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    unsigned style = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    bool pressed = (style & (CARD_PRESS | CARD_BUTTON)) && lv_obj_has_state(obj, LV_STATE_PRESSED);
    bool lit = (style & CARD_FILLED) || (pressed && (style & CARD_BUTTON));
    const int32_t c = CHAMFER;
    lv_draw_rect_dsc_t face;
    lv_draw_rect_dsc_init(&face);
    face.bg_color = lv_color_hex(lit ? LIME : CARD);
    lv_draw_rect(layer, &face, &a);
    triangle(layer, BG, a.x1, a.y1, a.x1 + c, a.y1, a.x1, a.y1 + c);
    triangle(layer, BG, a.x2 + 1, a.y2 + 1, a.x2 + 1 - c, a.y2 + 1, a.x2 + 1, a.y2 + 1 - c);
    uint32_t rim = lit || pressed ? LIME : EDGE;
    edge(layer, rim, a.x1 + c, a.y1, a.x2, a.y1);
    edge(layer, rim, a.x2, a.y1, a.x2, a.y2 - c);
    edge(layer, rim, a.x2, a.y2 - c, a.x2 - c, a.y2);
    edge(layer, rim, a.x2 - c, a.y2, a.x1, a.y2);
    edge(layer, rim, a.x1, a.y2, a.x1, a.y1 + c);
    edge(layer, rim, a.x1, a.y1 + c, a.x1 + c, a.y1);
    if (lit) return;
    triangle(layer, rim, a.x2 - 6, a.y1, a.x2 + 1, a.y1, a.x2 + 1, a.y1 + 7);
    triangle(layer, rim, a.x1, a.y2 - 6, a.x1, a.y2 + 1, a.x1 + 7, a.y2 + 1);
}

/* A button's text and icon turn dark while its face is lit. */
static void button_state(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    lv_color_t shade = lv_color_hex(lv_event_get_code(event) == LV_EVENT_PRESSED ? DARK : LIME);
    for (uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_t *child = lv_obj_get_child(button, (int32_t)i);
        if (lv_obj_check_type(child, &lv_image_class)) lv_obj_set_style_image_recolor(child, shade, 0);
        else lv_obj_set_style_text_color(child, shade, 0);
    }
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int width, int height, card_style_t style)
{
    lv_obj_t *obj = plain(parent);
    page_box(obj, x, y, width, height);
    lv_obj_add_event_cb(obj, draw_card, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)style);
    if (style & (CARD_PRESS | CARD_BUTTON)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        /* A press nudges the card down: also what redraws its face. */
        lv_obj_set_style_translate_y(obj, 1, LV_STATE_PRESSED);
    }
    if (style & CARD_BUTTON) {
        lv_obj_add_event_cb(obj, button_state, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(obj, button_state, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(obj, button_state, LV_EVENT_PRESS_LOST, NULL);
    }
    reveal(obj);
    return obj;
}

/* A command button: an optional icon and a word, centred. */
static lv_obj_t *command(lv_obj_t *parent, const char *name, const lv_image_dsc_t *image, int x, int y,
                         int width, int height, lv_event_cb_t callback)
{
    lv_obj_t *button = card(parent, x, y, width, height, CARD_BUTTON);
    width = lv_obj_get_style_width(button, 0);
    height = lv_obj_get_style_height(button, 0);
    int32_t words = text_width(FONT_TEXT, name, 2);
    int32_t total = words + (image ? (int32_t)image->header.w + 8 : 0);
    int32_t left = (width - total) / 2;
    if (image) icon(button, image, left, (height - (int32_t)image->header.h) / 2, LIME);
    lv_obj_t *caption = text(button, FONT_TEXT, name, left + total - words, (height - 19) / 2, LIME, 2);
    (void)caption;
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    return button;
}

/* A bar of cells, lit up to its value: framed for the sliders. */
typedef struct { uint8_t count; bool framed; bool battery; } segments_t;
static const segments_t s_slider_segments = {10, true, false}, s_progress_segments = {20, false, false};
static void draw_segments(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    const segments_t *look = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    bool slider = lv_obj_check_type(obj, &lv_slider_class);
    int32_t value = slider ? lv_slider_get_value(obj) : lv_bar_get_value(obj);
    int32_t minimum = slider ? lv_slider_get_min_value(obj) : lv_bar_get_min_value(obj);
    int32_t maximum = slider ? lv_slider_get_max_value(obj) : lv_bar_get_max_value(obj);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    if (look->framed) {
        lv_draw_rect_dsc_t frame;
        lv_draw_rect_dsc_init(&frame);
        frame.bg_opa = LV_OPA_TRANSP;
        frame.border_color = lv_color_hex(EDGE);
        frame.border_width = 1;
        frame.radius = 3;
        lv_draw_rect(layer, &frame, &box);
        lv_area_increase(&box, -4, -4);
    }
    int32_t span = lv_area_get_width(&box), gap = look->framed ? 3 : 2;
    int32_t lit = maximum > minimum ? ((value - minimum) * look->count + (maximum - minimum) / 2) / (maximum - minimum) : 0;
    lv_draw_rect_dsc_t cell;
    lv_draw_rect_dsc_init(&cell);
    cell.radius = 1;
    for (int32_t i = 0; i < look->count; ++i) {
        lv_area_t area = box;
        area.x1 = box.x1 + i * (span + gap) / look->count;
        area.x2 = box.x1 + (i + 1) * (span + gap) / look->count - gap - 1;
        cell.bg_color = lv_color_hex(i < lit ? LIME : UNLIT);
        lv_draw_rect(layer, &cell, &area);
    }
}

/* The battery: a lit cell that empties, amber then red when low. */
static void draw_battery(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const pet_onboarding_status_t *s = &s_device_status;
    unsigned percent = s->battery_percent > 100 ? 100 : s->battery_percent;
    uint32_t color = !s->battery_valid ? DIM : percent <= 15 ? RED : percent <= 25 ? AMBER : LIME;
    lv_draw_rect_dsc_t shape;
    lv_draw_rect_dsc_init(&shape);
    lv_area_t body = a, nub = a, fill;
    body.x2 -= 4;
    nub.x1 = body.x2 + 1;
    nub.y1 += 5;
    nub.y2 -= 5;
    shape.bg_color = lv_color_hex(color);
    shape.radius = 1;
    lv_draw_rect(layer, &shape, &nub);
    shape.bg_opa = LV_OPA_TRANSP;
    shape.border_color = lv_color_hex(color);
    shape.border_width = 2;
    shape.radius = 3;
    lv_draw_rect(layer, &shape, &body);
    if (!s->battery_valid) return;
    fill = body;
    lv_area_increase(&fill, -4, -4);
    int32_t width = lv_area_get_width(&fill) * (int32_t)percent / 100;
    if (width < 1) return;
    fill.x2 = fill.x1 + width - 1;
    lv_draw_rect_dsc_init(&shape);
    shape.bg_color = lv_color_hex(color);
    lv_draw_rect(layer, &shape, &fill);
}

/* Signal strength as four rising bars. */
static void draw_signal(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    unsigned level = (unsigned)(uintptr_t)lv_obj_get_user_data(obj);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    lv_draw_rect_dsc_t bar_look;
    lv_draw_rect_dsc_init(&bar_look);
    for (unsigned i = 0; i < 4; ++i) {
        int32_t height = 5 + 4 * (int32_t)i;
        lv_area_t bar = {box.x1 + 6 * (int32_t)i, box.y2 - height + 1, box.x1 + 6 * (int32_t)i + 3, box.y2};
        bar_look.bg_color = lv_color_hex(i < level ? LIME : UNLIT);
        lv_draw_rect(lv_event_get_layer(event), &bar_look, &bar);
    }
}

/* The status line under a page's title: centred, typing itself in, with a
 * block cursor. Messages stand a few seconds, then the resting text returns. */
static void place_status(const char *content)
{
    int32_t width = text_width(FONT_SMALL, s_typing, 2) + 10;
    int32_t left = (s_width - width) / 2;
    lv_obj_set_x(s_status_label, left);
    if (s_status_cursor) lv_obj_set_x(s_status_cursor, left + text_width(FONT_SMALL, content, 2) + 4);
}

static void type_status(const char *content, uint32_t color, bool typed)
{
    if (!s_status_label) return;
    if (!typed && !strcmp(content, s_typing)) {
        /* The same line, still typing or already there: only its colour. */
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
        if (s_status_cursor) lv_obj_set_style_bg_color(s_status_cursor, lv_color_hex(color), 0);
        return;
    }
    strlcpy(s_typing, content, sizeof(s_typing));
    s_typing_length = (uint8_t)strlen(s_typing);
    s_typing_shown = typed ? 0 : s_typing_length;
    char shown[sizeof(s_typing)];
    memcpy(shown, s_typing, s_typing_shown);
    shown[s_typing_shown] = 0;
    set_text(s_status_label, shown, color);
    if (s_status_cursor) lv_obj_set_style_bg_color(s_status_cursor, lv_color_hex(color), 0);
    place_status(shown);
    if (typed) {
        lv_timer_reset(s_type_timer);
        lv_timer_resume(s_type_timer);
    }
}

static void type_next(lv_timer_t *timer)
{
    if (!s_status_label || s_typing_shown >= s_typing_length) {
        lv_timer_pause(timer);
        return;
    }
    char shown[sizeof(s_typing)];
    memcpy(shown, s_typing, ++s_typing_shown);
    shown[s_typing_shown] = 0;
    lv_label_set_text(s_status_label, shown);
    place_status(shown);
}

/* The deck's own line: what matters most right now, in the order the owner
 * can act on it. */
static const char *deck_summary(char *out, size_t capacity, uint32_t *color)
{
    const pet_onboarding_status_t *s = &s_device_status;
    *color = LIME;
    switch (s->installation) {
        case PET_ONBOARDING_INSTALL_REQUESTED: return "PET INSTALL QUEUED";
        case PET_ONBOARDING_INSTALL_DOWNLOADING:
            snprintf(out, capacity, "RECEIVING PET %u%%", s->installation_percent);
            return out;
        case PET_ONBOARDING_INSTALL_VERIFYING: return "VERIFYING NEW PET";
        case PET_ONBOARDING_INSTALL_ACTIVATING: return "WAKING NEW PET";
        default: break;
    }
    if (s->installation == PET_ONBOARDING_INSTALL_FAILED || s->cloud == PET_ONBOARDING_CLOUD_ATTENTION) {
        *color = RED;
        return "NEEDS ATTENTION";
    }
    *color = AMBER;
    if (s->wifi != PET_ONBOARDING_WIFI_CONNECTED) return s->ssid[0] ? "WIFI OFFLINE" : "NO WIFI SET UP";
    if (s->cloud == PET_ONBOARDING_CLOUD_PAIRING) return "NOT PAIRED";
    if (s->retry_seconds) {
        snprintf(out, capacity, "RETRY IN %uS", s->retry_seconds);
        return out;
    }
    *color = LIME;
    return "ALL SYSTEMS NOMINAL";
}

static void status_rest(lv_timer_t *timer)
{
    if (timer) lv_timer_pause(timer);
    s_message = false;
    if (!s_status_label) return;
    if (s_hint) type_status(s_hint, LABEL, false);
    else {
        char summary[40];
        uint32_t color;
        const char *line = deck_summary(summary, sizeof(summary), &color);
        type_status(line, color, false);
    }
}

static void status_text(const char *message)
{
    if (!s_status_label) return;
    s_message = true;
    type_status(message, LIME, false);
    lv_timer_reset(s_status_timer);
    lv_timer_resume(s_status_timer);
}

static void status_line(lv_obj_t *parent, int y, const char *hint)
{
    if (parent == s_screen) y = page_y(y);
    s_hint = hint;
    s_status_label = text(parent, FONT_SMALL, "", 0, y, LABEL, 2);
    s_status_cursor = plain(parent);
    lv_obj_set_size(s_status_cursor, 6, 11);
    lv_obj_set_y(s_status_cursor, y + 1);
    lv_obj_set_style_bg_opa(s_status_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_status_cursor, lv_color_hex(LABEL), 0);
    reveal(s_status_label);
    if (hint) type_status(hint, LABEL, s_animate);
    else {
        char summary[40];
        uint32_t color;
        const char *line = deck_summary(summary, sizeof(summary), &color);
        type_status(line, color, s_animate);
    }
}

/* The page's title on a lit tab, three slashes after it. */
static void title(lv_obj_t *parent, const char *name, int y)
{
    int32_t words = text_width(FONT_TITLE, name, 3);
    int32_t width = words + 30, total = width + 8 + 3 * 7;
    int32_t left = (s_width - total) / 2;
    /* The small screen's shorter tab clears the status line below it. */
    int32_t height = SMALL_SCREEN ? 22 : 28;
    lv_obj_t *tab = card(parent, left, y, width, height, CARD_FILLED);
    lv_obj_set_x(tab, left);
    lv_obj_set_size(tab, width, height);
    text(tab, FONT_TITLE, name, 15, SMALL_SCREEN ? 2 : 3, DARK, 3);
    reveal(text(parent, FONT_SMALL, "///", left + width + 8,
                (parent == s_screen ? page_y(y) : y) + (SMALL_SCREEN ? 6 : 9), LIME, 1));
}

/* The chevron at the foot of a page: swiping up from it, or tapping it,
 * goes back. */
static void handle_clicked(lv_event_t *event)
{
    (void)event;
    swipe_up();
}
static void handle(void)
{
    lv_obj_t *zone = plain(s_screen);
    lv_obj_set_pos(zone, 0, s_height - 56);
    lv_obj_set_size(zone, s_width, 56);
    lv_obj_set_style_bg_opa(zone, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(zone, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_grad_color(zone, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_grad_dir(zone, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(zone, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_stop(zone, 0, 0);
    lv_obj_set_style_bg_grad_stop(zone, 110, 0);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, handle_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mark = icon(zone, &pet_icon_chevron_up,
                          (s_width - (int32_t)pet_icon_chevron_up.header.w) / 2, 22, LIME);
    reveal(mark);
}

/* The tick: the cursors blink, a scan spins, and the log's clock runs. */
static void tick(lv_timer_t *timer)
{
    (void)timer;
    static const char spinner[] = "|/-\\";
    ++s_ticks;
    if (s_scanning && s_page == PAGE_WIFI && !s_message && s_status_label) {
        char line[24];
        snprintf(line, sizeof(line), "SCANNING %c", spinner[s_ticks % 4]);
        type_status(line, LIME, false);
    }
    if (s_ticks % 4) return;
    bool on = (s_ticks / 4) % 2;
    set_visible(s_cursor, on);
    set_visible(s_status_cursor, on || s_typing_shown < s_typing_length);
    if (s_page == PAGE_HOME && !(s_ticks % 8)) render_status();
}

/* Heard or seen at once while the owner chooses; kept once they settle. */
static void setting_show(setting_t which)
{
    if (!s_slider[which]) return;
    if (lv_slider_get_value(s_slider[which]) != s_setting[which])
        lv_slider_set_value(s_slider[which], s_setting[which], LV_ANIM_OFF);
    char line[8];
    if (which == SETTING_VOLUME && !s_setting[which]) strcpy(line, "OFF");
    else snprintf(line, sizeof(line), "%u%%", s_setting[which]);
    set_text(s_slider_value[which], line, which == SETTING_VOLUME && !s_setting[which] ? AMBER : LIME);
}

static void setting_set(setting_t which, int32_t level)
{
    int32_t floor = which == SETTING_BRIGHTNESS ? PET_BRIGHTNESS_MIN : 0;
    level = level < floor ? floor : level > 100 ? 100 : level;
    if ((uint8_t)level != s_setting[which]) {
        s_setting[which] = (uint8_t)level;
        bool (*apply)(uint8_t, bool) = which == SETTING_VOLUME ? s_control.volume : s_control.brightness;
        if (apply) apply(s_setting[which], false);
        s_setting_unsaved[which] = true;
        lv_timer_reset(s_setting_timer);
        lv_timer_resume(s_setting_timer);
    }
    setting_show(which);
}

static void settings_keep(lv_timer_t *timer)
{
    if (timer) lv_timer_pause(timer);
    bool retry = false;
    for (unsigned which = 0; which < 2; ++which) {
        if (!s_setting_unsaved[which]) continue;
        bool (*apply)(uint8_t, bool) = which == SETTING_VOLUME ? s_control.volume : s_control.brightness;
        /* A full queue keeps the level unsaved; it is tried again shortly. */
        if (!apply || apply(s_setting[which], true)) s_setting_unsaved[which] = false;
        else retry = true;
    }
    if (retry) {
        lv_timer_reset(s_setting_timer);
        lv_timer_resume(s_setting_timer);
    }
}

static void slider_moved(lv_event_t *event)
{
    setting_t which = (setting_t)(uintptr_t)lv_event_get_user_data(event);
    setting_set(which, (lv_slider_get_value(lv_event_get_current_target_obj(event)) + 5) / 10 * 10);
}

/* Tapping the speaker mutes, and tapping it again restores the level. */
static void mute_clicked(lv_event_t *event)
{
    (void)event;
    if (s_setting[SETTING_VOLUME]) {
        s_volume_before_mute = s_setting[SETTING_VOLUME];
        setting_set(SETTING_VOLUME, 0);
    } else setting_set(SETTING_VOLUME, s_volume_before_mute ? s_volume_before_mute : 50);
}

/* VOLUME / BRIGHTNESS: an icon, the name, ten cells to drag, the level. */
static void setting_card(setting_t which, int y)
{
    lv_obj_t *face = card(s_menu, CARD_X, y, CARD_W, 58, CARD_PLAIN);
    const lv_image_dsc_t *image = which == SETTING_VOLUME ? &pet_icon_speaker : &pet_icon_sun;
    icon(face, image, 12 + (26 - (int32_t)image->header.w) / 2, (58 - (int32_t)image->header.h) / 2, LIME);
    if (which == SETTING_VOLUME) {
        lv_obj_t *mute = plain(face);
        lv_obj_set_size(mute, 46, 58);
        lv_obj_add_flag(mute, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mute, mute_clicked, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *rule = plain(face);
    lv_obj_set_pos(rule, 48, 9);
    lv_obj_set_size(rule, 1, 40);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(EDGE), 0);
    text(face, FONT_SMALL, which == SETTING_VOLUME ? "VOLUME" : "BRIGHTNESS", 58, 8, LABEL, 2);
    lv_obj_t *slider = lv_slider_create(face);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, 56, 25);
    int32_t extra_width = lv_obj_get_style_width(face, 0) - CARD_W;
    lv_obj_set_size(slider, 176 + extra_width, 24);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_ext_click_area(slider, 8);
    lv_obj_add_event_cb(slider, draw_segments, LV_EVENT_DRAW_MAIN, (void *)&s_slider_segments);
    lv_obj_add_event_cb(slider, slider_moved, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)which);
    s_slider[which] = slider;
    s_slider_value[which] = text(face, FONT_BIG, "", 240 + extra_width, 13, LIME, 0);
    setting_show(which);
}

/* The system log below the deck: real values, as a terminal session. */
static void render_log(const pet_onboarding_status_t *s)
{
    if (!s_log[0]) return;
    char line[64], name[20], version[20], field[40];
    unsigned n = 0;
#define LOG(color, ...) do { snprintf(line, sizeof(line), __VA_ARGS__); set_text(s_log[n++], line, color); } while (0)
    const char *firmware = s->firmware_version;
    if (!strncmp(firmware, "aipet-", 6)) firmware += 6;
    display_text(field, sizeof(field), firmware, 24, false);
    LOG(DIM, "$ uname -a");
    LOG(LIME, SYSTEM " %s esp32s3", field[0] ? field : "-");
    display_text(name, sizeof(name), s->pack_name, 12, true);
    display_text(version, sizeof(version), s->pack_version, 10, false);
    display_text(field, sizeof(field), s->build_id, 8, false);
    LOG(DIM, "$ cat /etc/pet");
    if (s->has_pet && name[0]) LOG(LIME, "%.12s v%.10s  build %.8s", name, version, field[0] ? field : "-");
    else LOG(AMBER, "no pet yet");
    LOG(DIM, "$ ls /pets");
    if (s->pet_capacity) LOG(LIME, "%u of %u slots used, #%u on screen", s->pet_count, s->pet_capacity, s->pet_index);
    else if (s->single_pet_slot) LOG(LIME, "1 slot %luK, no backup", (unsigned long)((s->slots[0].bytes + 1023) / 1024));
    else LOG(LIME, "A %luK  B %luK", (unsigned long)((s->slots[0].bytes + 1023) / 1024),
             (unsigned long)((s->slots[1].bytes + 1023) / 1024));
    display_text(field, sizeof(field), s->ssid, 20, false);
    LOG(DIM, "$ wifi");
    LOG(s->wifi == PET_ONBOARDING_WIFI_CONNECTED ? LIME : AMBER, "%s%s%s", field[0] ? field : "not set up",
        field[0] ? ": " : "", !field[0] ? "" : s->wifi == PET_ONBOARDING_WIFI_CONNECTED ? "connected" :
        s->wifi == PET_ONBOARDING_WIFI_CONNECTING ? "joining" : "offline");
    static const char *const cloud[] = {"needs pairing", "offline", "syncing", "online", "needs help"};
    LOG(DIM, "$ cloud");
    LOG(s->cloud == PET_ONBOARDING_CLOUD_ATTENTION ? RED : s->cloud >= PET_ONBOARDING_CLOUD_SYNCING ? LIME : AMBER,
        "%s", cloud[s->cloud]);
    static const char *const voice[] = {"unavailable", "ready: tap to talk", "listening", "thinking", "speaking", "error"};
    LOG(DIM, "$ voice");
    if (s->conversation != PET_ONBOARDING_CONVERSATION_UNAVAILABLE)
        LOG(s->conversation == PET_ONBOARDING_CONVERSATION_ERROR ? RED : LIME, "%s", voice[s->conversation]);
    else LOG(AMBER, "%s", !s->has_pet ? "no pet" : s->wifi != PET_ONBOARDING_WIFI_CONNECTED ? "no wifi" :
             s->cloud == PET_ONBOARDING_CLOUD_PAIRING ? "not paired" : !s->pet_linked ? "not linked to aipets.com" :
             "connecting");
    static const char *const sync[] = {"idle", "queued", "downloading", "verifying", "activating", "ready", "failed"};
    LOG(DIM, "$ sync");
    if (s->installation >= PET_ONBOARDING_INSTALL_REQUESTED && s->installation <= PET_ONBOARDING_INSTALL_ACTIVATING)
        LOG(LIME, "%s %u%%", sync[s->installation], s->installation_percent);
    else LOG(s->installation == PET_ONBOARDING_INSTALL_FAILED ? RED : LIME, "%s%s", sync[s->installation],
             s->retry_seconds ? ", retry soon" : "");
    display_text(field, sizeof(field), s->support_error_code, 32, false);
    if (field[0]) LOG(RED, "error %s", field);
    else if (s->retry_seconds) LOG(AMBER, "retry in %us", s->retry_seconds);
    else LOG(DIM, "no errors");
    uint32_t seconds = lv_tick_get() / 1000;
    LOG(DIM, "$ uptime");
    LOG(LIME, "up %lu:%02lu:%02lu", (unsigned long)(seconds / 3600), (unsigned long)(seconds / 60 % 60),
        (unsigned long)(seconds % 60));
    LOG(DIM, "$ credits");
    LOG(DIM, "menu font derived from Share Tech Mono");
    LOG(DIM, "(c) 2012 Carrois Type Design, Ralph du Carrois");
    LOG(DIM, "SIL OFL 1.1: see ABOUT below");
    LOG(DIM, "$");
#undef LOG
}

static void render_home(const pet_onboarding_status_t *s)
{
    char line[40];
    char ssid[20];
    display_text(ssid, sizeof(ssid), s->ssid, 16, false);
    bool connected = s->wifi == PET_ONBOARDING_WIFI_CONNECTED;
    set_text(s_wifi_name, ssid[0] ? ssid : "NOT SET UP", ssid[0] ? LIME : AMBER);
    set_text(s_wifi_state, connected ? "CONNECTED" : s->wifi == PET_ONBOARDING_WIFI_CONNECTING ? "JOINING" : "OFFLINE",
             connected ? DIM : AMBER);
    set_icon_color(s_wifi_icon, connected ? LIME : AMBER);

    if (s->battery_valid) {
        unsigned percent = s->battery_percent > 100 ? 100 : s->battery_percent;
        snprintf(line, sizeof(line), "%u%%", percent);
        set_text(s_battery_value, line, percent <= 15 ? RED : percent <= 25 ? AMBER : LIME);
    } else set_text(s_battery_value, "--", DIM);
    if (s_battery_icon) lv_obj_invalidate(s_battery_icon);

    static const char *const voice[] = {"", "READY", "LISTENING", "THINKING", "SPEAKING", "ERROR"};
    if (s->conversation != PET_ONBOARDING_CONVERSATION_UNAVAILABLE)
        set_text(s_voice_value, voice[s->conversation], s->conversation == PET_ONBOARDING_CONVERSATION_ERROR ? RED : LIME);
    else set_text(s_voice_value, !s->has_pet ? "NO PET" : !connected ? "NO WIFI" :
                  s->cloud == PET_ONBOARDING_CLOUD_PAIRING ? "NOT PAIRED" : !s->pet_linked ? "NOT LINKED" :
                  "CONNECTING", AMBER);

    char name[16];
    display_text(name, sizeof(name), s->pack_name, 9, true);
    set_text(s_pets_value, s->has_pet && name[0] ? name : "NONE", s->has_pet ? LIME : DIM);
    if (s->pet_capacity && s->pet_index) snprintf(line, sizeof(line), "PET %u/%u", s->pet_index, s->pet_count);
    else strcpy(line, "PET");
    set_text(s_pets_label, line, LABEL);

    bool download = s->installation >= PET_ONBOARDING_INSTALL_REQUESTED &&
        s->installation <= PET_ONBOARDING_INSTALL_ACTIVATING;
    set_visible(s_progress, download);
    if (s_progress && lv_bar_get_value(s_progress) != (int32_t)s->installation_percent)
        lv_bar_set_value(s_progress, (int32_t)s->installation_percent, LV_ANIM_OFF);
    if (!s_message) status_rest(NULL);
    render_log(s);
}

static void render_status(void)
{
    if (s_page == PAGE_HOME) render_home(&s_device_status);
}

static bool enqueue(const command_t *command)
{
    if (xQueueSend(s_commands, command, 0) == pdTRUE) return true;
    status_text("BUSY - TRY AGAIN");
    return false;
}

static void scan_clicked(lv_event_t *event)
{
    (void)event;
    command_t command = {.scan = true};
    if (enqueue(&command)) s_scanning = true;
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    show(PAGE_WIFI);
}

static void hidden_clicked(lv_event_t *event)
{
    (void)event;
    s_hidden = true;
    s_selected_ssid[0] = 0;
    show(PAGE_PASSWORD);
}

static bool join_selected_without_password(void)
{
    command_t command = {0};
    strlcpy(command.ssid, s_selected_ssid, sizeof(command.ssid));
    return enqueue(&command);
}

static void network_clicked(lv_event_t *event)
{
    size_t selected = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (selected >= s_network_count) return;
    strlcpy(s_selected_ssid, s_networks[selected].ssid,
            sizeof(s_selected_ssid));
    s_hidden = false;
    if (s_networks[selected].saved || !s_networks[selected].secured) {
        if (join_selected_without_password()) {
            status_text(s_networks[selected].saved ?
                        "SAVED KEY - RESTARTING" :
                        "OPEN NETWORK - RESTARTING");
        }
        return;
    }
    show(PAGE_PASSWORD);
}

/* LINK: the device's own code. Without an account it
 * links the device; with one it moves it to another account. */
static void link_clicked(lv_event_t *event)
{
    (void)event;
    if (!pet_network_wifi_is_ready()) { status_text("CONNECT WIFI FIRST"); return; }
    if (!s_setup_available || pet_setup_link(PET_SETUP_LINK_START) != ESP_OK) {
        status_text("CLOUD SETUP MISSING");
        return;
    }
    /* A question already waiting comes back as it was. */
    if (s_link.phase != PET_LINK_CLAIMED && s_link.phase != PET_LINK_LINKING) {
        s_link.phase = PET_LINK_ASKING;
        s_link.moving = s_enrolled;
        s_link.code[0] = s_link.error[0] = 0;
    }
    show(PAGE_LINK);
}

static void link_answered(bool yes)
{
    if (pet_setup_link(yes ? PET_SETUP_LINK_YES : PET_SETUP_LINK_NO) != ESP_OK) {
        status_text("BUSY - TRY AGAIN");
        return;
    }
    s_link.phase = yes ? PET_LINK_LINKING : PET_LINK_DECLINING;
    show(PAGE_LINK);
}
static void link_yes_clicked(lv_event_t *event) { (void)event; link_answered(true); }
static void link_no_clicked(lv_event_t *event) { (void)event; link_answered(false); }

/* Leaving the code: a moving device's code dies; a question never leaves. */
static void link_leave(page_t next)
{
    pet_setup_link(PET_SETUP_LINK_LEAVE);
    if (s_link.phase != PET_LINK_CLAIMED && s_link.phase != PET_LINK_LINKING) s_link.phase = PET_LINK_OFF;
    show(next);
}
static void link_cancel_clicked(lv_event_t *event) { (void)event; link_leave(s_paired ? PAGE_HOME : PAGE_WIFI); }
/* The typed four-character code from aipets.com: the fallback. */
static void code_clicked(lv_event_t *event)
{
    (void)event;
    if (!pet_network_wifi_is_ready()) { status_text("CONNECT WIFI FIRST"); return; }
    link_leave(s_enrolled ? PAGE_HOME : PAGE_CODE);
}

/* The field the keyboard types into shows the cursor; the other rests. */
static void focus_input(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target_obj(event);
    lv_keyboard_set_textarea(s_keyboard, target);
    lv_obj_t *fields[] = {s_ssid, s_password};
    for (unsigned i = 0; i < 2; ++i) {
        if (!fields[i]) continue;
        if (fields[i] == target) lv_obj_add_state(fields[i], LV_STATE_FOCUSED);
        else lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
    }
}

static void next_field(lv_event_t *event)
{
    (void)event;
    if (s_password) lv_obj_send_event(s_password, LV_EVENT_FOCUSED, NULL);
}

static void join_clicked(lv_event_t *event)
{
    (void)event;
    command_t command = {0};
    const char *ssid = s_hidden ? lv_textarea_get_text(s_ssid) : s_selected_ssid;
    const char *password = lv_textarea_get_text(s_password);
    /* LVGL limits characters, while Wi-Fi/NVS limits bytes. Never truncate a
     * multi-byte SSID/password into a different network or credential. */
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (!ssid_length || ssid_length >= sizeof(command.ssid)) {
        status_text("NAME: 1-32 BYTES"); return;
    }
    if (password_length >= sizeof(command.password) ||
        (password_length > 0 && password_length < 8)) {
        status_text("KEY: 8-64 CHARACTERS"); return;
    }
    if (password_length == 64 && strspn(password, "0123456789abcdefABCDEF") != 64) {
        status_text("64-CHARACTER KEYS ARE HEX"); return;
    }
    memcpy(command.ssid, ssid, ssid_length + 1);
    memcpy(command.password, password, password_length + 1);
    if (enqueue(&command)) {
        lv_textarea_set_text(s_password, "");
        status_text("SAVING WIFI - RESTARTING");
    }
    pet_enrollment_clear(&command, sizeof(command));
}

static void pair_clicked(lv_event_t *event)
{
    (void)event;
    if (!s_setup_available) { status_text("CLOUD SETUP MISSING"); return; }
    char code[PET_ENROLLMENT_CODE_BYTES] = {0};
    const char *typed = lv_textarea_get_text(s_code);
    if (strlen(typed) == PET_ENROLLMENT_CODE_LENGTH) memcpy(code, typed, PET_ENROLLMENT_CODE_LENGTH);
    esp_err_t err = pet_setup_submit(code);
    pet_enrollment_clear(code, sizeof(code));
    lv_textarea_set_text(s_code, "");
    status_text(err == ESP_OK ? "CHECKING CODE..." :
                err == ESP_ERR_INVALID_ARG ? "ENTER ALL 4 CHARACTERS" :
                "SETUP BUSY - RETRY");
}

/* The four code cells show what is typed; the next one holds the cursor. */
static void code_changed(lv_event_t *event)
{
    (void)event;
    const char *typed = s_code ? lv_textarea_get_text(s_code) : "";
    size_t length = strlen(typed);
    s_cursor = NULL;
    for (size_t i = 0; i < 4; ++i) {
        if (!s_code_cell[i]) continue;
        lv_obj_t *letter = lv_obj_get_child(s_code_cell[i], 0);
        char shown[2] = {i < length ? typed[i] : '_', 0};
        set_text(letter, shown, i < length ? LIME : DIM);
        set_visible(letter, true);
        if (i == length) s_cursor = letter;
    }
}

#if CONFIG_PET_VNEXT_DEVELOPER_LIBRARY /* The library page and its deck button. */
static void library_clicked(lv_event_t *event)
{
    (void)event;
    if(s_control.library&&s_control.library(false)){
        show(PAGE_LIBRARY);status_text("LOADING LIBRARY...");
    }
    else status_text("LIBRARY BUSY - RETRY");
}
static void next_library_clicked(lv_event_t *event)
{(void)event;if(!s_control.library||!s_control.library(true))status_text("LIBRARY BUSY - RETRY");}
static void choose_pet_clicked(lv_event_t *event)
{
    (void)event;if(!s_library_dropdown||!s_library.count)return;
    s_selected_pet=lv_dropdown_get_selected(s_library_dropdown);
    if(s_selected_pet>=s_library.count)return;
    if(!s_library.items[s_selected_pet].installable){status_text("NOT FOR THIS DEVICE");return;}
    show(PAGE_CONFIRM);
}
#endif
static void confirm_pet_clicked(lv_event_t *event)
{
    (void)event;
    if(s_selected_pet>=s_library.count||!s_control.select||!s_control.select(s_library.items[s_selected_pet].build_id)) {
        status_text("BUSY - RETRY");return;
    }
    show(PAGE_HOME);
}
static void library_back_clicked(lv_event_t *event){(void)event;show(PAGE_LIBRARY);}
static void home_clicked(lv_event_t *event){(void)event;show(PAGE_HOME);}
static void about_clicked(lv_event_t *event){(void)event;show(PAGE_ABOUT);}
static void retry_clicked(lv_event_t *event)
{
    (void)event;
    if (s_control.retry_sync && s_control.retry_sync()) {
        status_text(s_device_status.retry_seconds ?
                    "QUEUED - SERVER ASKED TO WAIT" :
                    "SYNC QUEUED");
    } else status_text("BUSY - TRY AGAIN");
}
static void restart_prompt_clicked(lv_event_t *event)
{(void)event;show(PAGE_RESTART_CONFIRM);}
static void restart_clicked(lv_event_t *event)
{
    (void)event;
    if (s_control.restart && s_control.restart()) status_text("REBOOTING...");
    else status_text("BUSY - TRY AGAIN");
}
static void password_visibility_clicked(lv_event_t *event)
{
    (void)event;
    if (!s_password) return;
    s_password_visible = !s_password_visible;
    lv_textarea_set_password_mode(s_password, !s_password_visible);
    lv_obj_t *button_obj = lv_event_get_target_obj(event);
    lv_obj_t *caption = lv_obj_get_child(button_obj, 0);
    if (caption) lv_label_set_text(caption, s_password_visible ? "HIDE" : "SHOW");
}

static lv_obj_t *input(const char *placeholder, int x, int y, int width, unsigned max_length, bool secret)
{
    lv_obj_t *obj = lv_textarea_create(s_screen);
    lv_obj_remove_style_all(obj);
    page_box(obj, x, y, width, 34);
    int32_t height = lv_obj_get_style_height(obj, 0);
    lv_textarea_set_one_line(obj, true);
    if (!pet_board_current()->round) lv_obj_set_height(obj, height);
    lv_textarea_set_max_length(obj, max_length);
    lv_textarea_set_placeholder_text(obj, placeholder);
    lv_textarea_set_password_bullet(obj, "*");
    lv_textarea_set_password_mode(obj, secret);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(obj, FONT_TEXT, 0);
    lv_obj_set_style_radius(obj, 3, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(CARD), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(EDGE), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(LIME), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(obj, 9, 0);
    lv_obj_set_style_pad_ver(obj, (height - 18) / 2, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(LIME), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    /* A blinking block cursor in inverse video, only in the focused field. */
    lv_obj_set_style_bg_color(obj, lv_color_hex(LIME), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(obj, lv_color_hex(DARK), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(obj, 530, LV_PART_CURSOR);
    lv_obj_add_event_cb(obj, focus_input, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(obj, focus_input, LV_EVENT_CLICKED, NULL);
    reveal(obj);
    return obj;
}

/* Replaces LVGL's key handling so the caps can be words the face can draw. */
static void key_pressed(lv_event_t *event)
{
    lv_obj_t *keyboard_obj = lv_event_get_current_target_obj(event);
    uint32_t id = lv_buttonmatrix_get_selected_button(keyboard_obj);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *key = lv_buttonmatrix_get_button_text(keyboard_obj, id);
    lv_obj_t *target = lv_keyboard_get_textarea(keyboard_obj);
    if (!key) return;
    if (!strcmp(key, keyboard_mode_lower)) lv_keyboard_set_mode(keyboard_obj, LV_KEYBOARD_MODE_TEXT_LOWER);
    else if (!strcmp(key, keyboard_mode_upper)) lv_keyboard_set_mode(keyboard_obj, LV_KEYBOARD_MODE_TEXT_UPPER);
    else if (!strcmp(key, keyboard_mode_special)) lv_keyboard_set_mode(keyboard_obj, LV_KEYBOARD_MODE_SPECIAL);
    else if (!strcmp(key, key_escape)) lv_obj_send_event(keyboard_obj, LV_EVENT_CANCEL, NULL);
    else if (!target) return;
    else if (!strcmp(key, key_enter)) lv_obj_send_event(target, LV_EVENT_READY, NULL);
    else if (!strcmp(key, key_delete)) lv_textarea_delete_char(target);
    else if (!strcmp(key, key_space)) lv_textarea_add_char(target, ' ');
    else lv_textarea_add_text(target, key);
}

static void keyboard(lv_obj_t *target, bool setup_code, int y, int width, int height)
{
    s_keyboard = lv_keyboard_create(s_screen);
    lv_obj_remove_style_all(s_keyboard);
    if (!pet_board_current()->round)
    {
        width = s_width - 32;
        height = page_y(height);
        y = page_y(y);
    }
    lv_obj_set_size(s_keyboard, width, height);
    lv_obj_set_pos(s_keyboard, (s_width - width) / 2, y);
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER,
                        keyboard_lower, keyboard_text_ctrl);
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER,
                        keyboard_upper, keyboard_text_ctrl);
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_SPECIAL,
                        keyboard_special, keyboard_special_ctrl);
    if (setup_code) {
        lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_USER_1,
                            keyboard_code, keyboard_code_ctrl);
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_USER_1);
    }
    else lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_event_cb(s_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_keyboard, key_pressed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_pad_all(s_keyboard, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_keyboard, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_keyboard, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_keyboard, 2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(CARD), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(EDGE), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, lv_color_hex(LIME), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_keyboard, FONT_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(LIME), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(LIME), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_keyboard, lv_color_hex(DARK), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(LIME), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(LIME), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_keyboard, lv_color_hex(DARK), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_keyboard, back_clicked, LV_EVENT_CANCEL, NULL);
    /* READY is handled only by textarea: avoid keyboard/textarea double submit. */
    reveal(s_keyboard);
    lv_keyboard_set_textarea(s_keyboard, target);
    lv_obj_send_event(target, LV_EVENT_FOCUSED, NULL);
}

static void setup_message(void)
{
    if (!s_setup_available) { status_text("CLOUD SETUP MISSING"); return; }
    switch (s_status) {
        case PET_ENROLL_NEEDS_CODE: status_text("READY FOR YOUR CODE"); break;
        case PET_ENROLL_REJECTED: status_text("CODE REJECTED - RECHECK"); break;
        case PET_ENROLL_STORAGE_ERROR: status_text("STORAGE NEEDS RECOVERY"); break;
        case PET_ENROLL_INVALID_CODE: status_text("ENTER ALL 4 CHARACTERS"); break;
        case PET_ENROLL_WAIT_WIFI: status_text("CONNECT WIFI FIRST"); break;
        case PET_ENROLL_WAIT_CLOCK: status_text("SETTING THE CLOCK..."); break;
        /* The cloud asked to wait: the typed code goes when the wait ends. */
        case PET_ENROLL_WAIT_CLOUD: status_text("WAITING..."); break;
        default: status_text("CHECKING CONNECTION..."); break;
    }
}

/* The picture tube: two shutters and the bright line between them. */
static void tube_top(void *object, int32_t height)
{
    lv_obj_set_height((lv_obj_t *)object, height);
}
static void tube_bottom(void *object, int32_t height)
{
    lv_obj_set_y((lv_obj_t *)object, s_height - height);
    lv_obj_set_height((lv_obj_t *)object, height);
}
static void tube_line(void *object, int32_t width)
{
    lv_obj_set_x((lv_obj_t *)object, (s_width - width) / 2);
    lv_obj_set_width((lv_obj_t *)object, width);
}
static void tube_run(lv_obj_t *part, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                     uint32_t duration, uint32_t delay, lv_anim_completed_cb_t done)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, part);
    lv_anim_set_exec_cb(&animation, exec);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_delay(&animation, delay);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    if (done) lv_anim_set_completed_cb(&animation, done);
    lv_anim_start(&animation);
}
static void tube(int32_t shutter, int32_t line_width, lv_opa_t line_opa)
{
    for (unsigned i = 0; i < 3; ++i) {
        s_tube[i] = plain(s_screen);
        lv_obj_set_style_bg_opa(s_tube[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_tube[i], lv_color_hex(i < 2 ? BG : LIME), 0);
    }
    lv_obj_set_pos(s_tube[0], 0, 0);
    lv_obj_set_size(s_tube[0], s_width, shutter);
    lv_obj_set_pos(s_tube[1], 0, s_height - shutter);
    lv_obj_set_size(s_tube[1], s_width, shutter);
    lv_obj_set_pos(s_tube[2], (s_width - line_width) / 2, s_height / 2 - 1);
    lv_obj_set_size(s_tube[2], line_width, 2);
    lv_obj_set_style_opa(s_tube[2], line_opa, 0);
}
/* Power on: a line across the dark tube opens into the page. */
static void crt_on(void)
{
    tube(s_height / 2, 0, LV_OPA_COVER);
    tube_run(s_tube[2], tube_line, 0, s_width, 90, 0, NULL);
    tube_run(s_tube[0], tube_top, s_height / 2, 0, 200, 90, NULL);
    tube_run(s_tube[1], tube_bottom, s_height / 2, 0, 200, 90, NULL);
    tube_run(s_tube[2], fade, LV_OPA_COVER, LV_OPA_TRANSP, 160, 150, NULL);
}

/* The pet shows from another task on some builds: the tube stays dark until
 * it does, and powers the deck back on if it does not within two seconds. */
static void leave_wait(lv_timer_t *timer)
{
    bool gone = lv_screen_active() != s_display_screen;
    if (s_leaving && !gone && lv_tick_elaps(s_leave_tick) < 2000) return;
    lv_timer_delete(timer);
    s_leave_timer = NULL;
    if (!s_leaving) return; /* a page opened meanwhile */
    show_page(s_page, false);
    if (!gone) crt_on();
}
static void back_to_pet(void *unused)
{
    (void)unused;
    if (s_control.return_to_pet) s_control.return_to_pet();
    if (lv_screen_active() != s_display_screen) {
        show_page(s_page, false); /* ready, out of sight, for the next swipe down */
        return;
    }
    s_leave_tick = lv_tick_get();
    if (!s_leave_timer) s_leave_timer = lv_timer_create(leave_wait, 50, NULL);
    if (!s_leave_timer) {
        show_page(s_page, false);
        crt_on();
    }
}
static void tube_dark(lv_anim_t *animation)
{
    (void)animation;
    lv_async_call(back_to_pet, NULL);
}
/* Power off: the page folds into a line, the line into a dot, then the pet. */
static void leave_to_pet(void)
{
    if (s_leaving || !s_device_status.has_pet || !s_control.return_to_pet) return;
    settings_keep(s_setting_timer);
    s_leaving = true;
    tube(0, s_width, LV_OPA_TRANSP);
    tube_run(s_tube[0], tube_top, 0, s_height / 2, 130, 0, NULL);
    tube_run(s_tube[1], tube_bottom, 0, s_height / 2, 130, 0, NULL);
    tube_run(s_tube[2], fade, LV_OPA_TRANSP, LV_OPA_COVER, 60, 80, NULL);
    tube_run(s_tube[2], tube_line, s_width, 0, 90, 140, tube_dark);
}

/* Swiping up leaves a page: to the page it came from, and from the deck
 * itself back to the pet. */
static void swipe_up(void)
{
    switch (s_page) {
        case PAGE_HOME: leave_to_pet(); break;
        case PAGE_WIFI: if (s_paired) show(PAGE_HOME); break;
        case PAGE_PASSWORD: case PAGE_CODE: show(PAGE_WIFI); break;
        case PAGE_RESTART_CONFIRM: case PAGE_LIBRARY: case PAGE_ABOUT: show(PAGE_HOME); break;
        case PAGE_CONFIRM: show(PAGE_LIBRARY); break;
        case PAGE_LINK:
            if (s_link.phase == PET_LINK_CLAIMED || s_link.phase == PET_LINK_LINKING) status_text("ANSWER ON THIS SCREEN");
            else link_leave(s_paired ? PAGE_HOME : PAGE_WIFI);
            break;
        default: break;
    }
}
static void swipe_later(void *unused)
{
    (void)unused;
    swipe_up();
}
static void gesture(lv_event_t *event)
{
    (void)event;
    lv_indev_t *input_device = lv_indev_active();
    if (!input_device || lv_indev_get_gesture_dir(input_device) != LV_DIR_TOP) return;
    /* The press that became a swipe must not also press what it started on. */
    lv_indev_wait_release(input_device);
    lv_async_call(swipe_later, NULL);
}
/* Pulling the deck up past the end of its log closes it too. */
static void deck_scrolled(lv_event_t *event)
{
    lv_obj_t *deck = lv_event_get_current_target_obj(event);
    lv_indev_t *input_device = lv_indev_active();
    if (s_leaving || !input_device || lv_indev_get_state(input_device) != LV_INDEV_STATE_PRESSED ||
        lv_obj_get_scroll_bottom(deck) > -OVERSCROLL_CLOSE) return;
    lv_indev_wait_release(input_device);
    lv_async_call(swipe_later, NULL);
}

/* Follow the physical glass, rather than drawing a round screen on a rectangle. */
static void bezel(void)
{
    s_bezel = plain(s_screen);
    lv_obj_set_size(s_bezel, s_width - 6, s_height - 6);
    lv_obj_center(s_bezel);
    lv_obj_set_style_radius(s_bezel, pet_board_current()->round ? LV_RADIUS_CIRCLE : 20, 0);
    lv_obj_set_style_border_width(s_bezel, 1, 0);
    lv_obj_set_style_border_color(s_bezel, lv_color_hex(UNLIT), 0);
}

/* "> WIFI ............ HOMENET": a boot log line with a dot leader. */
static lv_obj_t *boot_line(int y, uint32_t delay, const char *key, const char *value, uint32_t color)
{
    char line[40];
    int dots = 26 - (int)strlen(key) - (int)strlen(value);
    snprintf(line, sizeof(line), "> %s %.*s %s", key, dots < 1 ? 1 : dots, "..........................", value);
    lv_obj_t *obj = text(s_screen, FONT_SMALL, line, 0, page_y(y), color, 1);
    lv_obj_set_x(obj, (s_width - text_width(FONT_SMALL, line, 1)) / 2);
    reveal_at(obj, delay);
    return obj;
}

/* The terminal starts: the boot mark lit in lime, the version, then a short
 * boot log of what is true now. */
static void boot_page(void)
{
    const lv_image_dsc_t *logo = &pet_boot_mark;
    lv_obj_t *mark = icon(s_screen, logo, (s_width - (int32_t)logo->header.w) / 2, page_y(50), LIME);
    if (SMALL_SCREEN) {
        /* The mark keeps the 360px layout's proportion, shrunk from its top. */
        lv_image_set_pivot(mark, (int32_t)logo->header.w / 2, 0);
        lv_image_set_scale(mark, (uint32_t)(LV_SCALE_NONE * s_width / 360));
    }
    reveal_at(mark, 120);
    const char *version = esp_app_get_description()->version;
    if (!strncmp(version, "aipet-", 6)) version += 6;
    char line[40], field[16];
    display_text(field, sizeof(field), version, 12, true);
    snprintf(line, sizeof(line), HOST " TERMINAL V%s", field);
    for (char *p = line; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    reveal_at(centered(s_screen, FONT_SMALL, line, 176, 300, LABEL, 2), 300);
    display_text(field, sizeof(field), s_device_status.ssid, 12, true);
    boot_line(206, 520, "DISPLAY", "OK", LIME);
    boot_line(224, 680, "TOUCH", "OK", LIME);
    boot_line(242, 840, "WIFI", field[0] ? field : "SETUP", field[0] ? LIME : AMBER);
    lv_obj_t *last = boot_line(260, 1000, "PETS", "LOADING", LIME);
    s_cursor = plain(s_screen);
    lv_obj_set_size(s_cursor, 6, 11);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(LIME), 0);
    /* The line's own position: its laid-out coordinates are not known yet. */
    lv_obj_set_pos(s_cursor, lv_obj_get_style_x(last, 0) + text_width(FONT_SMALL, lv_label_get_text(last), 1) + 4,
                   lv_obj_get_style_y(last, 0) + 1);
    reveal_at(s_cursor, 1000);
}

static void wifi_page(void)
{
    title(s_screen, "WI-FI", 26);
    s_network_list = plain(s_screen);
    lv_obj_add_flag(s_network_list, LV_OBJ_FLAG_SCROLLABLE);
    page_box(s_network_list, 36, 80, 288, 162);
    int32_t list_width = lv_obj_get_style_width(s_network_list, 0);
    lv_obj_set_scroll_dir(s_network_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_network_list, LV_SCROLLBAR_MODE_OFF);
    reveal(s_network_list);
    if (!s_network_count) {
        lv_obj_t *empty = centered(s_network_list, FONT_SMALL, s_scanning ? "" : "NO NETWORKS FOUND",
                                   64, list_width, DIM, 2);
        (void)empty;
    }
    bool online = pet_network_wifi_is_ready();
    for (size_t i = 0; i < s_network_count; ++i) {
        char name[36];
        display_text(name, sizeof(name), s_networks[i].ssid, 32, false);
        bool here = online && !strcmp(s_networks[i].ssid, s_device_status.ssid);
        s_animate = false; /* the list prints as one */
        lv_obj_t *item = card(s_network_list, 0, (int32_t)i * 42, list_width, 38, CARD_PRESS);
        s_animate = true;
        lv_obj_t *bars = plain(item);
        lv_obj_set_pos(bars, 12, 9);
        lv_obj_set_size(bars, 22, 17);
        int8_t rssi = s_networks[i].rssi;
        lv_obj_set_user_data(bars, (void *)(uintptr_t)(rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1));
        lv_obj_add_event_cb(bars, draw_signal, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_t *caption = text(item, FONT_TEXT, name, 44, 9, here ? LIME : LABEL, 0);
        lv_obj_set_size(caption, list_width - 118, 19);
        lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
        const char *tag = here ? "ON" : s_networks[i].saved ? "SAVED" : !s_networks[i].secured ? "OPEN" : "";
        lv_obj_t *note = text(item, FONT_SMALL, tag, 0, 0, DIM, 1);
        lv_obj_align(note, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_add_event_cb(item, network_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    if (!s_enrolled && online) {
        command(s_screen, "SCAN", NULL, 40, 250, 90, 38, scan_clicked);
        command(s_screen, "HIDDEN", NULL, 135, 250, 90, 38, hidden_clicked);
        command(s_screen, "NEXT", NULL, 230, 250, 90, 38, link_clicked);
    } else {
        command(s_screen, "SCAN", &pet_icon_sync, 40, 250, 136, 38, scan_clicked);
        command(s_screen, "HIDDEN", NULL, 184, 250, 136, 38, hidden_clicked);
    }
    status_line(s_screen, 60, s_paired ? "SELECT A NETWORK" : online ? "WIFI CONNECTED" : "SELECT A NETWORK");
    if (s_paired) handle();
}

static void password_page(void)
{
    title(s_screen, "JOIN", 26);
    char ssid[36], line[48];
    display_text(ssid, sizeof(ssid), s_selected_ssid, 18, false);
    snprintf(line, sizeof(line), "KEY FOR %s", ssid);
    int y = 78;
    if (s_hidden) {
        s_ssid = input("network name", 64, 76, 232, 32, false);
        y = 114;
    }
    s_password = input("password", 44, y, 206, 64, true);
    s_password_reveal = command(s_screen, "SHOW", NULL, 256, y, 60, 34, password_visibility_clicked);
    if (s_hidden) keyboard(s_ssid, false, 156, HIDDEN_KEYBOARD_W, HIDDEN_KEYBOARD_H);
    else keyboard(s_password, false, 120, TEXT_KEYBOARD_W, TEXT_KEYBOARD_H);
    lv_obj_add_event_cb(s_password, join_clicked, LV_EVENT_READY, NULL);
    if (s_ssid) lv_obj_add_event_cb(s_ssid, next_field, LV_EVENT_READY, NULL);
    status_line(s_screen, 60, s_hidden ? "HIDDEN NETWORK" : line);
    handle();
}

static void code_page(void)
{
    title(s_screen, "PAIR", 26);
    /* The textarea holds the code; four cells show it. */
    s_code = lv_textarea_create(s_screen);
    lv_obj_add_flag(s_code, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(s_code, true);
    lv_textarea_set_max_length(s_code, PET_ENROLLMENT_CODE_LENGTH);
    lv_textarea_set_accepted_chars(s_code, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
    lv_obj_add_event_cb(s_code, code_changed, LV_EVENT_VALUE_CHANGED, NULL);
    for (unsigned i = 0; i < 4; ++i) {
        s_code_cell[i] = card(s_screen, 94 + 44 * (int32_t)i, 78, 40, 40, CARD_PLAIN);
        lv_obj_t *letter = text(s_code_cell[i], FONT_BIG, "_", 0, 0, DIM, 0);
        lv_obj_center(letter);
    }
    keyboard(s_code, true, 126, CODE_KEYBOARD_W, CODE_KEYBOARD_H);
    lv_obj_add_event_cb(s_code, pair_clicked, LV_EVENT_READY, NULL);
    status_line(s_screen, 60, "CODE FROM AIPETS.COM");
    code_changed(NULL);
    setup_message();
    handle();
}

/* The link page's resting line: what is happening, or why not, with the
 * cloud's code for support when nothing plainer fits. */
static const char *link_hint(void)
{
    static char failed[36];
    const pet_link_t *l = &s_link;
    const char *e = l->error;
    switch (l->phase) {
        case PET_LINK_ASKING: return "GETTING A CODE...";
        case PET_LINK_SHOWING: return "ENTER THIS CODE AT";
        case PET_LINK_CLAIMED:
            return !strcmp(e, "DEVICE_BUSY") ? "BUSY INSTALLING - TRY AGAIN" :
                !strcmp(e, "LINK_UNAVAILABLE") ? "NEW ACCOUNT CAN'T TAKE IT YET" :
                l->moving && strlen(l->check) != PET_LINK_CHECK_LENGTH ? "NO NUMBER - CAN'T MOVE" :
                l->check[0] ? "SAME NUMBER ON AIPETS.COM?" : "IS THIS YOUR ACCOUNT?";
        case PET_LINK_LINKING: return l->moving ? "MOVING..." : "LINKING...";
        case PET_LINK_DECLINING: return "GETTING A NEW CODE...";
        case PET_LINK_LINKED: return l->moving ? "MOVED" : "LINKED";
        default: break;
    }
    /* Paused, failed or stopped: why there is no code. */
    return !e[0] ? "TAP FOR A CODE" :
        !strcmp(e, "LINK_OFFERS_DISABLED") || !strcmp(e, "RELINK_DISABLED") ? "LINK CODES ARE OFF FOR NOW" :
        !strcmp(e, "RATE_LIMITED") ? "TOO MANY CODES - WAIT" :
        !strcmp(e, "DEVICE_BUSY") ? "BUSY INSTALLING - WAIT" :
        !strcmp(e, "LINK_CODE_EXPIRED") ? "CODE EXPIRED - TAP IT" :
        !strcmp(e, "LINK_NO_ANSWER") ? "NO ANSWER - TAP TO RETRY" :
        !strcmp(e, "SETUP_IDENTITY_TAKEN") ? "ID IN USE - TYPE A CODE" :
        !strcmp(e, "DEVICE_REVOKED") ? "REMOVED FROM ITS ACCOUNT" :
        !strcmp(e, "RELINK_UNSUPPORTED") ? "THIS DEVICE CAN'T MOVE" :
        !strcmp(e, "DEVICE_AUTH_REQUIRED") ? "DEVICE NOT AUTHORIZED" :
        !strcmp(e, "NOT_FOUND") ? "LINK CODES NOT AVAILABLE" :
        l->phase == PET_LINK_PAUSED ? "CLOUD BUSY - RETRYING" :
        (snprintf(failed, sizeof(failed), "LINK FAILED: %.18s", e), failed);
}

/* LINK: the code to type at aipets.com/link, then the question, answered only
 * here. A device without an account keeps the typed code as its fallback. */
static void link_page(void)
{
    const pet_link_t *l = &s_link;
    title(s_screen, "LINK", 26);
    if (l->phase == PET_LINK_CLAIMED || l->phase == PET_LINK_LINKING) {
        /* The name alone proves nothing: the claim's number, large, must match
         * the one aipets.com shows the owner who typed the code. */
        lv_obj_t *face = card(s_screen, 48, 84, 264, 84, CARD_PLAIN);
        char name[84];
        const bool numbered = strlen(l->check) == PET_LINK_CHECK_LENGTH;
        display_text(name, sizeof(name), l->account, 80, true);
        /* A name with no letter or digit the menu face can draw reads as none. */
        const bool named = strcspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") < strlen(name);
        text(face, FONT_SMALL, l->moving ? "MOVE TO" : "LINK TO", 14, 10, LABEL, 2);
        lv_obj_t *who = text(face, FONT_TEXT, named ? name : l->moving ? "ANOTHER ACCOUNT" : "THIS ACCOUNT",
                             14, 28, LIME, 1);
        lv_obj_set_size(who, numbered ? 176 : 236, 42);
        lv_label_set_long_mode(who, LV_LABEL_LONG_DOT);
        if (numbered) {
            text(face, FONT_SMALL, "NUMBER", 198, 10, LABEL, 2);
            text(face, FONT_BIG, l->check, 198, 30, LIME, 3);
        }
        /* A move sends the claim's number: without one, only NOT ME. */
        const bool yes = !l->moving || numbered;
        if (l->phase == PET_LINK_CLAIMED) {
            command(s_screen, "NOT ME", NULL, yes ? 52 : 118, 184, 124, 42, link_no_clicked);
            if (yes) command(s_screen, l->moving ? "MOVE" : "LINK", NULL, 184, 184, 124, 42, link_yes_clicked);
        }
    } else {
        /* A code past its time, or refused, is asked for again from here. */
        const bool shown = l->phase == PET_LINK_SHOWING && strlen(l->code) == PET_LINK_CODE_LENGTH;
        const bool again = l->phase == PET_LINK_OFF || l->phase == PET_LINK_FAILED;
        lv_obj_t *face = card(s_screen, 48, 84, 264, 56, again ? CARD_PRESS : CARD_PLAIN);
        if (again) lv_obj_add_event_cb(face, link_clicked, LV_EVENT_CLICKED, NULL);
        char code[PET_LINK_CODE_LENGTH + 2], device[16];
        if (shown) snprintf(code, sizeof(code), "%.4s-%.4s", l->code, l->code + 4);
        else strcpy(code, "____-____");
        centered(face, FONT_BIG, code, 13, 264, shown ? LIME : DIM, 3);
        centered(s_screen, FONT_TEXT, "AIPETS.COM/LINK", 150, 300, shown ? LIME : DIM, 2);
        display_text(device, sizeof(device), l->device, 6, true);
        if (device[0]) {
            char line[24];
            snprintf(line, sizeof(line), "DEVICE %s", device);
            centered(s_screen, FONT_SMALL, line, 176, 300, DIM, 2);
        }
        if (l->moving) command(s_screen, "CANCEL", NULL, 110, 204, 140, 40, link_cancel_clicked);
        else command(s_screen, "TYPE A CODE", NULL, 90, 204, 180, 40, code_clicked);
    }
    status_line(s_screen, 60, link_hint());
    handle();
}

/* The setup worker's news about the link, on its task. The question comes to
 * the owner when a menu page is up; a move ends on the deck, and control is
 * asked at once, to adopt the new account. */
static void link_changed(const pet_link_t *link)
{
    if (!link || pet_display_lock(-1) != ESP_OK) return;
    pet_link_t before = s_link;
    s_link = *link;
    const bool changed = before.phase != s_link.phase || before.moving != s_link.moving || strcmp(before.code, s_link.code) ||
        strcmp(before.account, s_link.account) || strcmp(before.error, s_link.error) || strcmp(before.check, s_link.check);
    const bool visible = lv_screen_active() == s_display_screen;
    if (s_link.phase == PET_LINK_LINKED && s_link.moving && before.phase != PET_LINK_LINKED) {
        if (s_control.retry_sync) s_control.retry_sync();
        if (s_page == PAGE_LINK) {
            show(PAGE_HOME);
            status_text("MOVED - SYNCING");
        }
    } else if (s_page == PAGE_LINK && changed) show_page(PAGE_LINK, false);
    else if (s_link.phase == PET_LINK_CLAIMED && before.phase != PET_LINK_CLAIMED && visible &&
             s_page != PAGE_BOOT && s_page != PAGE_PASSWORD) show(PAGE_LINK);
    pet_enrollment_clear(&before, sizeof(before));
    pet_display_unlock();
}

/* Broken by hand into centred lines of at most 30 characters, the width of
 * the column in the text face. */
static const char about_story[] =
    "AI Pets is a little companion\nthat lives with you.\n\n"
    "We started aipets.com so\neveryone can have their\nown AI agents and own\nall of their data.\n\n"
    "Your pet, its memories\nand your conversations\nbelong to you.";
/* LVGL's default font, Montserrat 14, is linked and carries Font Awesome's
 * symbols; the OFL text below covers both. Only some cues come from UISFX. */
static const char about_notices[] =
    "Share Tech Mono  SIL OFL 1.1, below\n"
    "Montserrat       SIL OFL 1.1, below\n"
    "  (c) 2011 The Montserrat\n"
    "  Project Authors\n"
    "Font Awesome     SIL OFL 1.1, below\n"
    "  (c) 2022 Fonticons, Inc.\n"
    "LVGL             MIT\n"
    "ESP-IDF          Apache-2.0\n"
    "Some sounds      UISFX, CC0";

/* ABOUT: why aipets.com exists, then the open-source notices. Every copy of
 * the firmware carries the menu font's notice and license, verbatim, and
 * shows them here. The column is clickable, so a finger scrolls it. */
static void about_page(void)
{
    title(s_screen, "ABOUT", 26);
    lv_obj_t *body = lv_obj_create(s_screen);
    lv_obj_remove_style_all(body);
    page_box(body, 44, 80, 272, 214);
    int32_t body_width = lv_obj_get_style_width(body, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 18, 0);
    lv_obj_t *story = text(body, FONT_TEXT, about_story, 0, 0, LIME, 0);
    lv_obj_t *site = text(body, FONT_TITLE, "aipets.com", 0, 0, LIME, 1);
    lv_obj_set_width(story, body_width);
    lv_obj_set_width(site, body_width);
    lv_obj_set_style_text_align(story, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(site, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *heading = plain(body);
    lv_obj_set_size(heading, body_width, 13);
    text(heading, FONT_SMALL, "// OPEN-SOURCE NOTICES", 0, 0, LABEL, 2);
    lv_obj_t *line = plain(heading);
    int32_t left = text_width(FONT_SMALL, "// OPEN-SOURCE NOTICES", 2) + 8;
    lv_obj_set_pos(line, left, 6);
    lv_obj_set_size(line, body_width - left, 1);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(EDGE), 0);
    text(body, FONT_SMALL, about_notices, 0, 0, LABEL, 0);
    lv_obj_t *words = text(body, FONT_SMALL, "", 0, 0, DIM, 0);
    lv_obj_set_width(words, body_width);
    lv_label_set_text_static(words, pet_menu_font_license);
    reveal(body);
    status_line(s_screen, 60, "YOUR PET, YOUR DATA");
    handle();
}

/* The deck: title, status, cards, then the system log further down. */
static void home_page(void)
{
    s_menu = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, s_width, s_height);
    lv_obj_set_scroll_dir(s_menu, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_menu, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_menu, deck_scrolled, LV_EVENT_SCROLL, NULL);
    lv_obj_t *pill = plain(s_menu);
    lv_obj_set_pos(pill, (s_width - 40) / 2, 12);
    lv_obj_set_size(pill, 40, 4);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(LABEL), 0);
    title(s_menu, "CONTROL", 24);
    status_line(s_menu, 60, NULL);
    s_progress = lv_bar_create(s_menu);
    lv_obj_remove_style_all(s_progress);
    lv_obj_set_pos(s_progress, (s_width - 160) / 2, 76);
    lv_obj_set_size(s_progress, 160, 3);
    lv_bar_set_range(s_progress, 0, 100);
    lv_obj_add_event_cb(s_progress, draw_segments, LV_EVENT_DRAW_MAIN, (void *)&s_progress_segments);

    lv_obj_t *wifi = card(s_menu, CARD_X, 84, 182, 64, CARD_PRESS);
    lv_obj_add_event_cb(wifi, back_clicked, LV_EVENT_CLICKED, NULL);
    s_wifi_icon = icon(wifi, &pet_icon_wifi, 10, 22, LIME);
    lv_obj_t *rule = plain(wifi);
    lv_obj_set_pos(rule, 44, 10);
    lv_obj_set_size(rule, 1, 44);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(EDGE), 0);
    text(wifi, FONT_SMALL, "WI-FI", 52, 9, LABEL, 2);
    s_wifi_name = text(wifi, FONT_TEXT, "", 52, 24, LIME, 0);
    lv_obj_set_size(s_wifi_name, 110, 19);
    lv_label_set_long_mode(s_wifi_name, LV_LABEL_LONG_DOT);
    s_wifi_state = text(wifi, FONT_SMALL, "", 52, 44, DIM, 2);
    icon(wifi, &pet_icon_chevron_right, lv_obj_get_style_width(wifi, 0) - 16, 25, DIM);

    lv_obj_t *battery = card(s_menu, CARD_X + 188, 84, 116, 64, CARD_PLAIN);
    text(battery, FONT_SMALL, "BATTERY", 10, 9, LABEL, 2);
    s_battery_icon = plain(battery);
    lv_obj_set_pos(s_battery_icon, 10, 33);
    lv_obj_set_size(s_battery_icon, 30, 16);
    lv_obj_add_event_cb(s_battery_icon, draw_battery, LV_EVENT_DRAW_MAIN, NULL);
    s_battery_value = text(battery, FONT_BIG, "", 46, 25, LIME, 0);

    int y = 154;
    if (s_control.volume) { setting_card(SETTING_VOLUME, y); y += 64; }
    if (s_control.brightness) { setting_card(SETTING_BRIGHTNESS, y); y += 64; }

    lv_obj_t *voice = card(s_menu, CARD_X, y, HALF_W, 56, CARD_PLAIN);
    icon(voice, &pet_icon_mic, 10, 16, LIME);
    rule = plain(voice);
    lv_obj_set_pos(rule, 36, 9);
    lv_obj_set_size(rule, 1, 38);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(EDGE), 0);
    text(voice, FONT_SMALL, "VOICE", 44, 9, LABEL, 2);
    s_voice_value = text(voice, FONT_TEXT, "", 44, 27, LIME, 0);
    lv_obj_t *pets = card(s_menu, CARD_X + HALF_W + 8, y, HALF_W, 56, CARD_PLAIN);
    icon(pets, &pet_icon_paw, 9, 17, LIME);
    rule = plain(pets);
    lv_obj_set_pos(rule, 38, 9);
    lv_obj_set_size(rule, 1, 38);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(EDGE), 0);
    s_pets_label = text(pets, FONT_SMALL, "PET", 46, 9, LABEL, 2);
    s_pets_value = text(pets, FONT_TEXT, "", 46, 27, LIME, 0);
    lv_obj_set_size(s_pets_value, 94, 19);
    lv_label_set_long_mode(s_pets_value, LV_LABEL_LONG_DOT);
    y += 62;

    if (s_enrolled) command(s_menu, "SYNC", &pet_icon_sync, CARD_X, y, HALF_W, 44, retry_clicked);
    else command(s_menu, "LINK", NULL, CARD_X, y, HALF_W, 44, link_clicked);
    command(s_menu, "REBOOT", &pet_icon_power, CARD_X + HALF_W + 8, y, HALF_W, 44, restart_prompt_clicked);
    y += 50;
#if CONFIG_PET_DEVICE_RELINK
    /* A linked Pocket Terminal moves with a code too: its control adopts the
     * new account without a restart. */
    if (s_enrolled) {
        command(s_menu, "LINK TO ANOTHER ACCOUNT", NULL, CARD_X, y, CARD_W, 44, link_clicked);
        y += 50;
    }
#endif
#if CONFIG_PET_VNEXT_DEVELOPER_LIBRARY
    if (s_control.library) {
        command(s_menu, "PET LIBRARY", &pet_icon_paw, CARD_X, y, CARD_W, 44, library_clicked);
        y += 50;
    }
#endif
    y += 14;
    text(s_menu, FONT_SMALL, "// SYSTEM LOG", page_x(LOG_X), y, LABEL, 2);
    lv_obj_t *line = plain(s_menu);
    lv_obj_set_pos(line, page_x(LOG_X) + 118, y + 6);
    lv_obj_set_size(line, s_width - 2 * page_x(LOG_X) - 118, 1);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(EDGE), 0);
    y += 24;
    for (unsigned i = 0; i < LOG_LINES; ++i) {
        s_log[i] = text(s_menu, FONT_SMALL, "", page_x(LOG_X), y + 15 * (int32_t)i, DIM, 0);
        /* One row each: a line too long for the screen ends in dots. */
        lv_obj_set_size(s_log[i], s_width - 2 * page_x(LOG_X), lv_font_get_line_height(FONT_SMALL));
        lv_label_set_long_mode(s_log[i], LV_LABEL_LONG_DOT);
    }
    s_cursor = plain(s_menu);
    lv_obj_set_pos(s_cursor, page_x(LOG_X) + 12, y + 15 * (LOG_LINES - 1) + 1);
    lv_obj_set_size(s_cursor, 6, 11);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(LIME), 0);
    y += 15 * LOG_LINES + 12;
    command(s_menu, "ABOUT", NULL, 100, y, 160, 38, about_clicked);
    /* Room to read the last lines in the middle of the glass. */
    lv_obj_t *end = plain(s_menu);
    lv_obj_set_pos(end, 0, y + 38 + 100);
    lv_obj_set_size(end, 1, 1);
    handle();
    render_status();
}

static void show_page(page_t page, bool animate)
{
    if (page == PAGE_CODE && s_enrolled) page = PAGE_HOME;
    if (s_keyboard) lv_keyboard_set_textarea(s_keyboard, NULL);
    if (s_password) lv_textarea_set_text(s_password, "");
    if (s_code) lv_textarea_set_text(s_code, "");
    lv_obj_clean(s_screen);
    s_status_label = s_status_cursor = s_network_list = s_ssid = s_password = s_code = s_keyboard = NULL;
    s_password_reveal = s_menu = s_progress = s_wifi_icon = s_wifi_name = s_wifi_state = NULL;
    s_battery_icon = s_battery_value = s_voice_value = s_pets_label = s_pets_value = NULL;
    s_cursor = s_bezel = s_tube[0] = s_tube[1] = s_tube[2] = NULL;
    memset(s_code_cell, 0, sizeof(s_code_cell));
    memset(s_log, 0, sizeof(s_log));
    memset(s_slider, 0, sizeof(s_slider));
    memset(s_slider_value, 0, sizeof(s_slider_value));
    s_library_dropdown = NULL;
    s_hint = NULL;
    s_message = false;
    s_typing[0] = 0;
    s_typing_shown = s_typing_length = 0;
    s_password_visible = false;
    s_leaving = false;
    s_animate = animate;
    s_reveal_order = 0;
    lv_timer_pause(s_status_timer);
    lv_timer_pause(s_type_timer);
    s_page = page;
    bezel();
    if (page == PAGE_BOOT) boot_page();
    else if (page == PAGE_WIFI) wifi_page();
    else if (page == PAGE_PASSWORD) password_page();
    else if (page == PAGE_CODE) code_page();
    else if (page == PAGE_HOME) home_page();
    else if (page == PAGE_ABOUT) about_page();
    else if (page == PAGE_LINK) link_page();
    else if (page == PAGE_LIBRARY) {
#if CONFIG_PET_VNEXT_DEVELOPER_LIBRARY
        title(s_screen,"PETS",26);
        s_library_dropdown=lv_dropdown_create(s_screen);
        lv_obj_remove_style_all(s_library_dropdown);
        page_box(s_library_dropdown, 50, 84, 260, 38);
        lv_obj_set_style_text_font(s_library_dropdown,FONT_TEXT,0);
        lv_obj_set_style_text_color(s_library_dropdown,lv_color_hex(LIME),0);
        lv_obj_set_style_radius(s_library_dropdown,3,0);
        lv_obj_set_style_bg_opa(s_library_dropdown,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(s_library_dropdown,lv_color_hex(CARD),0);
        lv_obj_set_style_border_width(s_library_dropdown,1,0);
        lv_obj_set_style_border_color(s_library_dropdown,lv_color_hex(EDGE),0);
        lv_obj_set_style_pad_hor(s_library_dropdown,10,0);lv_obj_set_style_pad_ver(s_library_dropdown,9,0);
        lv_dropdown_set_symbol(s_library_dropdown,"v");
        lv_obj_add_event_cb(s_library_dropdown,pet_list_opened,LV_EVENT_READY,NULL);
        lv_dropdown_clear_options(s_library_dropdown);
        if(!s_library.count)lv_dropdown_add_option(s_library_dropdown,"no approved pets yet",LV_DROPDOWN_POS_LAST);
        for(size_t i=0;i<s_library.count;++i) {
            char name[112],shown[112];display_text(name,sizeof(name),s_library.items[i].name,80,false);
            snprintf(shown,sizeof(shown),"%.80s%s",name,s_library.items[i].selected?" *":"");
            lv_dropdown_add_option(s_library_dropdown,shown,LV_DROPDOWN_POS_LAST);
        }
        reveal(s_library_dropdown);
        command(s_screen,"REFRESH",&pet_icon_sync,40,140,136,40,library_clicked);
        command(s_screen,"CHOOSE",NULL,184,140,136,40,choose_pet_clicked);
        if(s_library.next_cursor[0])command(s_screen,"NEXT PAGE",NULL,95,190,170,40,next_library_clicked);
        status_line(s_screen,60,"DEVELOPMENT LIBRARY");
        handle();
#else
        show_page(PAGE_HOME, animate);
#endif
    } else if (page == PAGE_CONFIRM) {
        const pet_control_library_item_t *pet=&s_library.items[s_selected_pet];
        title(s_screen,"INSTALL",26);
        char name[36];display_text(name,sizeof(name),pet->name,24,true);
        lv_obj_t *info=card(s_screen,CARD_X,84,CARD_W,92,CARD_PLAIN);
        lv_obj_t *heading=text(info,FONT_TEXT,name,14,10,LIME,1);
        lv_obj_set_size(heading,270,19);lv_label_set_long_mode(heading,LV_LABEL_LONG_DOT);
        char version[28],details[160];display_text(version,sizeof(version),pet->pack_version,24,false);
        snprintf(details,sizeof(details),"VERSION %s\nBUILD %.8s\n%lu KIB",version,pet->build_id,(unsigned long)((pet->bytes+1023)/1024));
        text(info,FONT_SMALL,details,14,36,LABEL,1);
        command(s_screen,"BACK",NULL,40,190,136,40,library_back_clicked);
        command(s_screen,"INSTALL",NULL,184,190,136,40,confirm_pet_clicked);
        status_line(s_screen,60,"YOUR PET STAYS UNTIL VERIFIED");
        handle();
    } else if (page == PAGE_RESTART_CONFIRM) {
        title(s_screen, "REBOOT", 26);
        lv_obj_t *info = card(s_screen, CARD_X + 12, 96, CARD_W - 24, 64, CARD_PLAIN);
        centered(info, FONT_SMALL, "WI-FI, PAIRING AND YOUR\nPETS ARE ALL KEPT.", 16, CARD_W - 24, LABEL, 2);
        command(s_screen, "CANCEL", NULL, 40, 180, 136, 42, home_clicked);
        command(s_screen, "REBOOT", &pet_icon_power, 184, 180, 136, 42, restart_clicked);
        status_line(s_screen, 60, "REBOOT THE TERMINAL?");
        handle();
    }
}

static void show(page_t page)
{
    show_page(page, page != s_page);
}

static void scan_finished(const pet_wifi_network_t *networks, size_t count, esp_err_t result)
{
    if (pet_display_lock(-1) != ESP_OK) return;
    s_scanning = false;
    if (result == ESP_OK) {
        s_network_count = count < PET_WIFI_SCAN_MAX ? count : PET_WIFI_SCAN_MAX;
        if (s_network_count) memcpy(s_networks, networks, s_network_count * sizeof(*networks));
        if (s_page == PAGE_WIFI) show(PAGE_WIFI);
    } else if (s_page == PAGE_WIFI) status_text("SCAN FAILED - RESCAN");
    pet_display_unlock();
}

static void setup_status(pet_enrollment_result_t status)
{
    if (pet_display_lock(-1) != ESP_OK) return;
    s_status = status;
    if(status==PET_ENROLL_COMPLETE)s_paired=s_enrolled=true;
    if (s_page == PAGE_CODE || s_page == PAGE_LINK) {
        if (status == PET_ENROLL_COMPLETE) show(PAGE_HOME);
        else if (s_page == PAGE_CODE) setup_message();
    }
    pet_display_unlock();
}

static void poll_connection(lv_timer_t *timer)
{
    (void)timer;
    bool visible=lv_screen_active()==s_display_screen;
    if(visible){pet_diagnostics_note_native_ui_heartbeat();atomic_store(&s_ui_last_tick,lv_tick_get());}
    atomic_store(&s_ui_visible_live,visible);
    bool online = pet_network_wifi_is_ready();
    if (s_page == PAGE_BOOT) return;
    if (online != s_last_online) {
        s_last_online = online;
        s_device_status.wifi = online ? PET_ONBOARDING_WIFI_CONNECTED :
            (s_device_status.ssid[0] ? PET_ONBOARDING_WIFI_CONNECTING :
             PET_ONBOARDING_WIFI_DISCONNECTED);
        if (s_page == PAGE_WIFI) show(PAGE_WIFI);
        if (s_page == PAGE_CODE && !online) status_text("WIFI LOST - CHECK WIFI");
        render_status();
    }
}

static void finish_splash(lv_timer_t *timer)
{
    (void)timer;
    if (s_splash_timer) {
        lv_timer_delete(s_splash_timer);
        s_splash_timer = NULL;
    }
    show(s_paired ? PAGE_HOME : PAGE_WIFI);
    command_t command = {.scan = true};
    if (enqueue(&command)) s_scanning = true;
}

/* The boot log stays until a pet takes the screen, or until it is clear none
 * will: then the setup or the deck. */
static void splash_tick(lv_timer_t *timer)
{
    uint32_t elapsed = lv_tick_elaps(s_boot_tick);
    if (elapsed < 1500) return;
    if (lv_screen_active() != s_display_screen || (s_status_known && !s_device_status.has_pet) || elapsed >= 3000)
        finish_splash(timer);
}

#if CONFIG_PET_VNEXT_ENROLLMENT && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void usb_wifi_request(const pet_usb_wifi_request_t *request, void *context)
{
    (void)context;
    uint8_t address[6];
    char mac[18], output[PET_USB_WIFI_FRAME_BYTES];
    if (esp_read_mac(address, ESP_MAC_WIFI_STA) != ESP_OK) return;
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0], address[1], address[2], address[3], address[4], address[5]);
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return;
    cJSON_AddNumberToObject(reply, "v", 1);
    cJSON_AddStringToObject(reply, "requestId", request->request_id);
    const char *error = NULL;
    bool saved_settings = false;
    if (request->action == PET_USB_WIFI_INFO)
    {
        wifi_ap_record_t ap = {0};
        bool connected = pet_network_wifi_is_ready() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        cJSON_AddStringToObject(reply, "status", "info");
        cJSON_AddStringToObject(reply, "mac", mac);
        cJSON_AddStringToObject(reply, "board", pet_board_current()->hardware);
        cJSON_AddStringToObject(reply, "firmware", esp_app_get_description()->version);
        cJSON_AddBoolToObject(reply, "connected", connected);
        char ssid[PET_SSID_MAX] = {0};
        if (connected) memcpy(ssid, ap.ssid, sizeof(ap.ssid));
        else if (pet_display_lock(-1) == ESP_OK)
        {
            strlcpy(ssid, s_device_status.ssid, sizeof(ssid));
            pet_display_unlock();
        }
        cJSON_AddStringToObject(reply, "ssid", ssid);
        cJSON *saved = cJSON_AddArrayToObject(reply, "saved");
        pet_wifi_network_metadata_t networks[PET_WIFI_PROFILE_MAX] = {0};
        size_t count = 0;
        if (pet_config_list_wifi_networks(networks, PET_WIFI_PROFILE_MAX, &count) == ESP_OK)
        {
            for (size_t i = 0; i < count; ++i) cJSON_AddItemToArray(saved, cJSON_CreateString(networks[i].ssid));
        }
    }
    else if (strcmp(request->mac, mac)) error = "WRONG_DEVICE";
    else
    {
        bool busy = true;
        if (pet_display_lock(-1) == ESP_OK)
        {
            busy = s_device_status.installation == PET_ONBOARDING_INSTALL_REQUESTED ||
                s_device_status.installation == PET_ONBOARDING_INSTALL_DOWNLOADING ||
                s_device_status.installation == PET_ONBOARDING_INSTALL_VERIFYING ||
                s_device_status.installation == PET_ONBOARDING_INSTALL_ACTIVATING;
            pet_display_unlock();
        }
        char password[PET_PASSWORD_MAX] = {0};
        if (busy) error = "BUSY";
        else if (request->action == PET_USB_WIFI_SAVED &&
                 pet_config_recall_wifi(request->ssid, password, sizeof(password)) != ESP_OK)
            error = "SAVED_NETWORK_MISSING";
        else
        {
            if (request->action == PET_USB_WIFI_SAVE) strcpy(password, request->password);
            if (pet_config_store_wifi_explicit(request->ssid, password) != ESP_OK) error = "SAVE_FAILED";
            else
            {
                cJSON_AddStringToObject(reply, "status", "saved");
                cJSON_AddStringToObject(reply, "mac", mac);
                saved_settings = true;
            }
        }
        pet_enrollment_clear(password, sizeof(password));
    }
    if (error)
    {
        cJSON_AddStringToObject(reply, "status", "error");
        cJSON_AddStringToObject(reply, "code", error);
    }
    const char prefix[] = "\n" PET_USB_WIFI_PREFIX;
    memcpy(output, prefix, sizeof(prefix) - 1);
    if (cJSON_PrintPreallocated(reply, output + sizeof(prefix) - 1,
                               sizeof(output) - sizeof(prefix) - 1, false))
    {
        strcat(output, "\n");
        /* Keep log output out of this frame, and drain its final USB packet
         * before the other core can service a restart request. */
        flockfile(stdout);
        fflush(stdout);
        int sent = usb_serial_jtag_write_bytes(output, strlen(output), pdMS_TO_TICKS(500));
        bool delivered = sent == (int)strlen(output) &&
            usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500)) == ESP_OK;
        funlockfile(stdout);
        if (saved_settings && delivered && s_control.restart) s_control.restart();
    }
    cJSON_Delete(reply);
}

static void usb_wifi_poll(void)
{
    if (!s_usb_wifi_ready) return;
    uint8_t bytes[256];
    uint32_t now = lv_tick_get();
    if ((s_usb_wifi.length || s_usb_wifi.overflow) && now - s_usb_wifi_last_input >= 2000)
        pet_enrollment_clear(&s_usb_wifi, sizeof(s_usb_wifi));
    int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0);
    if (count > 0)
    {
        s_usb_wifi_last_input = now;
        pet_usb_wifi_feed(&s_usb_wifi, bytes, (size_t)count, usb_wifi_request, NULL);
    }
    pet_enrollment_clear(bytes, sizeof(bytes));
}
#endif

static void command_task(void *argument)
{
    (void)argument;
    command_t command;
    for (;;) {
#if CONFIG_PET_VNEXT_ENROLLMENT && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        usb_wifi_poll();
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(50)) != pdTRUE) continue;
#else
        if (xQueueReceive(s_commands, &command, portMAX_DELAY) != pdTRUE) continue;
#endif
        if (command.scan) {
            esp_err_t err = pet_network_scan(scan_finished);
            if (err != ESP_OK) scan_finished(NULL, 0, err);
        } else {
            char remembered[PET_PASSWORD_MAX] = {0};
            const char *password = command.password;
            if (!password[0] && pet_config_recall_wifi(command.ssid, remembered, sizeof(remembered)) == ESP_OK)
                password = remembered;
            esp_err_t err = pet_config_store_wifi(command.ssid, password);
            pet_enrollment_clear(remembered, sizeof(remembered));
            pet_enrollment_clear(&command, sizeof(command));
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(400));
                esp_restart();
            }
            if (pet_display_lock(-1) == ESP_OK) {
                status_text("WIFI NOT SAVED - RETRY");
                pet_display_unlock();
            }
        }
        pet_enrollment_clear(&command, sizeof(command));
    }
}

esp_err_t pet_onboarding_start(const pet_config_t *config, const char *https_origin)
{
    if (!config || s_screen) return ESP_ERR_INVALID_ARG;
    s_commands = xQueueCreate(2, sizeof(command_t));
    if (!s_commands) return ESP_ERR_NO_MEM;
    lv_display_t *display = pet_display_start();
    if (!display) return ESP_FAIL;
    s_width = lv_display_get_horizontal_resolution(display);
    s_height = lv_display_get_vertical_resolution(display);
    if (pet_display_lock(-1) != ESP_OK) return ESP_FAIL;
    if (pet_touch_enable_resilient() != ESP_OK) {
        pet_display_unlock();
        return ESP_FAIL;
    }
    bsp_display_brightness_set(config->brightness);
    strlcpy(s_device_status.ssid, config->wifi_ssid,
            sizeof(s_device_status.ssid));
    s_device_status.wifi = config->wifi_ssid[0] ?
        PET_ONBOARDING_WIFI_CONNECTING : PET_ONBOARDING_WIFI_DISCONNECTED;
    s_device_status.cloud = PET_ONBOARDING_CLOUD_PAIRING;
    s_device_status.conversation = PET_ONBOARDING_CONVERSATION_UNAVAILABLE;
    s_device_status.installation = PET_ONBOARDING_INSTALL_IDLE;
    s_setting[SETTING_VOLUME] = config->volume <= 100 ? config->volume : 100;
    s_setting[SETTING_BRIGHTNESS] = config->brightness < PET_BRIGHTNESS_MIN ? PET_BRIGHTNESS_MIN :
        config->brightness > 100 ? 100 : config->brightness;
    s_display_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_display_screen);
    lv_obj_set_style_bg_color(s_display_screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s_display_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_display_screen, LV_OBJ_FLAG_SCROLLABLE);
    s_screen = lv_obj_create(s_display_screen);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, s_width, s_height);
    lv_obj_center(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(BG), 0);
    lv_obj_set_style_text_font(s_screen, FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_screen, lv_color_hex(LIME), 0);
    /* Preserve screen-like hit testing: a press in a gap can enter a list. */
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_screen, gesture, LV_EVENT_GESTURE, NULL);
    s_type_timer = lv_timer_create(type_next, 24, NULL);
    s_tick_timer = lv_timer_create(tick, 130, NULL);
    s_status_timer = lv_timer_create(status_rest, 3500, NULL);
    s_setting_timer = lv_timer_create(settings_keep, 900, NULL);
    s_splash_timer = lv_timer_create(splash_tick, 100, NULL);
    lv_timer_t *poll = lv_timer_create(poll_connection, 500, NULL);
    if (!s_type_timer || !s_tick_timer || !s_status_timer || !s_setting_timer || !s_splash_timer || !poll) {
        pet_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(s_type_timer);
    lv_timer_pause(s_status_timer);
    lv_timer_pause(s_setting_timer);
    s_boot_tick = lv_tick_get();
    show_page(PAGE_BOOT, true);
    lv_screen_load(s_display_screen);
    crt_on();
    pet_display_unlock();
    esp_err_t err = pet_network_start_wifi_only(config);
    if (err != ESP_OK) return err;
#if CONFIG_PET_VNEXT_ENROLLMENT && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usb = {.rx_buffer_size = 2048, .tx_buffer_size = 2048};
    s_usb_wifi_ready = usb_serial_jtag_is_driver_installed() || usb_serial_jtag_driver_install(&usb) == ESP_OK;
#endif
    if (xTaskCreate(command_task, "pet_wifi_setup", 4096, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    /* UI may run now; synchronize the availability flag under the LVGL lock. */
    pet_setup_on_link(link_changed);
    err = pet_setup_start(https_origin, setup_status);
    if (pet_display_lock(-1) == ESP_OK) {
        s_setup_available = err == ESP_OK;
        pet_display_unlock();
    }
    return ESP_OK; /* Wi-Fi wizard remains usable without a cloud deployment. */
}

void pet_onboarding_bind_control(const pet_onboarding_control_t *control)
{if(control&&!s_screen)s_control=*control;}

void pet_onboarding_set_library(const pet_control_library_t *library)
{
    if(!library||library->count>PET_CONTROL_LIBRARY_MAX||!s_screen||pet_display_lock(-1)!=ESP_OK)return;
    s_library=*library;s_selected_pet=0;
    /* A late response must not dismiss Wi-Fi/password entry. */
    if(s_page==PAGE_LIBRARY||s_page==PAGE_CONFIRM){show(PAGE_LIBRARY);lv_screen_load(s_display_screen);}
    pet_display_unlock();
}

void pet_onboarding_update_status(const pet_onboarding_status_t *status)
{
    if(!status||!s_screen||
       (unsigned)status->wifi>PET_ONBOARDING_WIFI_CONNECTED||
       (unsigned)status->cloud>PET_ONBOARDING_CLOUD_ATTENTION||
       (unsigned)status->conversation>PET_ONBOARDING_CONVERSATION_ERROR||
       (unsigned)status->installation>PET_ONBOARDING_INSTALL_FAILED||
       status->installation_percent>100||
       status->pet_count>status->pet_capacity||status->pet_index>status->pet_count||
       (status->volume_valid&&status->volume>100)||
       (status->brightness_valid&&(status->brightness<PET_BRIGHTNESS_MIN||status->brightness>100))||
       !memchr(status->ssid,0,sizeof(status->ssid))||
       !memchr(status->pack_name,0,sizeof(status->pack_name))||
       !memchr(status->pack_version,0,sizeof(status->pack_version))||
       !memchr(status->build_id,0,sizeof(status->build_id))||
       !memchr(status->firmware_version,0,sizeof(status->firmware_version))||
       !memchr(status->support_error_code,0,sizeof(status->support_error_code))||
       pet_display_lock(-1)!=ESP_OK)return;
    s_device_status=*status;
    s_status_known=true;
    if(status->has_pet)s_paired=true;
    if(status->cloud!=PET_ONBOARDING_CLOUD_PAIRING)s_paired=s_enrolled=true;
    /* The owner's own change wins until it is kept. */
    if(status->volume_valid&&!s_setting_unsaved[SETTING_VOLUME]&&status->volume!=s_setting[SETTING_VOLUME]){
        s_setting[SETTING_VOLUME]=status->volume;setting_show(SETTING_VOLUME);
    }
    if(status->brightness_valid&&!s_setting_unsaved[SETTING_BRIGHTNESS]&&status->brightness!=s_setting[SETTING_BRIGHTNESS]){
        s_setting[SETTING_BRIGHTNESS]=status->brightness;setting_show(SETTING_BRIGHTNESS);
    }
    render_status();
    pet_display_unlock();
}

bool pet_onboarding_setup_ready(void)
{return s_screen&&s_setup_available;}
bool pet_onboarding_ui_ready(void)
{return atomic_load(&s_ui_visible_live)&&(uint32_t)(lv_tick_get()-atomic_load(&s_ui_last_tick))<2000;}

void pet_onboarding_open_from_ui(void)
{
    if(!s_screen)return;
    const bool question=s_link.phase==PET_LINK_CLAIMED||s_link.phase==PET_LINK_LINKING;
    show_page(question?PAGE_LINK:s_paired?PAGE_HOME:PAGE_WIFI,true);
    lv_screen_load(s_display_screen);
    crt_on();
}
