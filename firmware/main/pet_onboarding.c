#include "pet_onboarding.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "bsp/esp-bsp.h"
#include "pet_display.h"
#include "pet_board.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pet_menu_assets.h"
#include "pet_network.h"
#include "pet_session_wire.h"
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
 * past the end of the log). No pack, frame_player or pet_face dependency.
 *
 * Every page is built from the theme: its colours by role, its type sizes,
 * and how it draws cards, titles, paper and the story frame. The owner picks
 * the theme on the deck's STYLE card; the device keeps it (menu_style). */
typedef enum {
    GLASS,  /* the glass behind everything */
    FACE,   /* card faces */
    UNLIT,  /* unlit segments */
    EDGE,   /* card edges, rules and corner marks */
    DIM,    /* secondary text */
    LABEL,  /* card labels */
    VALUE,  /* values */
    LIT,    /* lit segments, and a lit or pressed face */
    ON_LIT, /* text and icons on a lit face */
    GOOD,   /* all is well: connected, charged, ready, healthy */
    WARN,   /* needs a look */
    FAIL,   /* failed */
    ROLES,
} role_t;
/* A theme colours each role twice: on the glass and its cards, and on paper,
 * under lists, logs and text. An object flagged PAPER, and all it holds, is
 * on paper. */
typedef enum { ON_GLASS, ON_PAPER, SURFACES } surface_t;
#define PAPER LV_OBJ_FLAG_USER_1
/* The 360px layout's type sizes. A screen under 300px wide (the 1.54's 240)
 * takes each one size down. */
typedef enum { TYPE_SMALL, TYPE_TEXT, TYPE_TITLE, TYPE_BIG, TYPES } type_t;
typedef struct {
    const char *name;
    uint32_t color[SURFACES][ROLES];
    const lv_font_t *font[TYPES][2]; /* [size][a screen under 300px wide] */
    lv_event_cb_t draw_card;         /* LV_EVENT_DRAW_MAIN of a card, its card_style_t as user data */
    void (*title)(lv_obj_t *parent, const char *name, int y);
    lv_event_cb_t draw_paper;        /* LV_EVENT_DRAW_MAIN of paper: a sheet `paper_margin` px around it; or none */
    int32_t paper_margin;
    lv_event_cb_t draw_stat;         /* LV_EVENT_DRAW_MAIN of a stat's bar, its segments_t as user data */
    lv_event_cb_t draw_frame;        /* LV_EVENT_DRAW_MAIN of the story frame over the pet */
    void (*frame_tab)(lv_obj_t *frame);
    const char *credits[2];          /* the system log's credit for its faces */
    int32_t tracking;                /* added to spaced text's letter spacing */
} theme_t;
static void draw_card(lv_event_t *event);
static void tab_title(lv_obj_t *parent, const char *name, int y);
static void draw_segments(lv_event_t *event);
static void draw_story_frame(lv_event_t *event);
static void story_tab(lv_obj_t *frame);
/* TERMINAL: a HUD of lime on near-black in Share Tech Mono. Cards have two
 * corners cut, titles sit on a lit tab, and the story frame is a lime rim.
 * It has no paper, so it colours paper as glass. */
#define TERMINAL_COLORS {                                                            \
        [GLASS] = 0x0a0e07, [FACE] = 0x141b0e, [UNLIT] = 0x28331e, [EDGE] = 0x587a3c, \
        [DIM] = 0x86a566, [LABEL] = 0xb4d68b, [VALUE] = 0xc8ef8f, [LIT] = 0xc8ef8f,   \
        [ON_LIT] = 0x0c1108, [GOOD] = 0xc8ef8f, [WARN] = 0xffc15a, [FAIL] = 0xff6b57, \
    }
static const theme_t s_terminal = {
    .name = "TERMINAL",
    .color = {TERMINAL_COLORS, TERMINAL_COLORS},
    .font = {
        [TYPE_SMALL] = {&pet_menu_font_12, &pet_menu_font_12},
        [TYPE_TEXT] = {&pet_menu_font_16, &pet_menu_font_12},
        [TYPE_TITLE] = {&pet_menu_font_20, &pet_menu_font_16},
        [TYPE_BIG] = {&pet_menu_font_28, &pet_menu_font_20},
    },
    .draw_card = draw_card,
    .title = tab_title,
    .draw_stat = draw_segments,
    .draw_frame = draw_story_frame,
    .frame_tab = story_tab,
    .credits = {"menu font derived from Share Tech Mono", "(c) 2012 Carrois Type Design, Ralph du Carrois"},
};
#undef TERMINAL_COLORS
static void wood_card(lv_event_t *event);
static void sign_title(lv_obj_t *parent, const char *name, int y);
static void draw_parchment(lv_event_t *event);
static void wood_stat(lv_event_t *event);
static void wood_frame(lv_event_t *event);
static void wood_tab(lv_obj_t *frame);
/* WOODLAND: a fantasy game's wood. Cards are dark plank panels in a bevelled
 * frame with brass rivets, titles hang on signboards, lists, logs and text
 * sit on parchment, and the story frame is carved wood. Jersey for text,
 * Jacquard 12 for titles. */
static const theme_t s_woodland = {
    .name = "WOODLAND",
    .color = {
        [ON_GLASS] = {
            [GLASS] = 0x150d07, [FACE] = 0x4a2e17, [UNLIT] = 0x23150a, [EDGE] = 0x9b6834,
            [DIM] = 0xc0a078, [LABEL] = 0xe0b45c, [VALUE] = 0xf7e6c0, [LIT] = 0xe8a93c,
            [ON_LIT] = 0x2b1709, [GOOD] = 0x9ad45a, [WARN] = 0xff9b45, [FAIL] = 0xff8068,
        },
        [ON_PAPER] = {
            [GLASS] = 0xead6a6, [FACE] = 0xf1e2bb, [UNLIT] = 0xcdb27c, [EDGE] = 0xa27d48,
            [DIM] = 0x6e5132, [LABEL] = 0x5a3a1c, [VALUE] = 0x2e1a0b, [LIT] = 0xe2a63e,
            [ON_LIT] = 0x2b1709, [GOOD] = 0x2f6a1c, [WARN] = 0x8a4a00, [FAIL] = 0x9a2412,
        },
    },
    .font = {
        [TYPE_SMALL] = {&pet_woodland_font_12, &pet_woodland_font_12},
        [TYPE_TEXT] = {&pet_woodland_font_16, &pet_woodland_font_12},
        [TYPE_TITLE] = {&pet_woodland_title, &pet_woodland_title},
        [TYPE_BIG] = {&pet_woodland_font_28, &pet_woodland_font_20},
    },
    .draw_card = wood_card,
    .title = sign_title,
    .draw_paper = draw_parchment,
    .paper_margin = 6,
    .draw_stat = wood_stat,
    .draw_frame = wood_frame,
    .frame_tab = wood_tab,
    .credits = {"fonts derived from Jersey, Jacquard 12", "(c) 2023 The Soft Type Project Authors"},
    /* Jersey's capitals are two pixels wider than Share Tech Mono's: spaced
     * two closer, a label takes the room TERMINAL gives it. */
    .tracking = -2,
};
/* By pet_config_t.menu_style. */
static const theme_t *const s_themes[PET_MENU_STYLES] = {&s_terminal, &s_woodland};
static const theme_t *s_theme = &s_terminal;
#define SMALL_SCREEN (s_width > 0 && s_width < 300)
#define FONT_SMALL (s_theme->font[TYPE_SMALL][SMALL_SCREEN])
#define FONT_TEXT (s_theme->font[TYPE_TEXT][SMALL_SCREEN])
#define FONT_TITLE (s_theme->font[TYPE_TITLE][SMALL_SCREEN])
#define FONT_BIG (s_theme->font[TYPE_BIG][SMALL_SCREEN])
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
    /* The story screens, opened by a swipe on the pet. */
    PAGE_BLESSINGS,
    PAGE_CHARACTER,
    PAGE_STORY,
    /* The deck's list of the owner's stories. */
    PAGE_STORIES,
} page_t;
/* A card's look and touch: plain, pressable, lit, or a button that lights
 * while pressed; and what it is, for a theme that draws them apart: a list's
 * row, a slot such as an inventory cell, or a slot left empty. */
typedef enum {
    CARD_PLAIN = 0,
    CARD_PRESS = 1,
    CARD_FILLED = 2,
    CARD_BUTTON = 4,
    CARD_ROW = 8,
    CARD_SLOT = 16,
    CARD_EMPTY = 32,
} card_style_t;
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
/* The deck's pet picker: a cell for each installed pet, at most three. */
#define PET_PICKER_MAX 3
#define PICKER_CELL (SMALL_SCREEN ? 26 : 38)
#define PICKER_GAP (SMALL_SCREEN ? 3 : 6)
static lv_obj_t *s_pet_cell[PET_PICKER_MAX];
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
/* The story of the session's pet (story-log-v1, story-mode-v1): its log as
 * the menu face draws it, its state and the owner's stories, each with the
 * face ID of its pet, in PSRAM once the first arrives. The network task
 * writes them under s_story_lock, never the display lock; an open story page
 * redraws on the next tick once s_story_fresh is set. `shown` is the list as
 * the STORIES page shows it, which its taps read. */
typedef struct {
    char log_owner[81];
    uint8_t count;
    char lines[PET_STORY_LOG_LINES][PET_STORY_LOG_LINE_BYTES + 1];
    char state_owner[81];
    pet_story_state_t state;
    char list_owner[81];
    pet_story_list_t list, shown;
} story_t;
static story_t *s_story;
static SemaphoreHandle_t s_story_lock;
static atomic_bool s_story_fresh;
/* The deck's STORY card: the current story's title. */
static lv_obj_t *s_story_value;
/* The story frame over the pet, once made: it follows the menu's style. */
static lv_obj_t *s_story_frame;
/* For now, while the story's pacing is tried out: the frame's rim is green
 * while the pet is on its story's path, this yellow on a detour and this red
 * while it hurries back (story-hurry-v1), as the latest story.state's path
 * says (story-pacing-v1). s_frame_detour and s_frame_hurry are how the frame
 * is drawn; the tick draws it again when the path changes. */
#define DETOUR_YELLOW 0xffd400
#define HURRY_RED 0xff3030
static atomic_bool s_story_detour, s_story_hurry;
static bool s_frame_detour, s_frame_hurry;
/* RESTART on the STORIES page was tapped once, at this tick: a second tap
 * within RESTART_CONFIRM_MS starts the story over. */
#define RESTART_CONFIRM_MS 5000
static bool s_restart_armed;
static uint32_t s_restart_armed_at;
/* A story game's blessing is chosen by its id; "" until one is. */
static char s_blessing_id[PET_STORY_BLESSING_ID_BYTES + 1];

static void show(page_t page);
static void show_page(page_t page, bool animate);
static void swipe_up(void);
static void leave_to_pet(void);
static void render_status(void);
static bool current_story(char *title_out, size_t capacity);

/* A role's colour where `obj` is: on paper when it or what holds it is paper. */
static lv_color_t shade(lv_obj_t *obj, role_t role)
{
    surface_t surface = ON_GLASS;
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, PAPER)) {
            surface = ON_PAPER;
            break;
        }
    }
    return lv_color_hex(s_theme->color[surface][role]);
}

#if CONFIG_PET_VNEXT_DEVELOPER_LIBRARY
static void center_pet_list(void *unused)
{
    (void)unused;
    if(!s_library_dropdown||!lv_dropdown_is_open(s_library_dropdown))return;
    lv_obj_t *list=lv_dropdown_get_list(s_library_dropdown);
    lv_obj_set_width(list,240);lv_obj_set_style_max_height(list,180,0);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_obj_center(list);
    lv_obj_set_style_bg_color(list,shade(list, FACE),0);lv_obj_set_style_border_color(list,shade(list, EDGE),0);
    lv_obj_set_style_text_color(list,shade(list, VALUE),0);lv_obj_set_style_text_font(list,FONT_TEXT,0);
    lv_obj_set_style_bg_color(list,shade(list, LIT),LV_PART_SELECTED|LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list,shade(list, ON_LIT),LV_PART_SELECTED|LV_STATE_CHECKED);
}
static void pet_list_opened(lv_event_t *event)
{(void)event;lv_async_call(center_pet_list,NULL);}
#endif

/* Printable ASCII the menu face can draw, optionally in capitals: controls
 * become spaces, accented letters lose their accents and each other
 * character is one '?'. At most `max_chars`. */
static void display_text(char *out, size_t capacity, const char *in, size_t max_chars, bool capitals)
{
    /* The letters of Latin-1, U+00C0 to U+00FF, as in "cafe". */
    static const char latin1[] = "AAAAAAAC" "EEEEIIII" "DNOOOOOx" "OUUUUYTs" "aaaaaaac" "eeeeiiii" "dnooooo/" "ouuuuyty";
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && length + 1 < capacity && length < max_chars; ++p) {
        if ((*p & 0xc0) == 0x80) continue;
        char c = *p >= 0x80 ? '?' : *p < 0x20 || *p == 0x7f ? ' ' : (char)*p;
        unsigned times = 1;
        if (p[0] == 0xc3 && p[1] >= 0x80 && p[1] <= 0xbf) c = latin1[p[1] - 0x80];
        /* Typographic dashes and quotes (U+2013, U+2014, U+2018 to U+201D),
         * as in an account name with a curly apostrophe, read as ASCII. */
        if (p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x93 && p[2] <= 0x94) || (p[2] >= 0x98 && p[2] <= 0x9d)))
            c = p[2] <= 0x94 ? '-' : p[2] <= 0x9b ? '\'' : '"';
        /* An ellipsis (U+2026) is three dots. */
        if (p[0] == 0xe2 && p[1] == 0x80 && p[2] == 0xa6) {
            c = '.';
            times = 3;
        }
        for (; times && length + 1 < capacity && length < max_chars; --times)
            out[length++] = capitals && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
    out[length] = 0;
}

/* The letter spacing a spaced text gets in the theme's face. */
static int32_t tracked(int32_t spacing)
{
    return spacing > 0 ? LV_MAX(0, spacing + s_theme->tracking) : spacing;
}

static int32_t text_width(const lv_font_t *font, const char *text, int32_t spacing)
{
    spacing = tracked(spacing);
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
                      role_t color, int32_t spacing)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_letter_space(obj, tracked(spacing), 0);
    lv_obj_set_style_text_color(obj, shade(obj, color), 0);
    lv_label_set_text(obj, content);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

/* A line centred in `width` across the screen. */
static lv_obj_t *centered(lv_obj_t *parent, const lv_font_t *font, const char *content, int y, int width,
                          role_t color, int32_t spacing)
{
    lv_obj_t *obj = text(parent, font, content, 0, 0, color, spacing);
    /* Widths are the 360px layout's; a smaller screen is the limit. */
    lv_obj_set_width(obj, LV_MIN(width, s_width));
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, parent == s_screen ? page_y(y) : y);
    return obj;
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_image_dsc_t *image, int x, int y, role_t color)
{
    lv_obj_t *obj = lv_image_create(parent);
    lv_image_set_src(obj, image);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_image_recolor(obj, shade(obj, color), 0);
    lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static void set_text(lv_obj_t *label_obj, const char *content, role_t color)
{
    if (!label_obj) return;
    if (strcmp(lv_label_get_text(label_obj), content)) lv_label_set_text(label_obj, content);
    lv_color_t ink = shade(label_obj, color);
    if (!lv_color_eq(lv_obj_get_style_text_color(label_obj, LV_PART_MAIN), ink))
        lv_obj_set_style_text_color(label_obj, ink, 0);
}

static void set_icon_color(lv_obj_t *image, role_t color)
{
    if (!image) return;
    lv_color_t ink = shade(image, color);
    if (!lv_color_eq(lv_obj_get_style_image_recolor(image, LV_PART_MAIN), ink))
        lv_obj_set_style_image_recolor(image, ink, 0);
}

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (!obj || visible != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void triangle(lv_layer_t *layer, lv_color_t color, int32_t ax, int32_t ay, int32_t bx, int32_t by,
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
    shape.color = color;
    shape.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &shape);
}

static void edge(lv_layer_t *layer, lv_color_t color, int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.p1.x = ax;
    line.p1.y = ay;
    line.p2.x = bx;
    line.p2.y = by;
    line.color = color;
    line.width = 1;
    lv_draw_line(layer, &line);
}

/* A solid rectangle, its corners included. */
static void fill_rect(lv_layer_t *layer, lv_color_t color, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_draw_rect_dsc_t look;
    lv_draw_rect_dsc_init(&look);
    look.bg_color = color;
    const lv_area_t area = {x1, y1, x2, y2};
    lv_draw_rect(layer, &look, &area);
}

/* A border `width` px wide just inside `a`. */
static void border(lv_layer_t *layer, lv_color_t color, const lv_area_t *a, int32_t width)
{
    lv_draw_rect_dsc_t look;
    lv_draw_rect_dsc_init(&look);
    look.bg_opa = LV_OPA_TRANSP;
    look.border_color = color;
    look.border_width = width;
    lv_draw_rect(layer, &look, a);
}

/* Whether a card is pressed now, and whether it is lit: filled, chosen
 * (LV_STATE_CHECKED), or a button while pressed. */
static bool card_lit(lv_obj_t *obj, unsigned style, bool *pressed)
{
    *pressed = (style & (CARD_PRESS | CARD_BUTTON)) && lv_obj_has_state(obj, LV_STATE_PRESSED);
    return (style & CARD_FILLED) || lv_obj_has_state(obj, LV_STATE_CHECKED) || (*pressed && (style & CARD_BUTTON));
}

/* TERMINAL's card: its top-left and bottom-right corners cut away, a thin
 * rim, and marks in the other two corners. Pressed, a button lights up lime,
 * and so does a chosen cell. An empty slot is its outline. */
static void draw_card(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    unsigned style = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (style & CARD_EMPTY) {
        border(layer, shade(obj, UNLIT), &a, 1);
        return;
    }
    bool pressed;
    const bool lit = card_lit(obj, style, &pressed);
    const int32_t c = CHAMFER;
    lv_draw_rect_dsc_t face;
    lv_draw_rect_dsc_init(&face);
    face.bg_color = shade(obj, lit ? LIT : FACE);
    lv_draw_rect(layer, &face, &a);
    const lv_color_t glass = shade(obj, GLASS);
    triangle(layer, glass, a.x1, a.y1, a.x1 + c, a.y1, a.x1, a.y1 + c);
    triangle(layer, glass, a.x2 + 1, a.y2 + 1, a.x2 + 1 - c, a.y2 + 1, a.x2 + 1, a.y2 + 1 - c);
    const lv_color_t rim = shade(obj, lit || pressed ? LIT : EDGE);
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
    const lv_color_t ink = shade(button, lv_event_get_code(event) == LV_EVENT_PRESSED ? ON_LIT : VALUE);
    for (uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_t *child = lv_obj_get_child(button, (int32_t)i);
        if (lv_obj_check_type(child, &lv_image_class)) lv_obj_set_style_image_recolor(child, ink, 0);
        else lv_obj_set_style_text_color(child, ink, 0);
    }
}

/* A chosen cell lights up, its contents dark on lime. */
static void set_lit(lv_obj_t *cell, bool lit)
{
    if (!cell || lit == lv_obj_has_state(cell, LV_STATE_CHECKED)) return;
    if (lit) lv_obj_add_state(cell, LV_STATE_CHECKED);
    else lv_obj_remove_state(cell, LV_STATE_CHECKED);
    /* No style follows the state, so LVGL would repaint only the contents. */
    lv_obj_invalidate(cell);
    const lv_color_t ink = shade(cell, lit ? ON_LIT : VALUE);
    for (uint32_t i = 0; i < lv_obj_get_child_count(cell); ++i) {
        lv_obj_t *child = lv_obj_get_child(cell, (int32_t)i);
        if (lv_obj_check_type(child, &lv_image_class)) lv_obj_set_style_image_recolor(child, ink, 0);
        else lv_obj_set_style_text_color(child, ink, 0);
    }
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int width, int height, card_style_t style)
{
    lv_obj_t *obj = plain(parent);
    page_box(obj, x, y, width, height);
    lv_obj_add_event_cb(obj, s_theme->draw_card, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)style);
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
    if (image) icon(button, image, left, (height - (int32_t)image->header.h) / 2, VALUE);
    lv_obj_t *caption = text(button, FONT_TEXT, name, left + total - words, (height - 19) / 2, VALUE, 2);
    (void)caption;
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    return button;
}

/* A bar of cells, lit up to its value: framed for the sliders. A stat's
 * bar says whether it is health, which a theme may draw as hearts. A bar of
 * `stops` has a cell for each stop, lit up to the one its value is in. */
typedef struct { uint8_t count; bool framed; bool battery; bool hearts; bool stops; } segments_t;
static const segments_t s_slider_segments = {10, true, false, false, false};
static const segments_t s_progress_segments = {20, false, false, false, false};

/* How many of `count` cells are lit at `value`. */
static int32_t lit_cells(int32_t value, int32_t minimum, int32_t maximum, int32_t count)
{
    return maximum > minimum ? ((value - minimum) * count + (maximum - minimum) / 2) / (maximum - minimum) : 0;
}

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
        frame.border_color = shade(obj, EDGE);
        frame.border_width = 1;
        frame.radius = 3;
        lv_draw_rect(layer, &frame, &box);
        lv_area_increase(&box, -4, -4);
    }
    int32_t span = lv_area_get_width(&box), gap = look->framed ? 3 : 2;
    int32_t lit = lit_cells(value, minimum, maximum, look->count);
    if (look->stops && maximum > minimum)
        lit = LV_MIN(look->count, (value - minimum) * look->count / (maximum - minimum) + 1);
    const lv_color_t on = shade(obj, LIT), off = shade(obj, UNLIT);
    lv_draw_rect_dsc_t cell;
    lv_draw_rect_dsc_init(&cell);
    cell.radius = 1;
    for (int32_t i = 0; i < look->count; ++i) {
        lv_area_t area = box;
        area.x1 = box.x1 + i * (span + gap) / look->count;
        area.x2 = box.x1 + (i + 1) * (span + gap) / look->count - gap - 1;
        cell.bg_color = i < lit ? on : off;
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
    const lv_color_t color = shade(obj, !s->battery_valid ? DIM : percent <= 15 ? FAIL : percent <= 25 ? WARN : GOOD);
    lv_draw_rect_dsc_t shape;
    lv_draw_rect_dsc_init(&shape);
    lv_area_t body = a, nub = a, fill;
    body.x2 -= 4;
    nub.x1 = body.x2 + 1;
    nub.y1 += 5;
    nub.y2 -= 5;
    shape.bg_color = color;
    shape.radius = 1;
    lv_draw_rect(layer, &shape, &nub);
    shape.bg_opa = LV_OPA_TRANSP;
    shape.border_color = color;
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
    shape.bg_color = color;
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
        bar_look.bg_color = shade(obj, i < level ? VALUE : UNLIT);
        lv_draw_rect(lv_event_get_layer(event), &bar_look, &bar);
    }
}

/* ---- WOODLAND -------------------------------------------------------------
 * Wood, gold and brass beyond the colours of its roles. */
enum {
    WOOD_OUTLINE = 0x120a05,
    WOOD_LIGHT = 0xd29b5b,  /* a frame's lit bevel, at its top and left */
    WOOD_FRAME = 0x9b6834,
    WOOD_SHADE = 0x5e3a1c,  /* its shaded bevel, at its bottom and right */
    WOOD_INSET = 0x1d1109,  /* the groove between a frame and its planks */
    WOOD_HOLLOW = 0x3d2513, /* inside a slot */
    GOLD_LIGHT = 0xffe1a0,
    GOLD_SHADE = 0x9a6516,
    BRASS = 0xc99a3e,
    BRASS_LIGHT = 0xffeaa8,
    BRASS_SHADE = 0x6e4c1c,
    PAPER_EDGE = 0x5a3a1c,  /* parchment's outline */
    PAPER_AGED = 0xd6bc84,  /* its darker rim, and a roll's middle */
    PAPER_LIGHT = 0xf6ead0, /* a roll's lit top */
    PAPER_SHADE = 0xb48f55, /* a roll's shaded underside */
};

/* An image tiled over `a` from its top-left corner. */
static void tiles(lv_layer_t *layer, const lv_image_dsc_t *image, const lv_area_t *a)
{
    lv_draw_image_dsc_t look;
    lv_draw_image_dsc_init(&look);
    look.src = image;
    look.tile = 1;
    lv_draw_image(layer, &look, a);
}

/* A frame `width` px wide just inside `a`, lit from the top left: its top
 * and left edges `light`, its bottom and right edges `dark`. */
static void bevel(lv_layer_t *layer, const lv_area_t *a, int32_t width, lv_color_t face, lv_color_t light,
                  lv_color_t dark)
{
    border(layer, face, a, width);
    fill_rect(layer, light, a->x1, a->y1, a->x2 - 1, a->y1);
    fill_rect(layer, light, a->x1, a->y1, a->x1, a->y2 - 1);
    fill_rect(layer, dark, a->x1 + 1, a->y2, a->x2, a->y2);
    fill_rect(layer, dark, a->x2, a->y1 + 1, a->x2, a->y2);
}

/* A brass rivet, 3 px square, domed toward the light. */
static void rivet(lv_layer_t *layer, int32_t x, int32_t y)
{
    fill_rect(layer, lv_color_hex(BRASS_SHADE), x, y, x + 2, y + 2);
    fill_rect(layer, lv_color_hex(BRASS), x, y, x + 1, y + 1);
    fill_rect(layer, lv_color_hex(BRASS_LIGHT), x, y, x, y);
}

/* A panel of dark planks in a bevelled wooden frame, a rivet in each corner.
 * Lit, its planks are gold; pressed, its frame is. */
static void wood_panel(lv_layer_t *layer, lv_obj_t *obj, const lv_area_t *a, bool lit, bool pressed)
{
    const bool gold = lit || pressed;
    lv_area_t frame = *a, inner = *a;
    lv_area_increase(&frame, -1, -1);
    lv_area_increase(&inner, -4, -4);
    border(layer, lv_color_hex(WOOD_OUTLINE), a, 1);
    bevel(layer, &frame, 3, gold ? shade(obj, LIT) : lv_color_hex(WOOD_FRAME),
          lv_color_hex(gold ? GOLD_LIGHT : WOOD_LIGHT), lv_color_hex(gold ? GOLD_SHADE : WOOD_SHADE));
    border(layer, lv_color_hex(WOOD_INSET), &inner, 1);
    lv_area_increase(&inner, -1, -1);
    if (lit) {
        fill_rect(layer, shade(obj, LIT), inner.x1, inner.y1, inner.x2, inner.y2);
        fill_rect(layer, lv_color_hex(GOLD_LIGHT), inner.x1, inner.y1, inner.x2, inner.y1);
    } else tiles(layer, &pet_wood_planks, &inner);
    rivet(layer, frame.x1, frame.y1);
    rivet(layer, frame.x2 - 2, frame.y1);
    rivet(layer, frame.x1, frame.y2 - 2);
    rivet(layer, frame.x2 - 2, frame.y2 - 2);
}

/* A slot sunk into the wood, shaded at its top left, hollow inside; chosen,
 * it is gold. */
static void wood_slot(lv_layer_t *layer, lv_obj_t *obj, const lv_area_t *a, bool lit, bool pressed)
{
    const bool gold = lit || pressed;
    lv_area_t frame = *a;
    lv_area_increase(&frame, -1, -1);
    border(layer, lv_color_hex(WOOD_OUTLINE), a, 1);
    bevel(layer, &frame, 2, gold ? shade(obj, LIT) : lv_color_hex(WOOD_FRAME),
          lv_color_hex(gold ? GOLD_SHADE : WOOD_SHADE), lv_color_hex(gold ? GOLD_LIGHT : WOOD_LIGHT));
    lv_area_increase(&frame, -2, -2);
    fill_rect(layer, lit ? shade(obj, LIT) : lv_color_hex(WOOD_HOLLOW), frame.x1, frame.y1, frame.x2, frame.y2);
}

/* A list's row on paper: a rule under it; chosen or pressed, a gold bar. */
static void paper_row(lv_layer_t *layer, lv_obj_t *obj, const lv_area_t *a, bool lit)
{
    if (!lit) {
        fill_rect(layer, shade(obj, EDGE), a->x1 + 6, a->y2, a->x2 - 6, a->y2);
        return;
    }
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = shade(obj, LIT);
    bar.border_color = lv_color_hex(GOLD_SHADE);
    bar.border_width = 1;
    bar.radius = 3;
    lv_draw_rect(layer, &bar, a);
}

/* WOODLAND's card: a plank panel, a slot or a row on paper. */
static void wood_card(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    const unsigned style = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    bool pressed;
    const bool lit = card_lit(obj, style, &pressed);
    if (style & CARD_ROW) paper_row(layer, obj, &a, lit || pressed);
    else if (style & (CARD_SLOT | CARD_EMPTY)) wood_slot(layer, obj, &a, lit, pressed);
    else wood_panel(layer, obj, &a, lit, pressed);
}

/* A rope, twisted, from the top of the glass down to `bottom`. */
static void draw_rope(lv_event_t *event)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &a);
    tiles(lv_event_get_layer(event), &pet_wood_rope, &a);
}

static void rope(lv_obj_t *parent, int32_t x, int32_t bottom)
{
    const int32_t width = (int32_t)pet_wood_rope.header.w;
    int32_t top = 0;
    if (pet_board_current()->round) {
        /* Where the round glass's edge is above its outer side. */
        const int32_t r = s_width / 2, side = LV_MAX(LV_ABS(x - r), LV_ABS(x + width - 1 - r));
        top = r - (int32_t)sqrtf((float)(r * r - side * side)) + 1;
    }
    lv_obj_t *cord = plain(parent);
    lv_obj_set_pos(cord, x, top);
    lv_obj_set_size(cord, width, bottom - top);
    lv_obj_add_event_cb(cord, draw_rope, LV_EVENT_DRAW_MAIN, NULL);
    reveal(cord);
}

/* "WI-FI" as "Wi-Fi": blackletter capitals read only one at a time. */
static void title_case(char *out, size_t capacity, const char *in)
{
    size_t length = 0;
    bool first = true;
    for (; *in && length + 1 < capacity; ++in) {
        const char c = *in;
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        out[length++] = letter && !first && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        first = !letter;
    }
    out[length] = 0;
}

/* WOODLAND's title: in gold blackletter on a signboard hung from the top of
 * the page by two ropes. The small screen's board is shorter and hangs
 * higher, clear of the status line. */
static void sign_title(lv_obj_t *parent, const char *name, int y)
{
    char words[24];
    title_case(words, sizeof(words), name);
    const int32_t width = text_width(FONT_TITLE, words, 1) + 28, height = SMALL_SCREEN ? 26 : 30;
    const int32_t left = (s_width - width) / 2;
    const int32_t top = (parent == s_screen ? page_y(y) : y) - (SMALL_SCREEN ? 6 : 0);
    rope(parent, left + 8, top + 2);
    rope(parent, left + width - 8 - (int32_t)pet_wood_rope.header.w, top + 2);
    lv_obj_t *board = card(parent, left, top, width, height, CARD_PLAIN);
    lv_obj_set_pos(board, left, top);
    lv_obj_set_size(board, width, height);
    const int32_t line = lv_font_get_line_height(FONT_TITLE), base = FONT_TITLE->base_line;
    /* Its capitals, 12 px tall above the baseline, centred on the board. */
    text(board, FONT_TITLE, words, 14, height / 2 + 6 - (line - base), LIT, 1);
}

/* How far paper's sheet, and a scroll's rolls, reach beyond what it holds:
 * room a page leaves around it. */
#define ROLL 6
static int32_t paper_reach(bool rolled)
{
    return s_theme->draw_paper ? s_theme->paper_margin + (rolled ? ROLL : 0) : 0;
}

static void paper_margin(lv_event_t *event)
{
    lv_event_set_ext_draw_size(event, paper_reach((bool)(uintptr_t)lv_event_get_user_data(event)));
}

/* A scroll's wooden knob, 4 px wide from `x`, centred on `y`. */
static void knob(lv_layer_t *layer, int32_t x, int32_t y)
{
    fill_rect(layer, lv_color_hex(WOOD_OUTLINE), x, y - 3, x + 3, y + 3);
    fill_rect(layer, lv_color_hex(WOOD_FRAME), x + 1, y - 2, x + 2, y + 2);
}

/* A rolled end of a scroll across `sheet` at `y`: a cylinder of parchment
 * with a wooden knob at each end. */
static void roll(lv_layer_t *layer, const lv_area_t *sheet, int32_t y)
{
    const int32_t x1 = sheet->x1 - 3, x2 = sheet->x2 + 3;
    fill_rect(layer, lv_color_hex(PAPER_EDGE), x1, y - 4, x2, y + 4);
    fill_rect(layer, lv_color_hex(PAPER_LIGHT), x1 + 1, y - 3, x2 - 1, y - 2);
    fill_rect(layer, lv_color_hex(PAPER_AGED), x1 + 1, y - 1, x2 - 1, y + 1);
    fill_rect(layer, lv_color_hex(PAPER_SHADE), x1 + 1, y + 2, x2 - 1, y + 3);
    knob(layer, x1 - 4, y);
    knob(layer, x2 + 1, y);
}

/* WOODLAND's paper: a sheet of parchment, aged at its rim, `paper_margin` px
 * beyond what it holds; a scroll's is rolled at both ends. */
static void draw_parchment(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t sheet;
    lv_obj_get_coords(obj, &sheet);
    lv_area_increase(&sheet, s_theme->paper_margin, s_theme->paper_margin);
    lv_draw_rect_dsc_t look;
    lv_draw_rect_dsc_init(&look);
    look.bg_color = shade(obj, GLASS);
    look.border_color = lv_color_hex(PAPER_EDGE);
    look.border_width = 1;
    look.radius = 2;
    lv_draw_rect(layer, &look, &sheet);
    lv_area_t rim = sheet;
    lv_area_increase(&rim, -1, -1);
    border(layer, lv_color_hex(PAPER_AGED), &rim, 2);
    if (!lv_event_get_user_data(event)) return;
    roll(layer, &sheet, sheet.y1);
    roll(layer, &sheet, sheet.y2);
}

/* Puts `obj` on paper: what it holds takes paper's colours, and the theme's
 * sheet, rolled at both ends for a scroll, fills the box the page gave it,
 * with what it holds inside its margin. Call it once the box is set, before
 * its contents are sized. */
static void paper(lv_obj_t *obj, bool rolled)
{
    lv_obj_add_flag(obj, PAPER);
    const int32_t reach = paper_reach(rolled);
    if (!reach) return;
    lv_obj_set_pos(obj, lv_obj_get_style_x(obj, 0) + reach, lv_obj_get_style_y(obj, 0) + reach);
    lv_obj_set_size(obj, lv_obj_get_style_width(obj, 0) - 2 * reach, lv_obj_get_style_height(obj, 0) - 2 * reach);
    lv_obj_add_event_cb(obj, paper_margin, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *)(uintptr_t)rolled);
    lv_obj_add_event_cb(obj, s_theme->draw_paper, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)rolled);
    lv_obj_refresh_ext_draw_size(obj);
}

/* A heart of pixel art at `scale` from (x, y): 7 by 6, or 5 by 4 when small. */
static void heart(lv_layer_t *layer, lv_color_t color, int32_t x, int32_t y, int32_t scale, bool small)
{
    static const uint8_t large[] = {0x36, 0x7f, 0x7f, 0x3e, 0x1c, 0x08}, little[] = {0x0a, 0x1f, 0x0e, 0x04};
    const uint8_t *rows = small ? little : large;
    const int32_t width = small ? 5 : 7, height = small ? 4 : 6;
    for (int32_t row = 0; row < height; ++row) {
        for (int32_t from = 0; from < width; ++from) {
            if (!(rows[row] >> (width - 1 - from) & 1)) continue;
            int32_t to = from;
            while (to + 1 < width && rows[row] >> (width - 2 - to) & 1) ++to;
            fill_rect(layer, color, x + from * scale, y + row * scale, x + (to + 1) * scale - 1,
                      y + (row + 1) * scale - 1);
            from = to;
        }
    }
}

/* Health: a heart for each cell, lit green up to the value. */
static void hearts(lv_layer_t *layer, lv_obj_t *bar, const lv_area_t *box, int32_t lit, int32_t count)
{
    const int32_t pitch = lv_area_get_width(box) / count, height = lv_area_get_height(box);
    const bool small = height < 6 || pitch < 9;
    const int32_t scale = small ? 1 : LV_MAX(1, LV_MIN(height / 6, (pitch - 2) / 7));
    const int32_t tall = (small ? 4 : 6) * scale, top = box->y1 + (height - tall) / 2;
    const lv_color_t on = shade(bar, GOOD), off = shade(bar, UNLIT);
    for (int32_t i = 0; i < count; ++i)
        heart(layer, i < lit ? on : off, box->x1 + i * pitch, top, scale, small);
}

/* A potion on its side, gold up to the value, a cork in its mouth. */
static void potion(lv_layer_t *layer, lv_obj_t *bar, const lv_area_t *box, int32_t value, int32_t maximum)
{
    const int32_t height = lv_area_get_height(box), cork = LV_MAX(3, height / 2);
    lv_area_t glass = *box;
    glass.x2 -= cork;
    lv_draw_rect_dsc_t look;
    lv_draw_rect_dsc_init(&look);
    look.bg_color = shade(bar, UNLIT);
    look.border_color = lv_color_hex(GOLD_LIGHT);
    look.border_width = 1;
    look.radius = height / 2;
    lv_draw_rect(layer, &look, &glass);
    fill_rect(layer, lv_color_hex(WOOD_OUTLINE), glass.x2 + 1, box->y1 + height / 4, box->x2,
              box->y2 - height / 4);
    fill_rect(layer, lv_color_hex(WOOD_LIGHT), glass.x2 + 1, box->y1 + height / 4 + 1, box->x2 - 1,
              box->y2 - height / 4 - 1);
    lv_area_t liquid = glass;
    lv_area_increase(&liquid, -2, -2);
    const int32_t level = maximum > 0 ? lv_area_get_width(&liquid) * value / maximum : 0;
    if (level < 1 || lv_area_get_height(&liquid) < 1) return;
    liquid.x2 = liquid.x1 + level - 1;
    lv_draw_rect_dsc_init(&look);
    look.bg_color = shade(bar, LIT);
    look.radius = lv_area_get_height(&liquid) / 2;
    lv_draw_rect(layer, &look, &liquid);
    if (level > 4) fill_rect(layer, lv_color_hex(GOLD_LIGHT), liquid.x1 + 2, liquid.y1, liquid.x2 - 2, liquid.y1);
}

/* WOODLAND's stats: health as a row of hearts, the others as potions. */
static void wood_stat(lv_event_t *event)
{
    lv_obj_t *bar = lv_event_get_current_target_obj(event);
    const segments_t *look = lv_event_get_user_data(event);
    lv_area_t box;
    lv_obj_get_coords(bar, &box);
    const int32_t value = lv_bar_get_value(bar), maximum = lv_bar_get_max_value(bar);
    if (look->hearts)
        hearts(lv_event_get_layer(event), bar, &box, lit_cells(value, 0, maximum, look->count), look->count);
    else potion(lv_event_get_layer(event), bar, &box, value, maximum);
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

static void type_status(const char *content, role_t color, bool typed)
{
    if (!s_status_label) return;
    if (!typed && !strcmp(content, s_typing)) {
        /* The same line, still typing or already there: only its colour. */
        lv_obj_set_style_text_color(s_status_label, shade(s_status_label, color), 0);
        if (s_status_cursor) lv_obj_set_style_bg_color(s_status_cursor, shade(s_status_cursor, color), 0);
        return;
    }
    strlcpy(s_typing, content, sizeof(s_typing));
    s_typing_length = (uint8_t)strlen(s_typing);
    s_typing_shown = typed ? 0 : s_typing_length;
    char shown[sizeof(s_typing)];
    memcpy(shown, s_typing, s_typing_shown);
    shown[s_typing_shown] = 0;
    set_text(s_status_label, shown, color);
    if (s_status_cursor) lv_obj_set_style_bg_color(s_status_cursor, shade(s_status_cursor, color), 0);
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
static const char *deck_summary(char *out, size_t capacity, role_t *color)
{
    const pet_onboarding_status_t *s = &s_device_status;
    *color = VALUE;
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
        *color = FAIL;
        return "NEEDS ATTENTION";
    }
    *color = WARN;
    if (s->wifi != PET_ONBOARDING_WIFI_CONNECTED) return s->ssid[0] ? "WIFI OFFLINE" : "NO WIFI SET UP";
    if (s->cloud == PET_ONBOARDING_CLOUD_PAIRING) return "NOT PAIRED";
    if (s->retry_seconds) {
        snprintf(out, capacity, "RETRY IN %uS", s->retry_seconds);
        return out;
    }
    *color = GOOD;
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
        role_t color;
        const char *line = deck_summary(summary, sizeof(summary), &color);
        type_status(line, color, false);
    }
}

static void status_text(const char *message)
{
    if (!s_status_label) return;
    s_message = true;
    type_status(message, VALUE, false);
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
    lv_obj_set_style_bg_color(s_status_cursor, shade(s_status_cursor, LABEL), 0);
    /* Centred before it types: an empty line keeps its cursor mid-glass. */
    place_status("");
    reveal(s_status_label);
    if (hint) type_status(hint, LABEL, s_animate);
    else {
        char summary[40];
        role_t color;
        const char *line = deck_summary(summary, sizeof(summary), &color);
        type_status(line, color, s_animate);
    }
}

/* The page's title on a lit tab, three slashes after it. */
static void tab_title(lv_obj_t *parent, const char *name, int y)
{
    int32_t words = text_width(FONT_TITLE, name, 3);
    int32_t width = words + 30, total = width + 8 + 3 * 7;
    int32_t left = (s_width - total) / 2;
    /* The small screen's shorter tab clears the status line below it. */
    int32_t height = SMALL_SCREEN ? 22 : 28;
    lv_obj_t *tab = card(parent, left, y, width, height, CARD_FILLED);
    lv_obj_set_x(tab, left);
    lv_obj_set_size(tab, width, height);
    text(tab, FONT_TITLE, name, 15, SMALL_SCREEN ? 2 : 3, ON_LIT, 3);
    reveal(text(parent, FONT_SMALL, "///", left + width + 8,
                (parent == s_screen ? page_y(y) : y) + (SMALL_SCREEN ? 6 : 9), LIT, 1));
}

/* The page's title, as the theme draws it. */
static void title(lv_obj_t *parent, const char *name, int y)
{
    s_theme->title(parent, name, y);
}

static bool story_screen(page_t page)
{
    return page == PAGE_BLESSINGS || page == PAGE_CHARACTER || page == PAGE_STORY;
}

/* The chevron at the foot of a page: swiping up from it, or tapping it,
 * goes back. A story screen's chevron points the way it closes, and the
 * STORY screen's is at the top. */
static void handle_clicked(lv_event_t *event)
{
    (void)event;
    if (story_screen(s_page)) leave_to_pet();
    else swipe_up();
}
static void handle_at(const lv_image_dsc_t *mark, bool top)
{
    lv_obj_t *zone = plain(s_screen);
    /* At the top, inside the round glass, above and under the title. */
    lv_obj_set_pos(zone, top ? (s_width - 100) / 2 : 0, top ? page_y(8) : s_height - 56);
    lv_obj_set_size(zone, top ? 100 : s_width, top ? 44 : 56);
    if (!top) {
        /* Content fades out under the chevron as it scrolls. */
        lv_obj_set_style_bg_opa(zone, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(zone, shade(zone, GLASS), 0);
        lv_obj_set_style_bg_grad_color(zone, shade(zone, GLASS), 0);
        lv_obj_set_style_bg_grad_dir(zone, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_opa(zone, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(zone, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_stop(zone, 0, 0);
        lv_obj_set_style_bg_grad_stop(zone, 110, 0);
    }
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, handle_clicked, LV_EVENT_CLICKED, NULL);
    const int32_t x = (lv_obj_get_style_width(zone, 0) - (int32_t)mark->header.w) / 2;
    reveal(icon(zone, mark, x, top ? 0 : 27 - (int32_t)mark->header.h / 2, VALUE));
}
static void handle(void)
{
    handle_at(&pet_icon_chevron_up, false);
}

/* The story frame drawn again: its tab built again, as its style and the
 * story's path have it, and its rim. */
static void redraw_story_frame(void)
{
    if (!s_story_frame) return;
    lv_obj_clean(s_story_frame);
    s_theme->frame_tab(s_story_frame);
    lv_obj_invalidate(s_story_frame);
}

/* The tick: the cursors blink, a scan spins, the log's clock runs, and the
 * story frame follows the story's path. */
static void tick(lv_timer_t *timer)
{
    (void)timer;
    static const char spinner[] = "|/-\\";
    ++s_ticks;
    const bool detour = atomic_load(&s_story_detour), hurry = atomic_load(&s_story_hurry);
    if (detour != s_frame_detour || hurry != s_frame_hurry) {
        s_frame_detour = detour;
        s_frame_hurry = hurry;
        redraw_story_frame();
    }
    /* A new story shows at once on an open story page, the log's newest line in view. */
    if (atomic_exchange(&s_story_fresh, false) && (story_screen(s_page) || s_page == PAGE_STORIES) && !s_leaving &&
        lv_screen_active() == s_display_screen)
        show_page(s_page, false);
    if (s_restart_armed && s_page == PAGE_STORIES && lv_tick_elaps(s_restart_armed_at) >= RESTART_CONFIRM_MS) {
        s_restart_armed = false;
        show_page(PAGE_STORIES, false);
    }
    if (s_scanning && s_page == PAGE_WIFI && !s_message && s_status_label) {
        char line[24];
        snprintf(line, sizeof(line), "SCANNING %c", spinner[s_ticks % 4]);
        type_status(line, VALUE, false);
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
    set_text(s_slider_value[which], line, which == SETTING_VOLUME && !s_setting[which] ? WARN : VALUE);
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

/* A card of cells to drag or tap, as VOLUME's is: a column at its left for
 * the caller's icon (26 px wide from x 12), a rule, `name` over the cells,
 * and the value at the right in `value`. Its slider, whose cells `cells`
 * draws; the caller sets its range. */
static lv_obj_t *bar_card(int y, const char *name, const segments_t *cells, lv_obj_t **value)
{
    lv_obj_t *face = card(s_menu, CARD_X, y, CARD_W, 58, CARD_PLAIN);
    lv_obj_t *rule = plain(face);
    lv_obj_set_pos(rule, 48, 9);
    lv_obj_set_size(rule, 1, 40);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, shade(rule, EDGE), 0);
    text(face, FONT_SMALL, name, 58, 8, LABEL, 2);
    lv_obj_t *slider = lv_slider_create(face);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, 56, 25);
    int32_t extra_width = lv_obj_get_style_width(face, 0) - CARD_W;
    lv_obj_set_size(slider, 176 + extra_width, 24);
    lv_obj_set_ext_click_area(slider, 8);
    lv_obj_add_event_cb(slider, draw_segments, LV_EVENT_DRAW_MAIN, (void *)cells);
    *value = text(face, FONT_BIG, "", 240 + extra_width, 13, VALUE, 0);
    return slider;
}

/* VOLUME / BRIGHTNESS: an icon, the name, ten cells to drag, the level. */
static void setting_card(setting_t which, int y)
{
    lv_obj_t *slider = bar_card(y, which == SETTING_VOLUME ? "VOLUME" : "BRIGHTNESS", &s_slider_segments,
                                &s_slider_value[which]);
    lv_obj_t *face = lv_obj_get_parent(slider);
    const lv_image_dsc_t *image = which == SETTING_VOLUME ? &pet_icon_speaker : &pet_icon_sun;
    icon(face, image, 12 + (26 - (int32_t)image->header.w) / 2, (58 - (int32_t)image->header.h) / 2, VALUE);
    if (which == SETTING_VOLUME) {
        lv_obj_t *mute = plain(face);
        lv_obj_set_size(mute, 46, 58);
        lv_obj_add_flag(mute, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mute, mute_clicked, LV_EVENT_CLICKED, NULL);
    }
    lv_slider_set_range(slider, 0, 100);
    lv_obj_add_event_cb(slider, slider_moved, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)which);
    s_slider[which] = slider;
    setting_show(which);
}

/* A cell of the pet picker: the pet at that place among the installed pets.
 * With at most three, it is always the next or the previous one. */
static void pet_cell_clicked(lv_event_t *event)
{
    unsigned target = (unsigned)(uintptr_t)lv_event_get_user_data(event) + 1;
    unsigned shown = s_device_status.pet_index, count = s_device_status.pet_count;
    if (!shown || target == shown || target > count) return;
    int direction = target == shown % count + 1 ? 1 : -1;
    if (s_control.switch_pet && s_control.switch_pet(direction)) leave_to_pet();
    else status_text("BUSY - TRY AGAIN");
}

/* PET: the pet on screen, and with switch_pet a cell for each installed pet
 * to show another; render_home lights the one on screen. */
static void pet_card(int y)
{
    lv_obj_t *pets = card(s_menu, CARD_X, y, CARD_W, 56, CARD_PLAIN);
    int32_t width = lv_obj_get_style_width(pets, 0);
    icon(pets, &pet_icon_paw, 9, 17, VALUE);
    lv_obj_t *rule = plain(pets);
    lv_obj_set_pos(rule, 38, 9);
    lv_obj_set_size(rule, 1, 38);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, shade(rule, EDGE), 0);
    s_pets_label = text(pets, FONT_SMALL, "PET", 46, 9, LABEL, 2);
    s_pets_value = text(pets, FONT_TEXT, "", 46, 27, VALUE, 0);
    lv_obj_set_size(s_pets_value, width - 46 - PET_PICKER_MAX * (PICKER_CELL + PICKER_GAP) - 8, 19);
    lv_label_set_long_mode(s_pets_value, LV_LABEL_LONG_DOT);
    if (!s_control.switch_pet) return;
    for (unsigned i = 0; i < PET_PICKER_MAX; ++i) {
        char digit[2] = {(char)('1' + i), 0};
        s_pet_cell[i] = card(pets, 0, 8, PICKER_CELL, 40, CARD_PRESS | CARD_SLOT);
        lv_obj_center(text(s_pet_cell[i], FONT_TEXT, digit, 0, 0, VALUE, 0));
        lv_obj_add_event_cb(s_pet_cell[i], pet_cell_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

/* The picker's cells, right-aligned: one for each installed pet when there
 * is a choice, the one on screen lit. */
static void render_pet_cells(const pet_onboarding_status_t *s)
{
    const int32_t count = s->pet_count < PET_PICKER_MAX ? s->pet_count : PET_PICKER_MAX;
    for (int32_t i = 0; i < PET_PICKER_MAX; ++i) {
        if (!s_pet_cell[i]) continue;
        const bool shown = count > 1 && i < count;
        const int32_t right = lv_obj_get_style_width(lv_obj_get_parent(s_pet_cell[i]), 0) - 8;
        lv_obj_set_x(s_pet_cell[i], right - (count - i) * PICKER_CELL - (count - 1 - i) * PICKER_GAP);
        set_visible(s_pet_cell[i], shown);
        set_lit(s_pet_cell[i], shown && i + 1 == s->pet_index);
    }
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
    LOG(VALUE, SYSTEM " %s esp32s3", field[0] ? field : "-");
    display_text(name, sizeof(name), s->pack_name, 12, true);
    display_text(version, sizeof(version), s->pack_version, 10, false);
    display_text(field, sizeof(field), s->build_id, 8, false);
    LOG(DIM, "$ cat /etc/pet");
    if (s->has_pet && name[0]) LOG(VALUE, "%.12s v%.10s  build %.8s", name, version, field[0] ? field : "-");
    else LOG(WARN, "no pet yet");
    LOG(DIM, "$ ls /pets");
    if (s->pet_capacity) LOG(VALUE, "%u of %u slots used, #%u on screen", s->pet_count, s->pet_capacity, s->pet_index);
    else if (s->single_pet_slot) LOG(VALUE, "1 slot %luK, no backup", (unsigned long)((s->slots[0].bytes + 1023) / 1024));
    else LOG(VALUE, "A %luK  B %luK", (unsigned long)((s->slots[0].bytes + 1023) / 1024),
             (unsigned long)((s->slots[1].bytes + 1023) / 1024));
    display_text(field, sizeof(field), s->ssid, 20, false);
    LOG(DIM, "$ wifi");
    LOG(s->wifi == PET_ONBOARDING_WIFI_CONNECTED ? GOOD : WARN, "%s%s%s", field[0] ? field : "not set up",
        field[0] ? ": " : "", !field[0] ? "" : s->wifi == PET_ONBOARDING_WIFI_CONNECTED ? "connected" :
        s->wifi == PET_ONBOARDING_WIFI_CONNECTING ? "joining" : "offline");
    static const char *const cloud[] = {"needs pairing", "offline", "syncing", "online", "needs help"};
    LOG(DIM, "$ cloud");
    LOG(s->cloud == PET_ONBOARDING_CLOUD_ATTENTION ? FAIL : s->cloud >= PET_ONBOARDING_CLOUD_SYNCING ? GOOD : WARN,
        "%s", cloud[s->cloud]);
    static const char *const voice[] = {"unavailable", "ready: tap to talk", "listening", "thinking", "speaking", "error"};
    LOG(DIM, "$ voice");
    if (s->conversation != PET_ONBOARDING_CONVERSATION_UNAVAILABLE)
        LOG(s->conversation == PET_ONBOARDING_CONVERSATION_ERROR ? FAIL : GOOD, "%s", voice[s->conversation]);
    else LOG(WARN, "%s", !s->has_pet ? "no pet" : s->wifi != PET_ONBOARDING_WIFI_CONNECTED ? "no wifi" :
             s->cloud == PET_ONBOARDING_CLOUD_PAIRING ? "not paired" : !s->pet_linked ? "not linked to aipets.com" :
             "connecting");
    static const char *const sync[] = {"idle", "queued", "downloading", "verifying", "activating", "ready", "failed"};
    LOG(DIM, "$ sync");
    if (s->installation >= PET_ONBOARDING_INSTALL_REQUESTED && s->installation <= PET_ONBOARDING_INSTALL_ACTIVATING)
        LOG(VALUE, "%s %u%%", sync[s->installation], s->installation_percent);
    else LOG(s->installation == PET_ONBOARDING_INSTALL_FAILED ? FAIL : GOOD, "%s%s", sync[s->installation],
             s->retry_seconds ? ", retry soon" : "");
    display_text(field, sizeof(field), s->support_error_code, 32, false);
    if (field[0]) LOG(FAIL, "error %s", field);
    else if (s->retry_seconds) LOG(WARN, "retry in %us", s->retry_seconds);
    else LOG(DIM, "no errors");
    uint32_t seconds = lv_tick_get() / 1000;
    LOG(DIM, "$ uptime");
    LOG(VALUE, "up %lu:%02lu:%02lu", (unsigned long)(seconds / 3600), (unsigned long)(seconds / 60 % 60),
        (unsigned long)(seconds % 60));
    LOG(DIM, "$ credits");
    LOG(DIM, "%s", s_theme->credits[0]);
    LOG(DIM, "%s", s_theme->credits[1]);
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
    set_text(s_wifi_name, ssid[0] ? ssid : "NOT SET UP", ssid[0] ? VALUE : WARN);
    set_text(s_wifi_state, connected ? "CONNECTED" : s->wifi == PET_ONBOARDING_WIFI_CONNECTING ? "JOINING" : "OFFLINE",
             connected ? DIM : WARN);
    set_icon_color(s_wifi_icon, connected ? GOOD : WARN);

    if (s->battery_valid) {
        unsigned percent = s->battery_percent > 100 ? 100 : s->battery_percent;
        snprintf(line, sizeof(line), "%u%%", percent);
        set_text(s_battery_value, line, percent <= 15 ? FAIL : percent <= 25 ? WARN : GOOD);
    } else set_text(s_battery_value, "--", DIM);
    if (s_battery_icon) lv_obj_invalidate(s_battery_icon);

    static const char *const voice[] = {"", "READY", "LISTENING", "THINKING", "SPEAKING", "ERROR"};
    if (s->conversation != PET_ONBOARDING_CONVERSATION_UNAVAILABLE)
        set_text(s_voice_value, voice[s->conversation], s->conversation == PET_ONBOARDING_CONVERSATION_ERROR ? FAIL : GOOD);
    else set_text(s_voice_value, !s->has_pet ? "NO PET" : !connected ? "NO WIFI" :
                  s->cloud == PET_ONBOARDING_CLOUD_PAIRING ? "NOT PAIRED" : !s->pet_linked ? "NOT LINKED" :
                  "CONNECTING", WARN);

    char name[20];
    display_text(name, sizeof(name), s->pet_name[0] ? s->pet_name : s->pack_name, 16, true);
    set_text(s_pets_value, s->has_pet && name[0] ? name : "NONE", s->has_pet ? VALUE : DIM);
    if (s->pet_capacity && s->pet_index) snprintf(line, sizeof(line), "PET %u/%u", s->pet_index, s->pet_count);
    else strcpy(line, "PET");
    set_text(s_pets_label, line, LABEL);
    render_pet_cells(s);
    if (s_story_value) {
        char story[PET_STORY_LIST_TITLE_BYTES + 1];
        current_story(story, sizeof(story));
        set_text(s_story_value, story[0] ? story : "NONE", story[0] ? VALUE : DIM);
    }

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
        set_text(letter, shown, i < length ? VALUE : DIM);
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
    lv_obj_add_flag(obj, PAPER);
    lv_obj_set_style_radius(obj, 3, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, shade(obj, FACE), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, shade(obj, EDGE), 0);
    lv_obj_set_style_border_color(obj, shade(obj, LIT), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(obj, 9, 0);
    lv_obj_set_style_pad_ver(obj, (height - 18) / 2, 0);
    lv_obj_set_style_text_color(obj, shade(obj, VALUE), 0);
    lv_obj_set_style_text_color(obj, shade(obj, DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    /* A blinking block cursor in inverse video, only in the focused field. */
    lv_obj_set_style_bg_color(obj, shade(obj, LIT), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(obj, shade(obj, ON_LIT), LV_PART_CURSOR | LV_STATE_FOCUSED);
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
    lv_obj_set_style_bg_color(s_keyboard, shade(s_keyboard, FACE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_keyboard, shade(s_keyboard, EDGE), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, shade(s_keyboard, VALUE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_keyboard, FONT_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, shade(s_keyboard, LIT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_keyboard, shade(s_keyboard, LIT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_keyboard, shade(s_keyboard, ON_LIT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_keyboard, shade(s_keyboard, LIT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_keyboard, shade(s_keyboard, LIT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_keyboard, shade(s_keyboard, ON_LIT), LV_PART_ITEMS | LV_STATE_PRESSED);
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
        lv_obj_set_style_bg_color(s_tube[i], shade(s_tube[i], i < 2 ? GLASS : LIT), 0);
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
    show_page(gone && story_screen(s_page) ? PAGE_HOME : s_page, false);
    if (!gone) crt_on();
}
static void back_to_pet(void *unused)
{
    (void)unused;
    if (s_control.return_to_pet) s_control.return_to_pet();
    if (lv_screen_active() != s_display_screen) {
        /* Ready, out of sight, for the next swipe down; a story screen's
         * objects go. */
        show_page(story_screen(s_page) ? PAGE_HOME : s_page, false);
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
/* Power off: the page folds into a line, the line into a dot, then the pet.
 * A story screen was opened from the pet, so it always goes back. */
static void leave_to_pet(void)
{
    if (s_leaving || (!s_device_status.has_pet && !story_screen(s_page)) || !s_control.return_to_pet) return;
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
        case PAGE_RESTART_CONFIRM: case PAGE_LIBRARY: case PAGE_ABOUT: case PAGE_STORIES: show(PAGE_HOME); break;
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
static void leave_later(void *unused)
{
    (void)unused;
    leave_to_pet();
}
/* The swipe that leaves a page: up on the menu's pages, and back the way it
 * came on a story screen. */
static lv_dir_t closing_swipe(page_t page)
{
    switch (page) {
        case PAGE_BLESSINGS: return LV_DIR_LEFT;
        case PAGE_CHARACTER: return LV_DIR_RIGHT;
        case PAGE_STORY: return LV_DIR_BOTTOM;
        default: return LV_DIR_TOP;
    }
}
static void gesture(lv_event_t *event)
{
    (void)event;
    lv_indev_t *input_device = lv_indev_active();
    if (!input_device) return;
    const bool closing = lv_indev_get_gesture_dir(input_device) == closing_swipe(s_page);
    /* The press that became a swipe must not also press what it started on;
     * on a story screen, any swipe. */
    if (closing || story_screen(s_page)) lv_indev_wait_release(input_device);
    if (closing) lv_async_call(story_screen(s_page) ? leave_later : swipe_later, NULL);
}
/* Pulling the deck up past the end of its log closes it too, and the STORY
 * screen down past its first line. */
static void scrolled_past_end(lv_event_t *event)
{
    lv_obj_t *page = lv_event_get_current_target_obj(event);
    lv_indev_t *input_device = lv_indev_active();
    const bool top = s_page == PAGE_STORY;
    if (s_leaving || !input_device || lv_indev_get_state(input_device) != LV_INDEV_STATE_PRESSED ||
        (top ? lv_obj_get_scroll_top(page) : lv_obj_get_scroll_bottom(page)) > -OVERSCROLL_CLOSE) return;
    lv_indev_wait_release(input_device);
    lv_async_call(top ? leave_later : swipe_later, NULL);
}

/* Follow the physical glass, rather than drawing a round screen on a rectangle. */
static void bezel(void)
{
    s_bezel = plain(s_screen);
    lv_obj_set_size(s_bezel, s_width - 6, s_height - 6);
    lv_obj_center(s_bezel);
    lv_obj_set_style_radius(s_bezel, pet_board_current()->round ? LV_RADIUS_CIRCLE : 20, 0);
    lv_obj_set_style_border_width(s_bezel, 1, 0);
    lv_obj_set_style_border_color(s_bezel, shade(s_bezel, UNLIT), 0);
}

/* "> WIFI ............ HOMENET": a boot log line with a dot leader. */
static lv_obj_t *boot_line(int y, uint32_t delay, const char *key, const char *value, role_t color)
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
    lv_obj_t *mark = icon(s_screen, logo, (s_width - (int32_t)logo->header.w) / 2, page_y(50), LIT);
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
    boot_line(206, 520, "DISPLAY", "OK", GOOD);
    boot_line(224, 680, "TOUCH", "OK", GOOD);
    boot_line(242, 840, "WIFI", field[0] ? field : "SETUP", field[0] ? GOOD : WARN);
    lv_obj_t *last = boot_line(260, 1000, "PETS", "LOADING", VALUE);
    s_cursor = plain(s_screen);
    lv_obj_set_size(s_cursor, 6, 11);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cursor, shade(s_cursor, VALUE), 0);
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
    paper(s_network_list, false);
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
        lv_obj_t *item = card(s_network_list, 0, (int32_t)i * 42, list_width, 38, CARD_PRESS | CARD_ROW);
        s_animate = true;
        lv_obj_t *bars = plain(item);
        lv_obj_set_pos(bars, 12, 9);
        lv_obj_set_size(bars, 22, 17);
        int8_t rssi = s_networks[i].rssi;
        lv_obj_set_user_data(bars, (void *)(uintptr_t)(rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1));
        lv_obj_add_event_cb(bars, draw_signal, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_t *caption = text(item, FONT_TEXT, name, 44, 9, here ? VALUE : LABEL, 0);
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
        s_code_cell[i] = card(s_screen, 94 + 44 * (int32_t)i, 78, 40, 40, CARD_SLOT);
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
                             14, 28, VALUE, 1);
        lv_obj_set_size(who, numbered ? 176 : 236, 42);
        lv_label_set_long_mode(who, LV_LABEL_LONG_DOT);
        if (numbered) {
            text(face, FONT_SMALL, "NUMBER", 198, 10, LABEL, 2);
            text(face, FONT_BIG, l->check, 198, 30, VALUE, 3);
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
        centered(face, FONT_BIG, code, 13, 264, shown ? VALUE : DIM, 3);
        centered(s_screen, FONT_TEXT, "AIPETS.COM/LINK", 150, 300, shown ? VALUE : DIM, 2);
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
/* WOODLAND's faces, Jersey and Jacquard 12, are linked, and so is LVGL's
 * default font, Montserrat 14, which carries Font Awesome's symbols; the OFL
 * text below covers them all. Only some cues come from UISFX. */
static const char about_notices[] =
    "Share Tech Mono  SIL OFL 1.1, below\n"
    "Jersey           SIL OFL 1.1, below\n"
    "Jacquard 12      SIL OFL 1.1, below\n"
    "  (c) 2023 The Soft Type\n"
    "  Project Authors\n"
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
    paper(body, false);
    int32_t body_width = lv_obj_get_style_width(body, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 18, 0);
    lv_obj_t *story = text(body, FONT_TEXT, about_story, 0, 0, VALUE, 0);
    lv_obj_t *site = text(body, FONT_TITLE, "aipets.com", 0, 0, VALUE, 1);
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
    lv_obj_set_style_bg_color(line, shade(line, EDGE), 0);
    lv_obj_set_width(text(body, FONT_SMALL, about_notices, 0, 0, LABEL, 0), body_width);
    lv_obj_t *words = text(body, FONT_SMALL, "", 0, 0, DIM, 0);
    lv_obj_set_width(words, body_width);
    lv_label_set_text_static(words, pet_menu_font_license);
    reveal(body);
    status_line(s_screen, 60, "YOUR PET, YOUR DATA");
    handle();
}

/* Pixel art shown `scale` times its size, every pixel kept square. */
static lv_obj_t *pixel_icon(lv_obj_t *parent, const lv_image_dsc_t *image, int32_t scale, int x, int y,
                            role_t color)
{
    lv_obj_t *obj = icon(parent, image, x, y, color);
    lv_obj_set_size(obj, (int32_t)image->header.w * scale, (int32_t)image->header.h * scale);
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_antialias(obj, false);
    return obj;
}

/* A story game's blessing icons, in the protocol's order
 * (pet_blessing_icon_t); its storm is the bolt. */
static const lv_image_dsc_t *const s_blessing_icons[PET_BLESSING_ICONS] = {
    &pet_icon_heart, &pet_icon_bolt, &pet_icon_shield, &pet_icon_star, &pet_icon_flame, &pet_icon_leaf,
    &pet_icon_moon, &pet_icon_eye, &pet_icon_key, &pet_icon_feather, &pet_icon_potion, &pet_icon_sword,
    &pet_icon_blessing_mask, &pet_icon_blessing_hand, &pet_icon_blessing_bubble, &pet_icon_blessing_skull,
    &pet_icon_blessing_crown, &pet_icon_blessing_coin, &pet_icon_blessing_ring, &pet_icon_blessing_scroll,
    &pet_icon_blessing_bell, &pet_icon_blessing_hourglass, &pet_icon_blessing_sun, &pet_icon_blessing_cloud,
    &pet_icon_blessing_drop, &pet_icon_blessing_pick, &pet_icon_blessing_bow, &pet_icon_blessing_lantern,
    &pet_icon_blessing_gem, &pet_icon_blessing_ghost, &pet_icon_blessing_paw, &pet_icon_blessing_gear,
};

/* The story state of the pet on screen, or NULL. Under s_story_lock. */
static const pet_story_state_t *shown_state(void)
{
    return s_story && !strcmp(s_story->state_owner, s_device_status.pack_name) ? &s_story->state : NULL;
}

/* A blessing's uses left as pips, its user data: dark on a lit cell. */
static void draw_pips(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    const int32_t uses = (int32_t)(uintptr_t)lv_obj_get_user_data(obj);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    const int32_t pip = lv_area_get_height(&box);
    lv_draw_rect_dsc_t look;
    lv_draw_rect_dsc_init(&look);
    look.bg_color = shade(obj, lv_obj_has_state(lv_obj_get_parent(obj), LV_STATE_CHECKED) ? ON_LIT : VALUE);
    for (int32_t i = 0; i < uses; ++i) {
        const lv_area_t mark = {box.x1 + i * (pip + 1), box.y1, box.x1 + i * (pip + 1) + pip - 1, box.y2};
        lv_draw_rect(lv_event_get_layer(event), &look, &mark);
    }
}

/* A tapped blessing of a story game is chosen, and the page shows its USE. */
static void game_blessing_clicked(lv_event_t *event)
{
    const unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    const pet_story_state_t *game = shown_state();
    if (game && index < game->blessing_count)
        strlcpy(s_blessing_id, game->blessings[index].id, sizeof(s_blessing_id));
    xSemaphoreGive(s_story_lock);
    show_page(PAGE_BLESSINGS, false);
}

/* USE: the pet uses the chosen blessing and shows, to react to it; one that
 * takes the owner's words listens for them first. */
static void use_clicked(lv_event_t *event)
{
    (void)event;
    bool speak = false;
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    const pet_story_state_t *game = shown_state();
    for (uint8_t i = 0; game && i < game->blessing_count; ++i)
        if (!strcmp(game->blessings[i].id, s_blessing_id)) speak = game->blessings[i].speak;
    xSemaphoreGive(s_story_lock);
    if (s_control.use_blessing && s_control.use_blessing(s_blessing_id, speak)) leave_to_pet();
    else status_text("BUSY - TRY AGAIN");
}

/* One cell of a story game's grid: the blessing's picture over its uses,
 * dimmed once spent. */
static void game_cell(lv_obj_t *grid, int32_t index, int32_t cell, int32_t at_x, int32_t at_y,
                      const pet_story_blessing_t *blessing)
{
    const int32_t scale = SMALL_SCREEN ? 2 : 3, picture = 12 * scale, pip = SMALL_SCREEN ? 2 : 3;
    lv_obj_t *slot = card(grid, at_x, at_y, cell, cell, CARD_PRESS | CARD_SLOT);
    pixel_icon(slot, s_blessing_icons[blessing->icon], scale, (cell - picture) / 2, (cell - picture) / 2 - pip,
               blessing->uses ? VALUE : UNLIT);
    if (blessing->uses) {
        const int32_t width = (int32_t)blessing->uses * (pip + 1) - 1;
        lv_obj_t *pips = plain(slot);
        lv_obj_set_pos(pips, (cell - width) / 2, cell - pip - (SMALL_SCREEN ? 3 : 5));
        lv_obj_set_size(pips, width, pip);
        lv_obj_set_user_data(pips, (void *)(uintptr_t)blessing->uses);
        lv_obj_add_event_cb(pips, draw_pips, LV_EVENT_DRAW_MAIN, NULL);
    }
    set_lit(slot, !strcmp(blessing->id, s_blessing_id));
    lv_obj_add_event_cb(slot, game_blessing_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
}

/* BLESSINGS: the story's powers in a 3x3 inventory grid, empty cells dark,
 * and USE under the chosen one. A story without blessings shows only the
 * empty grid. */
static void blessings_page(void)
{
    title(s_screen, "BLESSINGS", 26);
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    const pet_story_state_t *game = shown_state();
    if (game && !game->has_blessings) game = NULL;
    /* A game's grid leaves room under it for USE. */
    const int32_t cell = game ? (SMALL_SCREEN ? 34 : 56) : (SMALL_SCREEN ? 40 : 64);
    const int32_t gap = SMALL_SCREEN ? 4 : game ? 6 : 8, side = 3 * cell + 2 * gap;
    const int32_t top = game ? page_y(80) : page_y(84), bottom = game ? page_y(262) : s_height - 56;
    lv_obj_t *grid = plain(s_screen);
    lv_obj_set_pos(grid, (s_width - side) / 2, top + (bottom - top - side) / 2);
    lv_obj_set_size(grid, side, side);
    const pet_story_blessing_t *chosen = NULL;
    for (int32_t i = 0; i < 9; ++i) {
        const int32_t x = i % 3 * (cell + gap), y = i / 3 * (cell + gap);
        if (game && i < game->blessing_count) {
            game_cell(grid, i, cell, x, y, &game->blessings[i]);
            if (!strcmp(game->blessings[i].id, s_blessing_id)) chosen = &game->blessings[i];
        } else {
            card(grid, x, y, cell, cell, CARD_EMPTY);
        }
    }
    static char name[PET_STORY_NAME_BYTES + 1];
    if (chosen) display_text(name, sizeof(name), chosen->name, PET_STORY_NAME_BYTES, true);
    if (chosen && chosen->uses) {
        char caption[16];
        snprintf(caption, sizeof(caption), chosen->speak ? "SPEAK - %u LEFT" : "USE - %u LEFT", (unsigned)chosen->uses);
        command(s_screen, caption, NULL, 80, 268, 200, 36, use_clicked);
    } else if (chosen) {
        lv_obj_t *spent = card(s_screen, 80, 268, 200, 36, CARD_PLAIN);
        lv_obj_center(text(spent, FONT_TEXT, "SPENT", 0, 0, DIM, 2));
    }
    xSemaphoreGive(s_story_lock);
    const char *line = chosen ? name : game ? "TAP A BLESSING" : "NO BLESSINGS";
    status_line(s_screen, 60, line);
    handle_at(&pet_icon_handle_left, false);
}

/* Plain cells, unlike the deck's framed sliders: nothing here to drag. */
static const segments_t s_stat_segments = {10, false, false, false, false};
static const segments_t s_health_segments = {10, false, false, true, false};

/* One stat: its picture, name and value over a bar of cells, `value` of
 * `maximum`; the value is written `number`. */
static void stat_row(lv_obj_t *sheet, int32_t y, int32_t row, const char *label, const lv_image_dsc_t *image,
                     uint8_t value, uint8_t maximum, const char *number)
{
    const segments_t *look = image == &pet_icon_heart ? &s_health_segments : &s_stat_segments;
    const int32_t width = lv_obj_get_style_width(sheet, 0), scale = SMALL_SCREEN ? 1 : 2;
    const int32_t inset = SMALL_SCREEN ? 8 : 12, column = inset + 12 * scale + (SMALL_SCREEN ? 6 : 12);
    const int32_t line = lv_font_get_line_height(FONT_SMALL), bar_height = LV_MIN(row - line - 4, 14);
    const int32_t top = y + (row - line - 2 - bar_height) / 2;
    pixel_icon(sheet, image, scale, inset, y + (row - 12 * scale) / 2, VALUE);
    text(sheet, FONT_SMALL, label, column, top, LABEL, 2);
    lv_obj_t *count = text(sheet, FONT_SMALL, number, 0, top, VALUE, 0);
    lv_obj_set_x(count, width - inset - text_width(FONT_SMALL, number, 0));
    lv_obj_t *bar = lv_bar_create(sheet);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, column, top + line + 2);
    lv_obj_set_size(bar, width - column - inset, bar_height);
    lv_bar_set_range(bar, 0, maximum);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_add_event_cb(bar, s_theme->draw_stat, LV_EVENT_DRAW_MAIN, (void *)look);
}

/* A condition tag: its padding and border around the text, and the gap
 * between tags. */
#define TAG_FRAME 12
#define TAG_GAP 6

/* A condition's amber tag, cut with dots at `max_width`. */
static void condition_tag(lv_obj_t *row, const char *name, int32_t max_width)
{
    lv_obj_t *chip = text(row, FONT_SMALL, name, 0, 0, WARN, 1);
    lv_obj_set_style_max_width(chip, max_width, 0);
    lv_obj_set_height(chip, lv_font_get_line_height(FONT_SMALL) + 4);
    lv_label_set_long_mode(chip, LV_LABEL_LONG_DOT);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, shade(chip, WARN), 0);
    lv_obj_set_style_pad_hor(chip, 5, 0);
    lv_obj_set_style_pad_top(chip, 1, 0);
}

/* The hero's conditions as amber tags in one centred row at `y`: as many as
 * fit, then "+N" for the rest, so the sheet below keeps its room. Returns the
 * y just below them. */
static int32_t condition_tags(const pet_story_state_t *game, int32_t y)
{
    const int32_t width = page_x(316) - page_x(44), more = text_width(FONT_SMALL, "+3", 1) + TAG_FRAME;
    lv_obj_t *row = plain(s_screen);
    lv_obj_set_pos(row, page_x(44), y);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TAG_GAP, 0);
    int32_t used = 0;
    unsigned shown = 0;
    for (; shown < game->condition_count; ++shown) {
        char name[PET_STORY_NAME_BYTES + 1];
        display_text(name, sizeof(name), game->conditions[shown], PET_STORY_NAME_BYTES, true);
        /* Room for this tag, and for "+N" after it unless it is the last. */
        const bool last = shown + 1 == game->condition_count;
        const int32_t room = width - used - (shown ? TAG_GAP : 0) - (last ? 0 : TAG_GAP + more);
        const int32_t need = text_width(FONT_SMALL, name, 1) + TAG_FRAME;
        if (shown && need > room) break;
        condition_tag(row, name, room);
        used += (shown ? TAG_GAP : 0) + LV_MIN(need, room);
    }
    if (shown < game->condition_count) {
        char rest[12];
        snprintf(rest, sizeof(rest), "+%u", game->condition_count - shown);
        condition_tag(row, rest, more);
    }
    lv_obj_update_layout(row);
    return y + lv_obj_get_height(row);
}

/* CHARACTER: the pet on screen and, when the story plays as a game, its hero's
 * stats as pixel bars with their conditions. The small screen names the pet on
 * the status line instead. */
static void character_page(void)
{
    static char name[20];
    const pet_onboarding_status_t *s = &s_device_status;
    display_text(name, sizeof(name), s->pet_name[0] ? s->pet_name : s->pack_name, 16, true);
    title(s_screen, "CHARACTER", 26);
    if (!SMALL_SCREEN) centered(s_screen, FONT_BIG, name, 80, 280, VALUE, 2);
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    const pet_story_state_t *game = shown_state();
    /* The sheet ends above the chevron. */
    int32_t top = SMALL_SCREEN ? page_y(84) : page_y(120);
    const int32_t bottom = LV_MIN(page_y(292), s_height - 60);
    if (game && game->condition_count) top = condition_tags(game, SMALL_SCREEN ? top : page_y(114)) + 6;
    if (game && game->has_hero) {
        const int32_t row = (bottom - top - 8) / 4;
        lv_obj_t *sheet = card(s_screen, 52, 120, 256, 172, CARD_PLAIN);
        lv_obj_set_y(sheet, top);
        lv_obj_set_height(sheet, bottom - top);
        char health[8], attack[8], defense[8], willpower[8];
        snprintf(health, sizeof(health), "%u/%u", game->hero.health, game->hero.max_health);
        snprintf(attack, sizeof(attack), "%u/20", game->hero.attack);
        snprintf(defense, sizeof(defense), "%u/20", game->hero.defense);
        snprintf(willpower, sizeof(willpower), "%u/20", game->hero.willpower);
        stat_row(sheet, 4, row, "HEALTH", &pet_icon_heart, game->hero.health, game->hero.max_health, health);
        stat_row(sheet, 4 + row, row, "ATTACK", &pet_icon_bolt, game->hero.attack, 20, attack);
        stat_row(sheet, 4 + 2 * row, row, "DEFENSE", &pet_icon_shield, game->hero.defense, 20, defense);
        stat_row(sheet, 4 + 3 * row, row, "WILLPOWER", &pet_icon_flame, game->hero.willpower, 20, willpower);
    }
    xSemaphoreGive(s_story_lock);
    status_line(s_screen, 60, SMALL_SCREEN ? name : "READY FOR ADVENTURE");
    handle_at(&pet_icon_handle_right, false);
}

/* One line of the story log, wrapped under its prompt. */
static void log_line(lv_obj_t *log, const char *line, role_t color)
{
    const int32_t width = lv_obj_get_style_width(log, 0);
    lv_obj_t *row = plain(log);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    text(row, FONT_SMALL, ">", 0, 0, DIM, 0);
    lv_obj_t *words = text(row, FONT_SMALL, line, 14, 0, color, 0);
    lv_obj_set_width(words, width - 14);
}

/* The STORY screen's header: the story's title, NEXT and the next
 * milestone's place, and a pip for each milestone, lit once reached.
 * Returns the y just below it. */
static int32_t story_header(const pet_story_state_t *state)
{
    char name[PET_STORY_TITLE_BYTES + 1], goal[PET_STORY_GOAL_BYTES + 1];
    display_text(name, sizeof(name), state->title, PET_STORY_TITLE_BYTES, true);
    display_text(goal, sizeof(goal), state->goal, PET_STORY_GOAL_BYTES, true);
    const int32_t pip = SMALL_SCREEN ? 6 : 8, next = 8 + lv_font_get_line_height(FONT_TEXT) + 3;
    const int32_t pips = next + lv_font_get_line_height(FONT_SMALL) + 5, height = pips + pip + 8;
    lv_obj_t *head = card(s_screen, 44, 80, 272, 62, CARD_PLAIN);
    lv_obj_set_height(head, height);
    const int32_t width = lv_obj_get_style_width(head, 0);
    lv_obj_t *title_line = text(head, FONT_TEXT, name, 12, 8, VALUE, 0);
    lv_obj_set_size(title_line, width - 24, lv_font_get_line_height(FONT_TEXT));
    lv_label_set_long_mode(title_line, LV_LABEL_LONG_DOT);
    text(head, FONT_SMALL, "NEXT", 12, next, LABEL, 2);
    const int32_t left = 12 + text_width(FONT_SMALL, "NEXT", 2) + 8;
    const bool ended = state->of && state->reached >= state->of;
    lv_obj_t *place = text(head, FONT_SMALL, goal[0] ? goal : ended ? "THE END" : "-", left, next, VALUE, 1);
    lv_obj_set_size(place, width - 12 - left, lv_font_get_line_height(FONT_SMALL));
    lv_label_set_long_mode(place, LV_LABEL_LONG_DOT);
    for (int32_t i = 0; i < state->of; ++i) {
        lv_obj_t *mark = plain(head);
        lv_obj_set_pos(mark, 12 + i * (pip + 4), pips);
        lv_obj_set_size(mark, pip, pip);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(mark, shade(mark, i < state->reached ? LIT : UNLIT), 0);
    }
    return lv_obj_get_style_y(head, 0) + height;
}

/* STORY: the story's header, then a line for each turn of the pet's story,
 * printed like a terminal: oldest first, the newest at the bottom beside the
 * cursor, older turns a swipe down away. Only the pet on screen's own story
 * shows. */
static void story_page(void)
{
    handle_at(&pet_icon_handle_down, true);
    title(s_screen, "STORY", 26);
    atomic_store(&s_story_fresh, false); /* this page reads the newest story */
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    const story_t *story = s_story;
    const char *pet = s_device_status.pack_name;
    const unsigned count = story && !strcmp(story->log_owner, pet) ? story->count : 0;
    const bool headed = story && !strcmp(story->state_owner, pet) && story->state.title[0];
    static char turns[16];
    snprintf(turns, sizeof(turns), "LAST %u TURNS", count);
    status_line(s_screen, 60, !count ? "NO TURNS YET" : count == 1 ? "LAST TURN" : turns);
    const int32_t top = headed ? story_header(&story->state) + 6 : page_y(84);
    lv_obj_t *log = lv_obj_create(s_screen);
    lv_obj_remove_style_all(log);
    page_box(log, 44, 84, 272, 210);
    lv_obj_set_y(log, top);
    lv_obj_set_height(log, page_y(294) - top);
    paper(log, true);
    lv_obj_set_scroll_dir(log, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(log, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(log, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(log, 6, 0);
    lv_obj_add_event_cb(log, scrolled_past_end, LV_EVENT_SCROLL, NULL);
    for (unsigned i = 0; i < count; ++i) log_line(log, story->lines[i], i + 1 == count ? VALUE : LABEL);
    xSemaphoreGive(s_story_lock);
    if (!count) {
        if (!headed) log_line(log, "NO STORY YET.", DIM);
        log_line(log, "LONG PRESS YOUR PET.", DIM);
    }
    lv_obj_t *prompt = plain(log);
    lv_obj_set_size(prompt, lv_obj_get_style_width(log, 0), lv_font_get_line_height(FONT_SMALL));
    text(prompt, FONT_SMALL, ">", 0, 0, DIM, 0);
    s_cursor = plain(prompt);
    lv_obj_set_pos(s_cursor, 14, 1);
    lv_obj_set_size(s_cursor, 6, 11);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cursor, shade(s_cursor, VALUE), 0);
    reveal(log);
    lv_obj_update_layout(log);
    lv_obj_scroll_to_y(log, LV_COORD_MAX, LV_ANIM_OFF);
}

/* The title of the current story of the pet on screen, as the menu face
 * draws it, or "" for none. False until a story.list came for that pet:
 * then the gateway keeps story mode, and the deck shows STORY. */
static bool current_story(char *title_out, size_t capacity)
{
    bool listed = false;
    title_out[0] = 0;
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    const story_t *story = s_story;
    if (story && !strcmp(story->list_owner, s_device_status.pack_name)) {
        listed = true;
        for (unsigned i = 0; i < story->list.count; ++i)
            if (!strcmp(story->list.stories[i].id, story->list.current))
                display_text(title_out, capacity, story->list.stories[i].title, PET_STORY_LIST_TITLE_BYTES, true);
    }
    xSemaphoreGive(s_story_lock);
    return listed;
}

static void stories_clicked(lv_event_t *event)
{
    (void)event;
    s_restart_armed = false;
    show(PAGE_STORIES);
}

/* STORY: the current story, by its title; a tap lists the owner's stories. */
static void story_card(int y)
{
    lv_obj_t *face = card(s_menu, CARD_X, y, CARD_W, 56, CARD_PRESS);
    const int32_t width = lv_obj_get_style_width(face, 0);
    lv_obj_add_event_cb(face, stories_clicked, LV_EVENT_CLICKED, NULL);
    pixel_icon(face, &pet_icon_star, 2, 7, 16, VALUE);
    lv_obj_t *rule = plain(face);
    lv_obj_set_pos(rule, 38, 9);
    lv_obj_set_size(rule, 1, 38);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, shade(rule, EDGE), 0);
    text(face, FONT_SMALL, "STORY", 46, 9, LABEL, 2);
    s_story_value = text(face, FONT_TEXT, "", 46, 27, VALUE, 0);
    lv_obj_set_size(s_story_value, width - 46 - 26, 19);
    lv_label_set_long_mode(s_story_value, LV_LABEL_LONG_DOT);
    icon(face, &pet_icon_chevron_right, width - 16, 21, DIM);
}

/* After a request from STORIES: the pet shows, or the line says why not. */
static void story_sent(esp_err_t sent)
{
    if (sent == ESP_OK) leave_to_pet();
    else status_text(sent == ESP_ERR_INVALID_STATE ? "OFFLINE - TRY AGAIN" : "BUSY - TRY AGAIN");
}

/* A tap on another story loads it (story.load); the one loaded stays. */
static void story_picked(lv_event_t *event)
{
    const unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    const pet_story_list_t *shown = &s_story->shown;
    s_restart_armed = false;
    if (index >= shown->count) return;
    if (!strcmp(shown->stories[index].id, shown->current)) {
        status_text("ALREADY LOADED");
        return;
    }
    story_sent(s_control.story("story.load", shown->stories[index].id));
}

/* RESTART wipes the story's memory, so it asks first: a second tap within
 * RESTART_CONFIRM_MS starts the story over (story.restart). */
static void restart_story_clicked(lv_event_t *event)
{
    (void)event;
    if (!s_restart_armed) {
        s_restart_armed = true;
        s_restart_armed_at = lv_tick_get();
        show_page(PAGE_STORIES, false);
        return;
    }
    s_restart_armed = false;
    const esp_err_t sent = s_control.story("story.restart", NULL);
    if (sent != ESP_OK) show_page(PAGE_STORIES, false);
    story_sent(sent);
}

/* STORIES: the owner's stories, newest first, the current one lit, and
 * RESTART for the current one. */
static void stories_page(void)
{
    title(s_screen, "STORIES", 26);
    lv_obj_t *list = plain(s_screen);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    page_box(list, 36, 80, 288, 162);
    paper(list, false);
    const int32_t width = lv_obj_get_style_width(list, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    reveal(list);
    atomic_store(&s_story_fresh, false); /* this page reads the newest list */
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    if (s_story) {
        const bool listed = !strcmp(s_story->list_owner, s_device_status.pack_name);
        s_story->shown = listed ? s_story->list : (pet_story_list_t){0};
    }
    xSemaphoreGive(s_story_lock);
    const pet_story_list_t *shown = s_story ? &s_story->shown : NULL;
    const unsigned count = shown ? shown->count : 0;
    const bool animate = s_animate;
    s_animate = false; /* the list prints as one */
    for (unsigned i = 0; i < count; ++i) {
        char name[PET_STORY_LIST_TITLE_BYTES + 1];
        display_text(name, sizeof(name), shown->stories[i].title, PET_STORY_LIST_TITLE_BYTES, true);
        lv_obj_t *item = card(list, 0, (int32_t)i * 42, width, 38, CARD_PRESS | CARD_ROW);
        lv_obj_t *caption = text(item, FONT_TEXT, name, 14, 9, VALUE, 0);
        lv_obj_set_size(caption, width - 28, 19);
        lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
        set_lit(item, !strcmp(shown->stories[i].id, shown->current));
        lv_obj_add_event_cb(item, story_picked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    s_animate = animate;
    if (!count) centered(list, FONT_SMALL, "NO STORIES YET", 64, width, DIM, 2);
    const bool restartable = count && shown->current[0];
    s_restart_armed = s_restart_armed && restartable;
    if (restartable)
        command(s_screen, s_restart_armed ? "CONFIRM" : "RESTART", &pet_icon_sync, 100, 250, 160, 38,
                restart_story_clicked);
    status_line(s_screen, 60, s_restart_armed ? "WIPES THE STORY'S MEMORY" : count ? "TAP ONE TO LOAD IT" : "");
    if (s_restart_armed) type_status(s_hint, WARN, false);
    handle();
}

/* Builds in the look of `style`, a PET_MENU_STYLE_*: the glass at once, the
 * story frame's tab again, and each page as it opens. */
static void use_theme(uint8_t style)
{
    s_theme = s_themes[style < PET_MENU_STYLES ? style : PET_MENU_STYLE_TERMINAL];
    if (s_display_screen) lv_obj_set_style_bg_color(s_display_screen, shade(s_display_screen, GLASS), 0);
    if (s_screen) {
        lv_obj_set_style_bg_color(s_screen, shade(s_screen, GLASS), 0);
        lv_obj_set_style_text_font(s_screen, FONT_TEXT, 0);
        lv_obj_set_style_text_color(s_screen, shade(s_screen, VALUE), 0);
    }
    redraw_story_frame();
}

/* A tap on STYLE switches the menu's look. Once the device can keep it, the
 * deck is built again in it, scrolled as it was, so STYLE stays under the
 * finger. */
static void style_clicked(lv_event_t *event)
{
    (void)event;
    const uint8_t next = s_theme == &s_terminal ? PET_MENU_STYLE_WOODLAND : PET_MENU_STYLE_TERMINAL;
    if (!s_control.style(next)) {
        status_text("BUSY - TRY AGAIN");
        return;
    }
    const int32_t scrolled = lv_obj_get_scroll_y(s_menu);
    use_theme(next);
    show_page(PAGE_HOME, false);
    lv_obj_update_layout(s_menu);
    lv_obj_scroll_to_y(s_menu, scrolled, LV_ANIM_OFF);
}

/* STYLE: the menu's look, by name; a tap switches it. */
static void style_card(int y)
{
    lv_obj_t *face = card(s_menu, CARD_X, y, CARD_W, 56, CARD_PRESS);
    const int32_t width = lv_obj_get_style_width(face, 0);
    lv_obj_add_event_cb(face, style_clicked, LV_EVENT_CLICKED, NULL);
    icon(face, &pet_icon_palette, 8, 17, VALUE);
    lv_obj_t *rule = plain(face);
    lv_obj_set_pos(rule, 38, 9);
    lv_obj_set_size(rule, 1, 38);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, shade(rule, EDGE), 0);
    text(face, FONT_SMALL, "STYLE", 46, 9, LABEL, 2);
    text(face, FONT_TEXT, s_theme->name, 46, 27, VALUE, 0);
    icon(face, &pet_icon_sync, width - 30, 18, DIM);
}

/* The deck: title, status, cards, then the system log further down. */
static void home_page(void)
{
    s_menu = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, s_width, s_height);
    lv_obj_set_scroll_dir(s_menu, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_menu, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_menu, scrolled_past_end, LV_EVENT_SCROLL, NULL);
    lv_obj_t *pill = plain(s_menu);
    lv_obj_set_pos(pill, (s_width - 40) / 2, 12);
    lv_obj_set_size(pill, 40, 4);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pill, shade(pill, LABEL), 0);
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
    s_wifi_icon = icon(wifi, &pet_icon_wifi, 10, 22, VALUE);
    lv_obj_t *rule = plain(wifi);
    lv_obj_set_pos(rule, 44, 10);
    lv_obj_set_size(rule, 1, 44);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, shade(rule, EDGE), 0);
    text(wifi, FONT_SMALL, "WI-FI", 52, 9, LABEL, 2);
    s_wifi_name = text(wifi, FONT_TEXT, "", 52, 24, VALUE, 0);
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
    s_battery_value = text(battery, FONT_BIG, "", 46, 25, VALUE, 0);

    int y = 154;
    if (s_control.volume) { setting_card(SETTING_VOLUME, y); y += 64; }
    if (s_control.brightness) { setting_card(SETTING_BRIGHTNESS, y); y += 64; }
    if (s_control.style) {
        style_card(y);
        y += 62;
    }

    lv_obj_t *voice = card(s_menu, CARD_X, y, CARD_W, 56, CARD_PLAIN);
    icon(voice, &pet_icon_mic, 10, 16, VALUE);
    rule = plain(voice);
    lv_obj_set_pos(rule, 36, 9);
    lv_obj_set_size(rule, 1, 38);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, shade(rule, EDGE), 0);
    text(voice, FONT_SMALL, "VOICE", 44, 9, LABEL, 2);
    s_voice_value = text(voice, FONT_TEXT, "", 44, 27, VALUE, 0);
    y += 62;
    pet_card(y);
    y += 62;
    char story[PET_STORY_LIST_TITLE_BYTES + 1];
    if (s_control.story && current_story(story, sizeof(story))) {
        story_card(y);
        y += 62;
    }

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
    lv_obj_set_style_bg_color(line, shade(line, EDGE), 0);
    y += 24;
    /* The log on paper, its sheet around its lines. */
    const int32_t reach = paper_reach(false), width = s_width - 2 * page_x(LOG_X);
    lv_obj_t *log = plain(s_menu);
    lv_obj_set_pos(log, page_x(LOG_X) - reach, y - reach);
    lv_obj_set_size(log, width + 2 * reach, 15 * LOG_LINES + 2 * reach);
    paper(log, false);
    for (unsigned i = 0; i < LOG_LINES; ++i) {
        s_log[i] = text(log, FONT_SMALL, "", 0, 15 * (int32_t)i, DIM, 0);
        /* One row each: a line too long for the screen ends in dots. */
        lv_obj_set_size(s_log[i], width, lv_font_get_line_height(FONT_SMALL));
        lv_label_set_long_mode(s_log[i], LV_LABEL_LONG_DOT);
    }
    s_cursor = plain(log);
    lv_obj_set_pos(s_cursor, 12, 15 * (LOG_LINES - 1) + 1);
    lv_obj_set_size(s_cursor, 6, 11);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cursor, shade(s_cursor, VALUE), 0);
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
    s_cursor = s_bezel = s_tube[0] = s_tube[1] = s_tube[2] = s_story_value = NULL;
    if (page != PAGE_STORIES) s_restart_armed = false;
    memset(s_code_cell, 0, sizeof(s_code_cell));
    memset(s_pet_cell, 0, sizeof(s_pet_cell));
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
    else if (page == PAGE_BLESSINGS) blessings_page();
    else if (page == PAGE_CHARACTER) character_page();
    else if (page == PAGE_STORY) story_page();
    else if (page == PAGE_STORIES) stories_page();
    else if (page == PAGE_LIBRARY) {
#if CONFIG_PET_VNEXT_DEVELOPER_LIBRARY
        title(s_screen,"PETS",26);
        s_library_dropdown=lv_dropdown_create(s_screen);
        lv_obj_remove_style_all(s_library_dropdown);
        page_box(s_library_dropdown, 50, 84, 260, 38);
        lv_obj_set_style_text_font(s_library_dropdown,FONT_TEXT,0);
        lv_obj_set_style_text_color(s_library_dropdown,shade(s_library_dropdown, VALUE),0);
        lv_obj_set_style_radius(s_library_dropdown,3,0);
        lv_obj_set_style_bg_opa(s_library_dropdown,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(s_library_dropdown,shade(s_library_dropdown, FACE),0);
        lv_obj_set_style_border_width(s_library_dropdown,1,0);
        lv_obj_set_style_border_color(s_library_dropdown,shade(s_library_dropdown, EDGE),0);
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
        lv_obj_t *heading=text(info,FONT_TEXT,name,14,10,VALUE,1);
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
        else if (request->action == PET_USB_WIFI_FORGET)
        {
            esp_err_t forgotten = pet_config_forget_wifi(request->ssid);
            if (forgotten == ESP_ERR_NOT_FOUND) error = "SAVED_NETWORK_MISSING";
            else if (forgotten == ESP_ERR_INVALID_STATE) error = "LAST_NETWORK";
            else if (forgotten != ESP_OK) error = "SAVE_FAILED";
            else
            {
                /* The device stays on its network; later attempts use the new primary and fallback. */
                pet_network_refresh_wifi();
                cJSON_AddStringToObject(reply, "status", "forgotten");
                cJSON_AddStringToObject(reply, "mac", mac);
            }
        }
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
        if (saved_settings && delivered)
        {
            if (!s_control.join_wifi || !s_control.join_wifi())
            {
                if (s_control.restart) s_control.restart();
            }
        }
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
            if (err == ESP_OK && s_control.join_wifi && s_control.join_wifi()) {
                if (pet_display_lock(-1) == ESP_OK) {
                    status_text("JOINING WIFI");
                    pet_display_unlock();
                }
                continue;
            }
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
    s_story_lock = xSemaphoreCreateMutex();
    if (!s_commands || !s_story_lock) return ESP_ERR_NO_MEM;
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
    use_theme(config->menu_style);
    s_display_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_display_screen);
    lv_obj_set_style_bg_color(s_display_screen, shade(s_display_screen, GLASS), 0);
    lv_obj_set_style_bg_opa(s_display_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_display_screen, LV_OBJ_FLAG_SCROLLABLE);
    s_screen = lv_obj_create(s_display_screen);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, s_width, s_height);
    lv_obj_center(s_screen);
    lv_obj_set_style_bg_color(s_screen, shade(s_screen, GLASS), 0);
    lv_obj_set_style_text_font(s_screen, FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_screen, shade(s_screen, VALUE), 0);
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
       !memchr(status->pet_name,0,sizeof(status->pet_name))||
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

void pet_onboarding_open_screen(pet_onboarding_screen_t screen)
{
    static const page_t pages[] = {PAGE_BLESSINGS, PAGE_CHARACTER, PAGE_STORY};
    if (!s_screen || (unsigned)screen >= sizeof(pages) / sizeof(pages[0])) return;
    show_page(pages[screen], true);
    lv_screen_load(s_display_screen);
    crt_on();
}

/* The story's PSRAM copy, made the first time; under s_story_lock. */
static story_t *story_copy(void)
{
    if (!s_story) s_story = heap_caps_calloc(1, sizeof(*s_story), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_story;
}

void pet_onboarding_set_story_log(const char *face_id, const char *const *lines, size_t count)
{
    if (!s_story_lock || !face_id || count > PET_STORY_LOG_LINES || (count && !lines)) return;
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    story_t *story = story_copy();
    if (story) {
        strlcpy(story->log_owner, face_id, sizeof(story->log_owner));
        story->count = (uint8_t)count;
        for (size_t i = 0; i < count; ++i)
            display_text(story->lines[i], sizeof(story->lines[i]), lines[i], PET_STORY_LOG_LINE_BYTES, false);
    }
    xSemaphoreGive(s_story_lock);
    atomic_store(&s_story_fresh, true);
}

void pet_onboarding_set_story_state(const char *face_id, const pet_story_state_t *state)
{
    if (!s_story_lock || !face_id || !state) return;
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    story_t *story = story_copy();
    if (story) {
        strlcpy(story->state_owner, face_id, sizeof(story->state_owner));
        story->state = *state;
    }
    xSemaphoreGive(s_story_lock);
    atomic_store(&s_story_detour, state->detour);
    atomic_store(&s_story_hurry, state->hurry);
    atomic_store(&s_story_fresh, true);
}

void pet_onboarding_set_story_list(const char *face_id, const pet_story_list_t *list)
{
    if (!s_story_lock || !face_id || !list || list->count > PET_STORY_LIST_LENGTH) return;
    xSemaphoreTake(s_story_lock, portMAX_DELAY);
    story_t *story = story_copy();
    if (story) {
        strlcpy(story->list_owner, face_id, sizeof(story->list_owner));
        story->list = *list;
    }
    xSemaphoreGive(s_story_lock);
    atomic_store(&s_story_fresh, true);
}

/* ---- The story frame ---------------------------------------------------
 * Over the pet in story mode: a rim just inside the glass's edge, and a STORY
 * tab at the top, as the theme draws them: in TERMINAL one pixel-art pixel
 * wide, lime with a dark outline; in WOODLAND two, carved wood with a knot in
 * each corner. For now the rim is green, yellow or red by the story's path
 * (path_color). It is one object, drawn again only when its style or the path
 * changes. Where the pet's own frames repaint it, it draws only the rim's
 * rectangles that touch the rows being repainted, as plain fills. */
#define FRAME_PIXEL 3 /* the pixel art's pixel, as BLESSINGS draws its icons */
#define FRAME_INSET 3 /* the rim's outer edge, in from the glass's */
#define FRAME_CUT 24  /* a rectangular glass's corners, cut at 45 degrees */

static bool overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* One rectangle of the rim, grown by `grow` pixels for its outline. */
static void frame_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *look, int32_t grow, int32_t x1, int32_t y1,
                       int32_t x2, int32_t y2)
{
    const lv_area_t area = {x1 - grow, y1 - grow, x2 + grow, y2 + grow};
    if (overlaps(&area, &layer->_clip_area))
        lv_draw_rect(layer, look, &area);
}

/* One run of the ring's rows that share their extent: a span from `out`
 * to `in` either side of the centre, or across where `in` is 0. */
static void ring_run(lv_layer_t *layer, const lv_draw_rect_dsc_t *look, int32_t grow, int32_t cx, int32_t y1,
                     int32_t y2, int32_t out, int32_t in)
{
    if (in <= 0) {
        frame_rect(layer, look, grow, cx - out, y1, cx + out - 1, y2);
        return;
    }
    frame_rect(layer, look, grow, cx - out, y1, cx - in - 1, y2);
    frame_rect(layer, look, grow, cx + in, y1, cx + out - 1, y2);
}

/* The round glass: a ring `width` px wide, `inset` px in from the glass's
 * edge, stepped a pixel-art pixel at a time. Each row of pixels runs from the
 * ring's inner edge to its outer edge, and rows that share their extent are
 * drawn as one rectangle, so the sides are long runs. */
static void round_rim(lv_layer_t *layer, const lv_draw_rect_dsc_t *look, int32_t grow, const lv_area_t *box,
                      int32_t inset, int32_t width)
{
    const int32_t p = FRAME_PIXEL, cx = (box->x1 + box->x2 + 1) / 2, cy = (box->y1 + box->y2 + 1) / 2;
    const int32_t outer = (box->x2 - box->x1 + 1) / 2 / p * p - inset / p * p, inner = outer - width;
    const int32_t top = cy - outer, rows = 2 * outer / p;
    const lv_area_t *clip = &layer->_clip_area;
    const int32_t first = LV_MAX(0, (clip->y1 - grow - top) / p), last = LV_MIN(rows - 1, (clip->y2 + grow - top) / p);
    int32_t run_y = top + first * p, run_out = -1, run_in = -1;
    for (int32_t row = first; row <= last; ++row) {
        const int32_t y = top + row * p, middle = y + p / 2 - cy, rise = middle < 0 ? -middle : middle;
        const int32_t out = (int32_t)sqrtf((float)(outer * outer - rise * rise)) / p * p;
        int32_t in = rise < inner ? ((int32_t)sqrtf((float)(inner * inner - rise * rise)) + p - 1) / p * p : 0;
        if (in > out - p) in = out - p;
        if (out == run_out && in == run_in) continue;
        if (run_out >= 0) ring_run(layer, look, grow, cx, run_y, y - 1, run_out, run_in);
        run_y = y;
        run_out = out;
        run_in = in;
    }
    if (run_out >= 0) ring_run(layer, look, grow, cx, run_y, top + last * p + p - 1, run_out, run_in);
}

/* A rectangular glass: four edges `width` px wide, `inset` px in from the
 * glass's, the corners cut at 45 degrees in pixel steps, inside a rounded
 * glass's corners. */
static void square_rim(lv_layer_t *layer, const lv_draw_rect_dsc_t *look, int32_t grow, const lv_area_t *box,
                       int32_t inset, int32_t width)
{
    const int32_t p = FRAME_PIXEL, c = FRAME_CUT, w = width;
    const int32_t x1 = box->x1 + inset, y1 = box->y1 + inset;
    const int32_t x2 = box->x2 - inset, y2 = box->y2 - inset;
    frame_rect(layer, look, grow, x1 + c, y1, x2 - c, y1 + w - 1);
    frame_rect(layer, look, grow, x1 + c, y2 - w + 1, x2 - c, y2);
    frame_rect(layer, look, grow, x1, y1 + c, x1 + w - 1, y2 - c);
    frame_rect(layer, look, grow, x2 - w + 1, y1 + c, x2, y2 - c);
    for (int32_t step = p; step < c; step += p) {
        const int32_t left = x1 + c - step, right = x2 - c + step;
        frame_rect(layer, look, grow, left, y1 + step, left + w - 1, y1 + step + p - 1);
        frame_rect(layer, look, grow, right - w + 1, y1 + step, right, y1 + step + p - 1);
        frame_rect(layer, look, grow, left, y2 - step - p + 1, left + w - 1, y2 - step);
        frame_rect(layer, look, grow, right - w + 1, y2 - step - p + 1, right, y2 - step);
    }
}

/* For now, in both styles, the rim's colour for where the pet is: green on
 * the story's path, yellow on a detour (s_frame_detour), red hurrying back
 * (s_frame_hurry). */
static lv_color_t path_color(lv_obj_t *obj)
{
    return s_frame_hurry    ? lv_color_hex(HURRY_RED)
           : s_frame_detour ? lv_color_hex(DETOUR_YELLOW)
                            : shade(obj, GOOD);
}

/* TERMINAL's rim: the dark outline first, then the pixels over it in the
 * path's colour (lime, its green, on the path). */
static void draw_story_frame(lv_event_t *event)
{
    lv_obj_t *frame = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t box;
    lv_obj_get_coords(frame, &box);
    lv_draw_rect_dsc_t look;
    lv_draw_rect_dsc_init(&look);
    for (int32_t grow = 1; grow >= 0; --grow) {
        look.bg_color = grow ? shade(frame, GLASS) : path_color(frame);
        if (pet_board_current()->round) round_rim(layer, &look, grow, &box, FRAME_INSET, FRAME_PIXEL);
        else square_rim(layer, &look, grow, &box, FRAME_INSET, FRAME_PIXEL);
    }
}

/* TERMINAL's tab: STORY, dark on the rim's colour, at the top of the rim. */
static void story_tab(lv_obj_t *frame)
{
    lv_obj_t *tab = plain(frame);
    lv_obj_set_size(tab, text_width(FONT_SMALL, "STORY", 2) + 4 * FRAME_PIXEL, lv_font_get_line_height(FONT_SMALL) + 2);
    lv_obj_align(tab, LV_ALIGN_TOP_MID, 0, FRAME_INSET);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tab, path_color(tab), 0);
    lv_obj_set_style_border_width(tab, 1, 0);
    lv_obj_set_style_border_color(tab, shade(tab, GLASS), 0);
    lv_obj_center(text(tab, FONT_SMALL, "STORY", 0, 0, ON_LIT, 2));
}

/* A diamond of pixel-art pixels centred on (x, y), `reach` pixels from its
 * middle to each tip, at `grow` for its outline. */
static void diamond(lv_layer_t *layer, const lv_draw_rect_dsc_t *look, int32_t grow, int32_t x, int32_t y,
                    int32_t reach)
{
    const int32_t p = FRAME_PIXEL;
    for (int32_t row = -reach; row <= reach; ++row) {
        const int32_t across = reach - LV_ABS(row);
        frame_rect(layer, look, grow, x - across * p - p / 2, y + row * p - p / 2, x + across * p + p / 2,
                   y + row * p + p / 2);
    }
}

/* A knot carved where the rim turns: a diamond of wood, lit at its rim, with
 * a brass stud at its heart. */
static void knot(lv_layer_t *layer, lv_draw_rect_dsc_t *look, int32_t grow, int32_t x, int32_t y)
{
    if (grow) {
        look->bg_color = lv_color_hex(WOOD_OUTLINE);
        diamond(layer, look, 1, x, y, 2);
        return;
    }
    static const uint32_t rings[] = {WOOD_LIGHT, WOOD_FRAME, BRASS};
    for (int32_t reach = 2; reach >= 0; --reach) {
        look->bg_color = lv_color_hex(rings[2 - reach]);
        diamond(layer, look, 0, x, y, reach);
    }
}

/* WOODLAND's rim: three pixel-art pixels of carved wood in a dark outline,
 * lit at the outside and shaded toward the pet, and a knot at each corner. */
static void wood_frame(lv_event_t *event)
{
    lv_obj_t *frame = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t box;
    lv_obj_get_coords(frame, &box);
    lv_draw_rect_dsc_t look;
    lv_draw_rect_dsc_init(&look);
    const bool round = pet_board_current()->round;
    const int32_t p = FRAME_PIXEL;
    /* From the outside in, each band over the last: the outline, then shade,
     * wood and light, each a pixel-art pixel narrower. For now the light,
     * the band always in view, is the path's colour. */
    const struct { int32_t grow, pixels; lv_color_t color; } bands[] = {
        {1, 3, lv_color_hex(WOOD_OUTLINE)}, {0, 3, lv_color_hex(WOOD_SHADE)}, {0, 2, lv_color_hex(WOOD_FRAME)},
        {0, 1, path_color(frame)},
    };
    for (unsigned i = 0; i < sizeof(bands) / sizeof(bands[0]); ++i) {
        look.bg_color = bands[i].color;
        if (round) round_rim(layer, &look, bands[i].grow, &box, FRAME_INSET, bands[i].pixels * p);
        else square_rim(layer, &look, bands[i].grow, &box, FRAME_INSET, bands[i].pixels * p);
    }
    /* The corners: on the round glass where its diagonals cross the rim; on
     * a rectangle, the middle of each corner's cut. */
    const int32_t cx = (box.x1 + box.x2 + 1) / 2, cy = (box.y1 + box.y2 + 1) / 2;
    const int32_t middle = (box.x2 - box.x1 + 1) / 2 / p * p - FRAME_INSET / p * p - 3 * p / 2;
    const int32_t dx = round ? middle * 707 / 1000 : cx - box.x1 - FRAME_INSET - FRAME_CUT / 2 - 3 * p / 2;
    const int32_t dy = round ? dx : cy - box.y1 - FRAME_INSET - FRAME_CUT / 2;
    for (int32_t grow = 1; grow >= 0; --grow) {
        knot(layer, &look, grow, cx - dx, cy - dy);
        knot(layer, &look, grow, cx + dx, cy - dy);
        knot(layer, &look, grow, cx - dx, cy + dy);
        knot(layer, &look, grow, cx + dx, cy + dy);
    }
}

/* A small board: planks in a one-pixel bevel, outlined. */
static void draw_board(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &a);
    border(layer, lv_color_hex(WOOD_OUTLINE), &a, 1);
    lv_area_increase(&a, -1, -1);
    bevel(layer, &a, 1, lv_color_hex(WOOD_FRAME), lv_color_hex(WOOD_LIGHT), lv_color_hex(WOOD_SHADE));
    lv_area_increase(&a, -1, -1);
    tiles(layer, &pet_wood_planks, &a);
}

/* WOODLAND's tab: STORY in gold on a small board at the top of the rim. */
static void wood_tab(lv_obj_t *frame)
{
    lv_obj_t *tab = plain(frame);
    lv_obj_set_size(tab, text_width(FONT_SMALL, "STORY", 2) + 6 * FRAME_PIXEL, lv_font_get_line_height(FONT_SMALL) + 8);
    lv_obj_align(tab, LV_ALIGN_TOP_MID, 0, FRAME_INSET);
    lv_obj_add_event_cb(tab, draw_board, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_center(text(tab, FONT_SMALL, "STORY", 0, 0, LIT, 2));
}

/* The frame in the menu's style as it is now. */
static void frame_drawn(lv_event_t *event)
{
    s_theme->draw_frame(event);
}

static void frame_deleted(lv_event_t *event)
{
    if (lv_event_get_current_target_obj(event) == s_story_frame) s_story_frame = NULL;
}

lv_obj_t *pet_onboarding_story_frame(lv_obj_t *parent)
{
    lv_obj_t *frame = plain(parent);
    lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(frame, frame_drawn, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(frame, frame_deleted, LV_EVENT_DELETE, NULL);
    s_theme->frame_tab(frame);
    s_story_frame = frame;
    return frame;
}
