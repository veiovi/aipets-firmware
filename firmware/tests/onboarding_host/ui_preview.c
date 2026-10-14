/* Design preview: the real menu pages rendered by real LVGL on the host into
 * native board frames (PPM), for review before anything is flashed. Hardware and
 * RTOS are the same seams as ui_test.c. usage: ui_preview <output-directory>
 * [<pet.rgb565> [terminal|woodland]], the second a square RGB565 picture of a
 * pet as wide as the glass's longer side, which the story frame is drawn
 * over, and the third the menu's style, as the device keeps it. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "src/misc/lv_timer_private.h"
#include "../../main/pet_onboarding.c"
#include "../../main/pet_touch.c"

static lv_indev_t *touch_input;
static struct esp_lcd_touch_fixture touch_hardware = {.config = {.int_gpio_num = 5, .levels = {.interrupt = 0}}};
static touch_adapter_context_prefix_t touch_context = {
    .handle = &touch_hardware, .scale = {.x = 1.0f, .y = 1.0f}, .with_irq = true, .touch_sem = (void *)1};
static command_t last_command;
static uint8_t draw_buffer[360 * 12 * 2] __attribute__((aligned(16)));
static uint16_t frame[410 * 502];
static bool online;

void pet_diagnostics_note_native_ui_heartbeat(void) {}
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t app = {.version = "aipet-8.0.0-240"}; return &app; }
static bool library_request(bool next) { (void)next; return true; }
static bool select_request(const char *build) { (void)build; return true; }
static void return_request(void) {}
static bool retry_request(void) { return true; }
static bool restart_request(void) { return true; }
static bool setting_request(uint8_t value, bool save) { (void)value; (void)save; return true; }
static bool switch_request(int direction) { (void)direction; return true; }
static esp_err_t story_request(const char *type, const char *id) { (void)type; (void)id; return ESP_OK; }
static bool blessing_request(const char *id, bool speak) { (void)id; (void)speak; return true; }
static bool style_request(uint8_t style) { (void)style; return true; }
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) { (void)caps; return calloc(count, size); }
QueueHandle_t xQueueCreate(unsigned count, unsigned size) { (void)count; (void)size; return &last_command; }
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{ (void)queue; (void)ticks; last_command = *(const command_t *)item; return pdTRUE; }
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks) { (void)queue; (void)item; (void)ticks; return 0; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void esp_restart(void) { abort(); }
/* The touch interrupt's semaphore (void *)1 never signals; mutexes are free. */
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) { (void)ticks; return semaphore != (void *)1; }
int xSemaphoreGive(SemaphoreHandle_t semaphore) { (void)semaphore; return pdTRUE; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)2; }
int gpio_get_level(gpio_num_t gpio_num) { (void)gpio_num; return 1; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t handle, esp_lcd_touch_point_data_t *points,
                                 uint8_t *count, uint8_t capacity)
{ (void)handle; (void)points; (void)capacity; *count = 0; return ESP_OK; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int32_t y = area->y1; y <= area->y2; ++y)
        for (int32_t x = area->x1; x <= area->x2; ++x) frame[y * pet_board_current()->width + x] = *source++;
    lv_display_flush_ready(display);
}
lv_display_t *pet_display_start(void)
{
    const pet_board_profile_t *board = pet_board_current();
    lv_display_t *display = lv_display_create(board->width, board->height);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    touch_input = lv_indev_create();
    lv_indev_set_type(touch_input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_driver_data(touch_input, &touch_context);
    return display;
}
lv_indev_t *pet_display_input(void) { return touch_input; }
esp_err_t pet_display_lock(int timeout) { (void)timeout; return ESP_OK; }
void pet_display_unlock(void) {}
esp_err_t bsp_display_brightness_set(int brightness) { (void)brightness; return ESP_OK; }
bool pet_network_wifi_is_ready(void) { return online; }
esp_err_t pet_network_start_wifi_only(const pet_config_t *config) { (void)config; return ESP_OK; }
esp_err_t pet_network_scan(pet_network_scan_callback_t callback) { (void)callback; return ESP_OK; }
esp_err_t pet_config_recall_wifi(const char *ssid, char *password, size_t size)
{ (void)ssid; (void)password; (void)size; return ESP_FAIL; }
esp_err_t pet_config_store_wifi(const char *ssid, const char *password) { (void)ssid; (void)password; return ESP_OK; }
esp_err_t pet_setup_start(const char *origin, pet_setup_status_callback_t callback)
{ (void)origin; (void)callback; return ESP_OK; }
esp_err_t pet_setup_submit(const char *code) { (void)code; return ESP_OK; }
static pet_setup_link_callback_t link_view;
void pet_setup_on_link(pet_setup_link_callback_t callback) { link_view = callback; }
esp_err_t pet_setup_link(pet_setup_link_action_t action) { (void)action; return ESP_OK; }

/* Let animations and typing run for `ms`, then draw every pixel. */
static void settle(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) { lv_tick_inc(5); lv_timer_handler(); }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
}
static void save(const char *directory, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    const pet_board_profile_t *board = pet_board_current();
    fprintf(file, "P6\n%u %u\n255\n", board->width, board->height);
    for (size_t i = 0; i < (size_t)board->width * board->height; ++i) {
        uint16_t c = frame[i];
        uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                          (uint8_t)((c & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    printf("%s\n", path);
}

static lv_obj_t *find_text(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(obj, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

/* The ABOUT page's scrolling column: the parent of the font's license. */
static lv_obj_t *about_body(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_label_class) && lv_label_get_text(obj) == pet_menu_font_license)
        return lv_obj_get_parent(obj);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = about_body(lv_obj_get_child(obj, (int32_t)i));
        if (found) return found;
    }
    return NULL;
}

static void scroll_home(int32_t y)
{
    lv_obj_update_layout(s_screen);
    lv_obj_scroll_to_y(s_menu, y, LV_ANIM_OFF);
}

/* The pet's screen as the face shows it, a square picture centred on the
 * glass, with the story frame over it. */
static void frame_over_pet(const char *directory, const char *picture)
{
    const pet_board_profile_t *board = pet_board_current();
    const uint32_t side = board->width > board->height ? board->width : board->height;
    static lv_image_dsc_t pet = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565}};
    uint8_t *pixels = malloc(side * side * 2);
    FILE *file = fopen(picture, "rb");
    assert(pixels && file && fread(pixels, 2, side * side, file) == side * side);
    fclose(file);
    pet.header.w = pet.header.h = side;
    pet.header.stride = side * 2;
    pet.data_size = side * side * 2;
    pet.data = pixels;
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_t *image = lv_image_create(screen);
    lv_image_set_src(image, &pet);
    lv_obj_center(image);
    lv_obj_remove_flag(pet_onboarding_story_frame(screen), LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(screen);
    settle(100);
    save(directory, "frame");
    /* The same pet on a detour (story-pacing-v1). */
    const pet_story_state_t detour = {.story = true, .detour = true, .reached = 2, .of = 5};
    pet_onboarding_set_story_state("pablo", &detour);
    settle(200);
    save(directory, "frame-detour");
    /* Hurrying back to the story (story-hurry-v1). */
    const pet_story_state_t hurry = {.story = true, .hurry = true, .reached = 2, .of = 5};
    pet_onboarding_set_story_state("pablo", &hurry);
    settle(200);
    save(directory, "frame-hurry");
}

int main(int argc, char **argv)
{
    assert(argc >= 2 && argc <= 4);
    lv_init();
    const pet_onboarding_control_t controls = {.library = library_request, .select = select_request,
        .return_to_pet = return_request, .retry_sync = retry_request, .restart = restart_request,
        .volume = setting_request, .brightness = setting_request, .switch_pet = switch_request,
        .story = story_request, .use_blessing = blessing_request, .style = style_request};
    pet_onboarding_bind_control(&controls);
    pet_config_t config = {.brightness = 80, .volume = 64};
    if (argc == 4 && !strcmp(argv[3], "woodland")) config.menu_style = PET_MENU_STYLE_WOODLAND;
    strcpy(config.wifi_ssid, "HomeNet");
    assert(pet_onboarding_start(&config, "https://fixture.invalid") == ESP_OK);
    settle(1400);
    save(argv[1], "boot");
    finish_splash(NULL);

    /* A Pocket Terminal at home: one pet on screen, online
     * but not linked to the cloud, so he cannot talk yet. */
    s_paired = true;
    pet_onboarding_status_t status = {.wifi = PET_ONBOARDING_WIFI_CONNECTED, .cloud = PET_ONBOARDING_CLOUD_OFFLINE,
        .conversation = PET_ONBOARDING_CONVERSATION_UNAVAILABLE, .installation = PET_ONBOARDING_INSTALL_IDLE,
        .ssid = "HomeNet", .pack_name = "demo", .pack_version = "0.1.3", .build_id = "00000000-0000-4000-8000-0000000000a1",
        .firmware_version = "aipet-8.0.0-240", .battery_percent = 72, .battery_valid = true, .has_pet = true,
        .pet_capacity = 1, .pet_count = 1, .pet_index = 1, .pet_name = "DEMO", .volume = 64, .volume_valid = true,
        .brightness = 80, .brightness_valid = true};
    pet_onboarding_update_status(&status);
    show_page(PAGE_HOME, true);
    settle(1500);
    save(argv[1], "home");
    scroll_home(236);
    settle(100);
    save(argv[1], "home-cards");
    scroll_home(520);
    settle(100);
    save(argv[1], "home-log");
    scroll_home(LV_COORD_MAX); /* the end of the deck: the ABOUT button */
    settle(100);
    save(argv[1], "home-about");
    scroll_home(lv_obj_get_y(lv_obj_get_parent(s_pet_cell[0])) + 28 - s_height / 2); /* the pet picker, mid-glass */
    settle(100);
    save(argv[1], "home-pets");
    lv_obj_t *style = lv_obj_get_parent(find_text(s_menu, "STYLE"));
    scroll_home(lv_obj_get_y(style) + 28 - s_height / 2); /* the STYLE card, mid-glass */
    settle(100);
    save(argv[1], "deck-style");

    pet_wifi_network_t networks[4] = {{.ssid = "HomeNet", .rssi = -48, .secured = true, .saved = true},
        {.ssid = "Office-5G", .rssi = -63, .secured = true}, {.ssid = "Guest", .rssi = -71, .secured = false},
        {.ssid = "neighbour-network-with-long-name", .rssi = -84, .secured = true}};
    online = true;
    scan_finished(networks, 4, ESP_OK);
    show(PAGE_WIFI);
    settle(1500);
    save(argv[1], "wifi");

    strlcpy(s_selected_ssid, "Office-5G", sizeof(s_selected_ssid));
    s_hidden = false;
    show(PAGE_PASSWORD);
    lv_textarea_set_text(s_password, "example-pass");
    settle(1500);
    save(argv[1], "password");

    s_paired = s_enrolled = false;
    show(PAGE_CODE);
    lv_textarea_set_text(s_code, "A2B");
    settle(1500);
    save(argv[1], "code");

    /* The device's own link code, then the question only a tap answers; and
     * the same for a linked Pocket Terminal moving to another account. */
    s_paired = true;
    memset(&s_link, 0, sizeof(s_link));
    s_link.phase = PET_LINK_SHOWING;
    strcpy(s_link.code, "K7QX9MPA");
    strcpy(s_link.device, "1a2b3c");
    show(PAGE_LINK);
    settle(1500);
    save(argv[1], "link");
    s_link.phase = PET_LINK_CLAIMED;
    s_link.code[0] = 0;
    strcpy(s_link.account, "Alex\xe2\x80\x99s pets");
    strcpy(s_link.check, "472");
    show_page(PAGE_LINK, true);
    settle(1500);
    save(argv[1], "link-question");
    s_enrolled = s_link.moving = true;
    s_link.phase = PET_LINK_SHOWING;
    strcpy(s_link.code, "W2MN8QRT");
    s_link.account[0] = 0;
    show_page(PAGE_LINK, true);
    settle(1500);
    save(argv[1], "move-code");
    s_link.phase = PET_LINK_CLAIMED;
    strcpy(s_link.account, "Mia's pets");
    strcpy(s_link.check, "093");
    show_page(PAGE_LINK, true);
    settle(1500);
    save(argv[1], "move-question");

    s_paired = s_enrolled = true;
    show(PAGE_RESTART_CONFIRM);
    settle(1500);
    save(argv[1], "restart");

    /* ABOUT: the story, then scrolled to aipets.com and the open-source
     * notices under it. */
    show(PAGE_ABOUT);
    settle(1500);
    save(argv[1], "about");
    lv_obj_t *body = about_body(s_screen);
    assert(body);
    lv_obj_update_layout(s_screen);
    lv_obj_scroll_to_y(body, lv_obj_get_y(lv_obj_get_child(body, 1)) - 8, LV_ANIM_OFF);
    settle(100);
    save(argv[1], "about-notices");

    /* Story mode: the deck's STORY card, its STORIES page and RESTART's
     * confirmation. */
    pet_story_list_t list = {.count = 3};
    static const char *const titles[] = {"The Lantern of Thornspire", "Moonmirror Lake", "The Clockwork Orchard"};
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(list.stories[i].id, sizeof(list.stories[i].id), "6a3f0c2e-1d4b-4c8a-9e57-2b9d0f1a7c3%u", i);
        strcpy(list.stories[i].title, titles[i]);
    }
    strcpy(list.current, list.stories[0].id);
    pet_onboarding_set_story_list("demo", &list);
    show_page(PAGE_HOME, false);
    scroll_home(lv_obj_get_y(lv_obj_get_parent(s_story_value)) + 28 - s_height / 2);
    settle(100);
    save(argv[1], "deck-story");
    stories_clicked(NULL);
    settle(1500);
    save(argv[1], "stories");
    restart_story_clicked(NULL);
    settle(1500);
    save(argv[1], "stories-restart");

    /* The story screens a swipe on the pet opens: blessings and the character
     * sheet of a story that is no game, and the story with its header and six
     * turns, and with nothing yet. */
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);
    settle(1500);
    save(argv[1], "blessings");
    pet_onboarding_open_screen(PET_ONBOARDING_CHARACTER);
    settle(1500);
    save(argv[1], "character");

    /* A story played as a game: its blessings, one chosen, a spent one and
     * empty cells, and the hero's sheet with all four conditions. */
    pet_story_state_t game = {.story = true, .reached = 3, .of = 5, .has_hero = true, .hero = {10, 20, 6, 2, 10},
        .condition_count = 4, .has_blessings = true, .blessing_count = 7};
    strcpy(game.title, "The Lantern of Thornspire");
    strcpy(game.conditions[0], "Slowed");
    strcpy(game.conditions[1], "Moon-blessed");
    strcpy(game.conditions[2], "Poisoned");
    strcpy(game.conditions[3], "Brave");
    const pet_story_blessing_t blessings[] = {
        {"healing-light", "Healing Light", PET_BLESSING_HEART, 2, false},
        {"thunderstorm", "Thunderstorm", PET_BLESSING_STORM, 0, false},
        {"iron-skin", "Iron Skin", PET_BLESSING_SHIELD, 1, false},
        {"wishing-star", "Wishing Star", PET_BLESSING_STAR, 3, false},
        {"quick-feather", "Quick Feather", PET_BLESSING_FEATHER, 9, false},
        {"second-wind", "Second Wind", PET_BLESSING_POTION, 1, false},
        {"moon-blade", "Moon Blade", PET_BLESSING_SWORD, 4, false},
    };
    memcpy(game.blessings, blessings, sizeof(blessings));
    pet_onboarding_set_story_state("demo", &game);
    strcpy(s_blessing_id, "healing-light");
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);
    settle(1500);
    save(argv[1], "blessings-game");
    pet_onboarding_open_screen(PET_ONBOARDING_CHARACTER);
    settle(1500);
    save(argv[1], "character-game");
    static const char *const story[] = {"I spotted a purple swirl under Moonmirror Lake.",
        "I sent Grumble to fetch the silver key.", "I waded into the moonlit lake.",
        "I asked the dragon about the stolen key.", "I bargained with a toll troll and lost my hat...",
        "I jumped from the old bridge into the river."};
    pet_story_state_t state = {.story = true, .reached = 2, .of = 5};
    strcpy(state.title, "The Lantern of Thornspire");
    strcpy(state.goal, "the middle of Moonmirror Lake");
    pet_onboarding_set_story_state("demo", &state);
    pet_onboarding_set_story_log("demo", story, 6);
    pet_onboarding_open_screen(PET_ONBOARDING_STORY);
    settle(1500);
    save(argv[1], "story");
    state.title[0] = 0;
    pet_onboarding_set_story_state("demo", &state);
    pet_onboarding_set_story_log("demo", NULL, 0);
    pet_onboarding_open_screen(PET_ONBOARDING_STORY);
    settle(1500);
    save(argv[1], "story-empty");
    if (argc >= 3) frame_over_pet(argv[1], argv[2]);
    return 0;
}
