/* Real LVGL, fake hardware/RTOS. Include the UI to exercise event handlers
 * without making internal widget pointers part of the firmware's public API. */
#include <assert.h>
#include <stdlib.h>
#include "src/misc/lv_timer_private.h"
#define CONFIG_PET_VNEXT_DEVELOPER_LIBRARY 1
#include "../../main/pet_onboarding.c"
#include "../../main/pet_touch.c"
/* Kconfig leaves disabled options undefined. */
#ifndef CONFIG_PET_POCKET_TERMINAL
#define CONFIG_PET_POCKET_TERMINAL 0
#endif
#ifndef CONFIG_PET_DEVICE_RELINK
#define CONFIG_PET_DEVICE_RELINK 0
#endif

static bool online;
static lv_indev_t *touch_input;
static struct esp_lcd_touch_fixture touch_hardware={
    .config={.int_gpio_num=5,.levels={.interrupt=0}}};
static touch_adapter_context_prefix_t touch_context={
    .handle=&touch_hardware,.scale={.x=2.0f,.y=3.0f},.with_irq=true,
    .touch_sem=(void *)1};
static int touch_gpio_level=1;
static unsigned touch_semaphore_count,touch_reads;
static esp_err_t touch_read_result=ESP_OK;
static esp_lcd_touch_point_data_t touch_point={.x=20,.y=30};
static uint8_t touch_point_count;
static unsigned enqueued;
static command_t last_command;
static char last_code[PET_ENROLLMENT_CODE_BYTES];
static char selected_build[37];
static bool requested_next;
static unsigned returned;
static unsigned retries,restarts;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static char usb_output[1024], stored_password[65];
static bool usb_drained, usb_saving, store_fails;
static unsigned usb_stores;
bool usb_serial_jtag_is_driver_installed(void) { return true; }
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{ (void)config; return ESP_OK; }
int usb_serial_jtag_read_bytes(void *buffer, size_t length, TickType_t wait)
{ (void)buffer; (void)length; (void)wait; return 0; }
int usb_serial_jtag_write_bytes(const void *buffer, size_t length, TickType_t wait)
{
    (void)wait;
    assert(length < sizeof(usb_output));
    memcpy(usb_output, buffer, length);
    usb_output[length] = 0;
    usb_drained = false;
    return (int)length;
}
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t wait)
{ (void)wait; usb_drained = true; return ESP_OK; }
esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    (void)type;
    const uint8_t value[] = {0x2a, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(mac, value, sizeof(value));
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{ strcpy((char *)ap->ssid, "Joined network"); return ESP_OK; }
esp_err_t pet_config_list_wifi_networks(pet_wifi_network_metadata_t *out, size_t capacity, size_t *count)
{ assert(capacity > 0); strcpy(out[0].ssid, "Saved network"); *count = 1; return ESP_OK; }
esp_err_t pet_config_store_wifi_explicit(const char *ssid, const char *password)
{
    assert(!strcmp(ssid, "Open network"));
    strcpy(stored_password, password);
    ++usb_stores;
    return store_fails ? ESP_FAIL : ESP_OK;
}
static char forgotten_ssid[33];
static unsigned wifi_refreshes;
esp_err_t pet_config_forget_wifi(const char *ssid)
{
    if (!strcmp(ssid, "Only network")) return ESP_ERR_INVALID_STATE;
    strcpy(forgotten_ssid, ssid);
    return ESP_OK;
}
esp_err_t pet_network_refresh_wifi(void)
{
    ++wifi_refreshes;
    return ESP_OK;
}
#endif
static unsigned native_heartbeats;
static int volume_now=-1,volume_kept=-1,brightness_now=-1,brightness_kept=-1;
static lv_obj_t *other_screen;
void pet_diagnostics_note_native_ui_heartbeat(void) { ++native_heartbeats; }
const esp_app_desc_t *esp_app_get_description(void)
{static const esp_app_desc_t app={.version="aipet-6.0.2-240"};return &app;}
static bool library_request(bool next){requested_next=next;return true;}
static bool select_request(const char *build){strcpy(selected_build,build);return true;}
/* Like the firmware: the pet's screen takes over before this returns. */
static bool return_later,volume_fails;
/* Like the Pocket firmware, the pet's screen takes over before this returns;
 * with return_later, like builds that show it from another task. */
static void return_request(void){++returned;if(!return_later)lv_screen_load(other_screen);}
static bool retry_request(void){++retries;return true;}
static bool restart_request(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_saving) assert(usb_drained && strstr(usb_output, "\"status\":\"saved\""));
#endif
    ++restarts;
    return true;
}
static unsigned joins;
static bool join_fails;
static bool join_wifi_request(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_saving) assert(usb_drained && strstr(usb_output, "\"status\":\"saved\""));
#endif
    ++joins;
    return !join_fails;
}
static bool volume_request(uint8_t value,bool save)
{if(volume_fails)return false;if(save)volume_kept=value;else volume_now=value;return true;}
/* The deck's pet picker: the last switch asked for, +1 next or -1 previous. */
static int switched;
static unsigned switches;
static bool switch_fails;
static bool switch_request(int direction){if(switch_fails)return false;switched=direction;++switches;return true;}
/* The STORIES page's requests: the last one, and how the session answers. */
static char story_type[16],story_id[40];
static unsigned story_requests;
static esp_err_t story_result=ESP_OK;
static esp_err_t story_request(const char *type,const char *id)
{strcpy(story_type,type);strcpy(story_id,id?id:"");++story_requests;return story_result;}
/* BLESSINGS' USE: the blessing last used, whether it takes the owner's words,
 * and whether the request is taken. */
static char blessing_used[32];
static bool blessing_fails,blessing_spoken;
static bool blessing_request(const char *id,bool speak)
{if(blessing_fails)return false;strcpy(blessing_used,id);blessing_spoken=speak;return true;}
/* STYLE: the style last kept, and whether the device can keep one. */
static int kept_style = -1;
static bool style_fails;
static bool style_request(uint8_t style)
{
    if (style_fails) return false;
    kept_style = style;
    return true;
}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps){assert(caps&MALLOC_CAP_SPIRAM);return calloc(count,size);}
static bool brightness_request(uint8_t value,bool save){if(save)brightness_kept=value;else brightness_now=value;return true;}
static uint8_t draw_buffer[360 * 12 * 4] __attribute__((aligned(16)));
QueueHandle_t xQueueCreate(unsigned count, unsigned size) { (void)count; (void)size; return &last_command; }
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{ (void)queue; (void)ticks; last_command = *(const command_t *)item; ++enqueued; return pdTRUE; }
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{ (void)queue; (void)item; (void)ticks; return 0; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void esp_restart(void) { abort(); }
/* The touch interrupt's semaphore is (void *)1; mutexes are always free here. */
int xSemaphoreTake(SemaphoreHandle_t semaphore,TickType_t ticks)
{(void)ticks;if(semaphore!=(void *)1)return pdTRUE;if(!touch_semaphore_count)return 0;--touch_semaphore_count;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t semaphore){(void)semaphore;return pdTRUE;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void *)2;}
int gpio_get_level(gpio_num_t gpio_num){(void)gpio_num;return touch_gpio_level;}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t handle)
{(void)handle;++touch_reads;return touch_read_result;}
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t handle,
    esp_lcd_touch_point_data_t *points,uint8_t *count,uint8_t capacity)
{
    (void)handle;
    if(!capacity)return ESP_ERR_INVALID_ARG;
    *points=touch_point;*count=touch_point_count;return touch_read_result;
}
/* What the glass shows, RGB565, as the flushes leave it. */
static uint16_t shown[410 * 502];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int32_t y = area->y1; y <= area->y2; ++y)
        for (int32_t x = area->x1; x <= area->x2; ++x) shown[y * pet_board_current()->width + x] = *source++;
    lv_display_flush_ready(display);
}
lv_display_t *pet_display_start(void)
{
    const pet_board_profile_t *board = pet_board_current();
    lv_display_t *display = lv_display_create(board->width, board->height);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    touch_input=lv_indev_create();
    lv_indev_set_type(touch_input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_driver_data(touch_input,&touch_context);
    return display;
}
lv_indev_t *pet_display_input(void){return touch_input;}
esp_err_t pet_display_lock(int timeout) { (void)timeout; return ESP_OK; }
void pet_display_unlock(void) {}
esp_err_t bsp_display_brightness_set(int brightness) { (void)brightness; return ESP_OK; }
bool pet_network_wifi_is_ready(void) { return online; }
esp_err_t pet_network_start_wifi_only(const pet_config_t *config) { (void)config; return ESP_OK; }
esp_err_t pet_network_scan(pet_network_scan_callback_t callback) { (void)callback; return ESP_OK; }
esp_err_t pet_config_recall_wifi(const char *ssid, char *password, size_t size)
{ (void)ssid; (void)password; (void)size; return ESP_FAIL; }
esp_err_t pet_config_store_wifi(const char *ssid, const char *password)
{ (void)ssid; (void)password; return ESP_OK; }
esp_err_t pet_setup_start(const char *origin, pet_setup_status_callback_t callback)
{ (void)origin; (void)callback; return ESP_OK; }
esp_err_t pet_setup_submit(const char *code)
{
    if (!pet_enrollment_code_valid(code)) return ESP_ERR_INVALID_ARG;
    strcpy(last_code, code);
    return ESP_OK;
}
/* The setup worker's side of the link code: the screen's requests, and its
 * news, delivered like the worker does. */
static pet_setup_link_callback_t link_view;
static int last_link_action=-1;
static unsigned link_requests;
void pet_setup_on_link(pet_setup_link_callback_t callback){link_view=callback;}
esp_err_t pet_setup_link(pet_setup_link_action_t action){last_link_action=(int)action;++link_requests;return ESP_OK;}

static void run(uint32_t ms)
{
    for(uint32_t t=0;t<ms;t+=4){lv_tick_inc(4);lv_timer_handler();}
}

static void assert_glass_bounds(lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_update_layout(s_screen);
    lv_obj_get_coords(obj, &area);
    if (!pet_board_current()->round)
    {
        if (area.x1 < 0 || area.x2 >= pet_board_current()->width ||
            area.y1 < 0 || area.y2 >= pet_board_current()->height)
        {
            fprintf(stderr, "Page %d: control outside glass [%d,%d]-[%d,%d]\n",
                    s_page, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2);
        }
        assert(area.x1 >= 0 && area.x2 < pet_board_current()->width);
        assert(area.y1 >= 0 && area.y2 < pet_board_current()->height);
        return;
    }
    int xs[] = {area.x1, area.x2}, ys[] = {area.y1, area.y2};
    for (unsigned x = 0; x < 2; ++x)
        for (unsigned y = 0; y < 2; ++y)
            assert((xs[x]-180)*(xs[x]-180) + (ys[y]-180)*(ys[y]-180) <= 180*180);
}

/* Resting page controls fit the physical glass. Scrolling content is clipped. */
static void assert_page_on_glass(void)
{
    for(uint32_t i=0;i<lv_obj_get_child_count(s_screen);++i) {
        lv_obj_t *child=lv_obj_get_child(s_screen,(int32_t)i);
        if(child==s_bezel||child==s_tube[0]||child==s_tube[1]||child==s_tube[2]||child==s_menu||
           lv_obj_has_flag(child,LV_OBJ_FLAG_HIDDEN)||lv_obj_get_y(child)>=300)continue;
        assert_glass_bounds(child);
    }
}

/* The menu face has ASCII glyphs only: every label on the page can be drawn. */
static void assert_ascii(lv_obj_t *obj)
{
    if(lv_obj_check_type(obj,&lv_label_class)) {
        for(const unsigned char *p=(const unsigned char *)lv_label_get_text(obj);*p;++p)
            assert(*p=='\n'||(*p>=0x20&&*p<0x7f));
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i)assert_ascii(lv_obj_get_child(obj,(int32_t)i));
}

static unsigned keyboard_max_columns(lv_obj_t *keyboard)
{
    const char *const *map = lv_keyboard_get_map_array(keyboard);
    unsigned row = 0, maximum = 0;
    for (size_t i = 0; map[i][0]; ++i) {
        if (!strcmp(map[i], "\n")) {
            if (row > maximum) maximum = row;
            row = 0;
        } else ++row;
    }
    return row > maximum ? row : maximum;
}

static void press_keyboard_key(lv_obj_t *keyboard, const char *text)
{
    const char *const *map = lv_keyboard_get_map_array(keyboard);
    uint32_t button = 0;
    for (size_t i = 0; map[i][0]; ++i) {
        if (!strcmp(map[i], "\n")) continue;
        if (!strcmp(map[i], text)) {
            lv_buttonmatrix_set_selected_button(keyboard, button);
            lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
            return;
        }
        ++button;
    }
    assert(!"keyboard key not found");
}

/* A real finger through the touch reader and LVGL, in the glass's pixels:
 * down at the start, moved in steps, lifted. */
static void finger(int x0,int y0,int x1,int y1)
{
    touch_context.scale.x=touch_context.scale.y=1.0f;
    touch_read_result=ESP_OK;touch_gpio_level=0;touch_point_count=1;
    for(int step=0;step<=12;++step) {
        touch_point.x=(uint16_t)(x0+(x1-x0)*step/12);touch_point.y=(uint16_t)(y0+(y1-y0)*step/12);
        lv_tick_inc(8);lv_timer_handler();
    }
    touch_gpio_level=1;touch_point_count=0;
    run(80);
}

/* The same in the 360 px layout's coordinates. */
static void drag(int x0,int y0,int x1,int y1)
{
    finger(x0 * pet_board_current()->width / 360, y0 * pet_board_current()->height / 360,
           x1 * pet_board_current()->width / 360, y1 * pet_board_current()->height / 360);
}

static const char *log_text(unsigned line){return lv_label_get_text(s_log[line]);}

static lv_obj_t *find_text(lv_obj_t *obj,const char *text)
{
    if(lv_obj_check_type(obj,&lv_label_class)&&!strcmp(lv_label_get_text(obj),text))return obj;
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i){
        lv_obj_t *found=find_text(lv_obj_get_child(obj,(int32_t)i),text);
        if(found)return found;
    }
    return NULL;
}

static lv_obj_t *find_image(lv_obj_t *obj,const void *source)
{
    if(lv_obj_check_type(obj,&lv_image_class)&&lv_image_get_src(obj)==source)return obj;
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i){
        lv_obj_t *found=find_image(lv_obj_get_child(obj,(int32_t)i),source);
        if(found)return found;
    }
    return NULL;
}

static unsigned count_text(lv_obj_t *obj,const char *text)
{
    unsigned n=lv_obj_check_type(obj,&lv_label_class)&&!strcmp(lv_label_get_text(obj),text);
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i)n+=count_text(lv_obj_get_child(obj,(int32_t)i),text);
    return n;
}
/* A command button, found by its caption, pressed like a finger would. */
static void click_text(lv_obj_t *root,const char *caption)
{
    lv_obj_t *label=find_text(root,caption);
    assert(label);lv_obj_send_event(lv_obj_get_parent(label),LV_EVENT_CLICKED,NULL);
}
static const char *resting_line(void){run(1500);return lv_label_get_text(s_status_label);}
/* News from the setup worker, as it delivers it: on its task, a copy. */
static const char *news_check="";
static void link_news(pet_link_phase_t phase,bool moving,const char *code,const char *account,const char *error)
{
    pet_link_t link={.phase=phase,.moving=moving};
    strcpy(link.device,"1a2b3c");strcpy(link.code,code);strcpy(link.account,account);strcpy(link.error,error);
    strcpy(link.check,news_check);
    assert(link_view);link_view(&link);
}

/* The link code on the screen: the code to type at
 * aipets.com/link, the question that only a tap answers, the typed-code
 * fallback, and a linked device moving to another account. */
static void link_flows(void)
{
    /* No account: LINK is the deck's first button, and the page waits for
     * the worker's code. */
    assert(!s_enrolled&&s_paired&&online);show(PAGE_HOME);
    unsigned asked=link_requests;
    click_text(s_menu,"LINK");
    assert(s_page==PAGE_LINK&&link_requests==asked+1&&last_link_action==PET_SETUP_LINK_START);
    assert(find_text(s_screen,"____-____")&&find_text(s_screen,"TYPE A CODE")&&!find_text(s_screen,"CANCEL"));
    assert(!strcmp(resting_line(),"GETTING A CODE..."));
    link_news(PET_LINK_SHOWING,false,"K7QX9MPA","","");
    assert(s_page==PAGE_LINK&&find_text(s_screen,"K7QX-9MPA")&&find_text(s_screen,"AIPETS.COM/LINK"));
    assert(find_text(s_screen,"DEVICE 1A2B3C")&&!strcmp(resting_line(),"ENTER THIS CODE AT"));
    assert_page_on_glass();assert_ascii(s_screen);
    /* The typed code stays the fallback; leaving stops the code. */
    click_text(s_screen,"TYPE A CODE");
    assert(s_page==PAGE_CODE&&last_link_action==PET_SETUP_LINK_LEAVE);
    swipe_up();assert(s_page==PAGE_WIFI);
    click_text(s_screen,"NEXT");assert(s_page==PAGE_LINK&&last_link_action==PET_SETUP_LINK_START);
    /* Codes off, or refused: the reason, and the fallback beside it. */
    link_news(PET_LINK_PAUSED,false,"","","LINK_OFFERS_DISABLED");
    assert(!strcmp(resting_line(),"LINK CODES ARE OFF FOR NOW")&&find_text(s_screen,"TYPE A CODE"));
    link_news(PET_LINK_FAILED,false,"","","SETUP_IDENTITY_TAKEN");
    assert(!strcmp(resting_line(),"ID IN USE - TYPE A CODE"));
    link_news(PET_LINK_FAILED,false,"","","INVALID_REQUEST"); /* a firmware bug: the code for support */
    assert(!strcmp(resting_line(),"LINK FAILED: INVALID_REQUEST")&&find_text(s_screen,"TYPE A CODE"));
    /* The code card asks again. */
    asked=link_requests;
    lv_obj_send_event(lv_obj_get_parent(find_text(s_screen,"____-____")),LV_EVENT_CLICKED,NULL);
    assert(link_requests==asked+1&&last_link_action==PET_SETUP_LINK_START&&!strcmp(resting_line(),"GETTING A CODE..."));
    link_news(PET_LINK_OFF,false,"","","LINK_CODE_EXPIRED");
    assert(!strcmp(resting_line(),"CODE EXPIRED - TAP IT"));
    /* Stopped after offers with no code: why, and a tap asks again. */
    link_news(PET_LINK_OFF,false,"","","LINK_NO_ANSWER");
    assert(!strcmp(resting_line(),"NO ANSWER - TAP TO RETRY"));
    link_news(PET_LINK_OFF,false,"","","LINK_OFFERS_DISABLED");
    assert(!strcmp(resting_line(),"LINK CODES ARE OFF FOR NOW")&&find_text(s_screen,"TYPE A CODE"));
    link_news(PET_LINK_SHOWING,false,"K7QX9MPA","","");
    /* Somebody typed it: the question, beside the claim's number, which
     * only a tap answers. The name reads as ASCII, curly apostrophe and all. */
    news_check="472";
    link_news(PET_LINK_CLAIMED,false,"","Alex\xe2\x80\x99s pets","");
    assert(find_text(s_screen,"LINK TO")&&find_text(s_screen,"ALEX'S PETS")&&find_text(s_screen,"NOT ME"));
    assert(find_text(s_screen,"472")&&find_text(s_screen,"NUMBER")&&!strcmp(resting_line(),"SAME NUMBER ON AIPETS.COM?"));
    assert(count_text(s_screen,"LINK")==2&&!find_text(s_screen,"K7QX-9MPA"));
    assert_page_on_glass();assert_ascii(s_screen);
    /* Another claim, another number: the question follows it. */
    news_check="815";
    link_news(PET_LINK_CLAIMED,false,"","Alex\xe2\x80\x99s pets","");
    assert(find_text(s_screen,"815")&&!find_text(s_screen,"472"));
    /* Without a number (an older cloud), the name alone, and no number. */
    news_check="";
    link_news(PET_LINK_CLAIMED,false,"","Alex\xe2\x80\x99s pets","");
    assert(!find_text(s_screen,"NUMBER")&&!strcmp(resting_line(),"IS THIS YOUR ACCOUNT?"));
    /* A name the menu face cannot draw at all reads as no name. */
    news_check="472";
    link_news(PET_LINK_CLAIMED,false,"","\xe7\x8c\xab\xe7\x8c\xab \xe7\x8c\xab","");
    assert(find_text(s_screen,"THIS ACCOUNT")&&!find_text(s_screen,"?? ?")&&find_text(s_screen,"472"));
    news_check="";
    swipe_up();assert(s_page==PAGE_LINK&&!strcmp(lv_label_get_text(s_status_label),"ANSWER ON THIS SCREEN"));
    handle_clicked(NULL);assert(s_page==PAGE_LINK);
    link_no_clicked(NULL);
    assert(last_link_action==PET_SETUP_LINK_NO&&!find_text(s_screen,"NOT ME")&&!strcmp(resting_line(),"GETTING A NEW CODE..."));
    link_news(PET_LINK_SHOWING,false,"W2MN8QRT","","");
    assert(find_text(s_screen,"W2MN-8QRT"));
    /* Found by the worker while the owner is elsewhere in the menu, or with
     * the pet on screen: the question comes to the owner. */
    swipe_up();assert(s_page==PAGE_HOME&&last_link_action==PET_SETUP_LINK_LEAVE);
    link_news(PET_LINK_OFF,false,"","","");
    link_news(PET_LINK_CLAIMED,false,"","","");
    assert(s_page==PAGE_LINK&&find_text(s_screen,"THIS ACCOUNT"));
    lv_screen_load(other_screen);show(PAGE_HOME);
    pet_onboarding_open_from_ui();assert(s_page==PAGE_LINK&&lv_screen_active()==s_display_screen);
    run(400);
    /* A question never goes back to a code, or a stale answer, on its own. */
    link_clicked(NULL);assert(s_page==PAGE_LINK&&find_text(s_screen,"NOT ME"));
    link_yes_clicked(NULL);
    assert(last_link_action==PET_SETUP_LINK_YES&&!find_text(s_screen,"NOT ME")&&!strcmp(resting_line(),"LINKING..."));
    swipe_up();assert(s_page==PAGE_LINK);
    /* Setup completes exactly as after a typed code. */
    setup_status(PET_ENROLL_COMPLETE);
    assert(s_page==PAGE_HOME&&s_enrolled);
    link_news(PET_LINK_LINKED,false,"","","");
    assert(s_page==PAGE_HOME);

    /* A linked device moves only from the deck, only on a tap on MOVE, and
     * control is asked at once to adopt the new account. */
    asked=link_requests;
    link_clicked(NULL);
    assert(s_page==PAGE_LINK&&link_requests==asked+1&&last_link_action==PET_SETUP_LINK_START);
    assert(find_text(s_screen,"CANCEL")&&!find_text(s_screen,"TYPE A CODE"));
    link_news(PET_LINK_SHOWING,true,"W2MN8QRT","","");
    assert(find_text(s_screen,"W2MN-8QRT"));assert_page_on_glass();
    news_check="093";
    link_news(PET_LINK_CLAIMED,true,"","Mia","");
    assert(find_text(s_screen,"MOVE TO")&&find_text(s_screen,"MIA")&&find_text(s_screen,"MOVE")&&find_text(s_screen,"093"));
    assert_page_on_glass();assert_ascii(s_screen);
    link_news(PET_LINK_CLAIMED,true,"","Mia","DEVICE_BUSY");
    assert(!strcmp(resting_line(),"BUSY INSTALLING - TRY AGAIN"));
    link_news(PET_LINK_CLAIMED,true,"","Mia","LINK_UNAVAILABLE");
    assert(!strcmp(resting_line(),"NEW ACCOUNT CAN'T TAKE IT YET")&&find_text(s_screen,"MOVE"));
    /* A move claimed without a number: only NOT ME. */
    news_check="";
    link_news(PET_LINK_CLAIMED,true,"","Mia","");
    assert(find_text(s_screen,"NOT ME")&&!find_text(s_screen,"MOVE")&&!strcmp(resting_line(),"NO NUMBER - CAN'T MOVE"));
    assert_page_on_glass();
    news_check="093";
    link_news(PET_LINK_CLAIMED,true,"","Mia","");
    link_yes_clicked(NULL);assert(last_link_action==PET_SETUP_LINK_YES&&!strcmp(resting_line(),"MOVING..."));
    unsigned synced=retries;
    link_news(PET_LINK_LINKED,true,"","Mia","");
    assert(s_page==PAGE_HOME&&retries==synced+1&&!strcmp(lv_label_get_text(s_status_label),"MOVED - SYNCING"));
    link_news(PET_LINK_LINKED,true,"","Mia","");assert(retries==synced+1); /* once */
    news_check="";
    retries=synced; /* The deck's own SYNC count, checked later, starts where it was. */
    /* Cancel: the code dies, and the deck is back. */
    link_clicked(NULL);link_news(PET_LINK_SHOWING,true,"K7QX9MPA","","");
    click_text(s_screen,"CANCEL");assert(s_page==PAGE_HOME&&last_link_action==PET_SETUP_LINK_LEAVE);
    link_news(PET_LINK_DECLINING,true,"","","");link_news(PET_LINK_OFF,true,"","","");
    assert(s_page==PAGE_HOME);
    link_clicked(NULL);link_news(PET_LINK_FAILED,true,"","","RELINK_UNSUPPORTED");
    assert(!strcmp(resting_line(),"THIS DEVICE CAN'T MOVE"));
    link_news(PET_LINK_PAUSED,true,"","","RELINK_DISABLED");
    assert(!strcmp(resting_line(),"LINK CODES ARE OFF FOR NOW"));
    swipe_up();assert(s_page==PAGE_HOME&&last_link_action==PET_SETUP_LINK_LEAVE);
    link_news(PET_LINK_OFF,true,"","","");
}

/* The deck's pet picker, and the story screens a swipe on the pet opens:
 * each closes with the opposite swipe, or a tap on its chevron, and the pet
 * shows again. */
static void story_screens(pet_onboarding_status_t *status)
{
    status->has_pet=true;status->pet_capacity=3;status->pet_count=3;status->pet_index=1;
    strcpy(status->pack_name,"glowberry-1a2b3c4d");strcpy(status->pet_name,"GLOWBERRY");
    pet_onboarding_update_status(status);
    /* A cell for each installed pet, the one on screen lit. Another cell asks
     * for that pet, the next or the previous one, and the deck closes. */
    lv_screen_load(s_display_screen);show(PAGE_HOME);
    for(unsigned i=0;i<3;++i)assert(!lv_obj_has_flag(s_pet_cell[i],LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_state(s_pet_cell[0],LV_STATE_CHECKED)&&!lv_obj_has_state(s_pet_cell[1],LV_STATE_CHECKED));
    assert(!strcmp(lv_label_get_text(s_pets_value),"GLOWBERRY"));
    assert_ascii(s_screen);
    unsigned back=returned;
    lv_obj_send_event(s_pet_cell[0],LV_EVENT_CLICKED,NULL);assert(!switches&&!s_leaving); /* already on screen */
    lv_obj_send_event(s_pet_cell[2],LV_EVENT_CLICKED,NULL);
    assert(switches==1&&switched==-1&&s_leaving);run(400);assert(returned==back+1);
    lv_screen_load(s_display_screen);show(PAGE_HOME);
    lv_obj_send_event(s_pet_cell[1],LV_EVENT_CLICKED,NULL);assert(switches==2&&switched==1);run(400);
    status->pet_index=3;pet_onboarding_update_status(status);
    lv_screen_load(s_display_screen);show(PAGE_HOME);
    lv_obj_send_event(s_pet_cell[0],LV_EVENT_CLICKED,NULL);assert(switches==3&&switched==1);run(400);
    lv_screen_load(s_display_screen);show(PAGE_HOME);
    /* A full queue says so, and the deck stays. */
    switch_fails=true;lv_obj_send_event(s_pet_cell[1],LV_EVENT_CLICKED,NULL);switch_fails=false;
    assert(switches==3&&!s_leaving&&!strcmp(lv_label_get_text(s_status_label),"BUSY - TRY AGAIN"));
    /* Two pets, two cells; one pet, nothing to choose. */
    status->pet_count=2;status->pet_index=2;pet_onboarding_update_status(status);
    assert(!lv_obj_has_flag(s_pet_cell[1],LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(s_pet_cell[2],LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_state(s_pet_cell[1],LV_STATE_CHECKED)&&!lv_obj_has_state(s_pet_cell[0],LV_STATE_CHECKED));
    status->pet_count=1;status->pet_index=1;pet_onboarding_update_status(status);
    for(unsigned i=0;i<3;++i)assert(lv_obj_has_flag(s_pet_cell[i],LV_OBJ_FLAG_HIDDEN));
    status->pet_count=3;pet_onboarding_update_status(status);

    /* BLESSINGS: a story without blessings shows its empty 3x3 grid and says
     * so; nothing there lights up. A swipe up or right stays; a swipe left goes
     * back to the pet. */
    lv_screen_load(other_screen);
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);run(600);
    assert(s_page==PAGE_BLESSINGS&&lv_screen_active()==s_display_screen);
    assert(!strcmp(lv_label_get_text(s_status_label),"NO BLESSINGS"));
    assert(!find_image(s_screen,&pet_icon_flame)&&!find_image(s_screen,&pet_icon_bolt)&&!find_image(s_screen,&pet_icon_key));
    assert_page_on_glass();assert_ascii(s_screen);
    back=returned;
    drag(180,250,180,90);drag(60,180,300,180);run(400);
    assert(s_page==PAGE_BLESSINGS&&returned==back);
    drag(300,180,60,180);run(400);
    assert(returned==back+1&&lv_screen_active()==other_screen&&s_page==PAGE_HOME&&!find_image(s_screen,&pet_icon_flame));

    /* CHARACTER: the pet's own name, not its face ID, and no stats without a
     * story's hero. A swipe right goes back, and so does its chevron. */
    pet_onboarding_open_screen(PET_ONBOARDING_CHARACTER);run(600);
    assert(s_page==PAGE_CHARACTER&&find_text(s_screen,"GLOWBERRY")&&!find_text(s_screen,"GLOWBERRY-1A2B3C4D"));
    assert(!find_text(s_screen,"HEALTH")&&!find_text(s_screen,"WILLPOWER")&&!find_image(s_screen,&pet_icon_heart));
    assert_page_on_glass();assert_ascii(s_screen);
    drag(300,180,60,180);run(400);assert(s_page==PAGE_CHARACTER&&returned==back+1);
    drag(60,180,300,180);run(400);assert(returned==back+2&&lv_screen_active()==other_screen);
    pet_onboarding_open_screen(PET_ONBOARDING_CHARACTER);run(600);
    handle_clicked(NULL);run(400);assert(returned==back+3);

    /* STORY: with no log yet, how to start one. */
    pet_onboarding_open_screen(PET_ONBOARDING_STORY);run(600);
    assert(s_page==PAGE_STORY&&find_text(s_screen,"NO STORY YET.")&&find_text(s_screen,"LONG PRESS YOUR PET."));
    assert(!strcmp(lv_label_get_text(s_status_label),"NO TURNS YET"));
    assert_page_on_glass();assert_ascii(s_screen);
    /* The story.log arrives while it is open: a line a turn, oldest first, the
     * newest at the bottom and in view. Letters the face cannot draw read as
     * ASCII, and anything else as '?'. */
    static const char *const story[]={"I spotted a purple swirl under Moonmirror Lake.",
        "I sent Grumble to fetch the silver key.","I waded into the moonlit lake.",
        "I asked the dragon about the stolen key.","I followed Grumble\xe2\x80\x99s tracks past the caf\xc3\xa9\xe2\x80\xa6",
        "I jumped from a bridge \xe2\x80\x94 into the hay! \xf0\x9f\x90\x89"};
    pet_onboarding_set_story_log("glowberry-1a2b3c4d",story,6);run(200);
    assert(s_page==PAGE_STORY&&!strcmp(lv_label_get_text(s_status_label),"LAST 6 TURNS"));
    lv_obj_t *oldest=find_text(s_screen,story[0]),*newest=find_text(s_screen,"I jumped from a bridge - into the hay! ?");
    assert(oldest&&newest&&find_text(s_screen,"I followed Grumble's tracks past the cafe...")&&!find_text(s_screen,"NO STORY YET."));
    assert(lv_color_eq(lv_obj_get_style_text_color(newest,LV_PART_MAIN),shade(newest,VALUE)));
    assert(lv_color_eq(lv_obj_get_style_text_color(oldest,LV_PART_MAIN),shade(oldest,LABEL)));
    lv_obj_t *log=lv_obj_get_parent(lv_obj_get_parent(oldest));
    assert(lv_obj_get_scroll_bottom(log)<=0);
    assert_page_on_glass();assert_ascii(s_screen);
    /* Each log replaces the last, an empty one clears it, and another pet's
     * log never shows for this one. */
    pet_onboarding_set_story_log("glowberry-1a2b3c4d",story+4,2);run(200);
    assert(!find_text(s_screen,story[0])&&find_text(s_screen,"I followed Grumble's tracks past the cafe..."));
    assert(!strcmp(lv_label_get_text(s_status_label),"LAST 2 TURNS"));
    pet_onboarding_set_story_log("glowberry-1a2b3c4d",NULL,0);run(200);
    assert(find_text(s_screen,"NO STORY YET.")&&!strcmp(lv_label_get_text(s_status_label),"NO TURNS YET"));
    pet_onboarding_set_story_log("another-pet-00000000",story,6);run(200);
    assert(find_text(s_screen,"NO STORY YET."));
    /* A short log: a swipe down goes back to the pet; up or across stays. */
    pet_onboarding_set_story_log("glowberry-1a2b3c4d",story,1);run(200);
    assert(!strcmp(lv_label_get_text(s_status_label),"LAST TURN"));
    drag(180,250,180,90);drag(60,180,300,180);run(400);assert(s_page==PAGE_STORY&&returned==back+3);
    drag(180,120,180,300);run(400);assert(returned==back+4&&lv_screen_active()==other_screen);
    /* A long one scrolls under that swipe instead. Its chevron, a swipe down
     * from it, or a pull past the first line goes back. */
    const char *many[20];
    for(unsigned i=0;i<20;++i)many[i]=story[i%6];
    pet_onboarding_set_story_log("glowberry-1a2b3c4d",many,20);
    pet_onboarding_open_screen(PET_ONBOARDING_STORY);run(600);
    log=lv_obj_get_parent(lv_obj_get_parent(find_text(s_screen,story[0])));
    int32_t newest_in_view=lv_obj_get_scroll_y(log);
    assert(newest_in_view>0&&lv_obj_get_scroll_bottom(log)<=0);
    drag(180,120,180,280);run(400);
    assert(s_page==PAGE_STORY&&returned==back+4&&lv_obj_get_scroll_y(log)<newest_in_view);
    drag(180,30,180,200);run(400);assert(returned==back+5);
    pet_onboarding_open_screen(PET_ONBOARDING_STORY);run(600);
    handle_clicked(NULL);run(400);assert(returned==back+6);
    pet_onboarding_open_screen(PET_ONBOARDING_STORY);run(600);
    log=lv_obj_get_parent(lv_obj_get_parent(find_text(s_screen,story[0])));
    lv_obj_scroll_to_y(log,0,LV_ANIM_OFF);
    drag(180,100,180,300);run(400);assert(returned==back+7);
    /* Malformed calls keep the log as it was. */
    pet_onboarding_set_story_log(NULL,story,2);pet_onboarding_set_story_log("x",many,21);
    pet_onboarding_set_story_log("x",NULL,2);
    assert(s_story->count==20&&!strcmp(s_story->log_owner,"glowberry-1a2b3c4d"));
    /* Opened from the pet, a story screen goes back to it even before the
     * deck's status knows the pet. */
    status->has_pet=false;pet_onboarding_update_status(status);
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);run(600);
    handle_clicked(NULL);run(400);assert(returned==back+8);
    status->has_pet=true;pet_onboarding_update_status(status);
    lv_screen_load(s_display_screen);
}

/* The STORY screen header's milestone pips, and how many are lit. */
static unsigned pips(lv_obj_t *head,unsigned *lit)
{
    unsigned n=0;
    *lit=0;
    for(uint32_t i=0;i<lv_obj_get_child_count(head);++i){
        lv_obj_t *child=lv_obj_get_child(head,(int32_t)i);
        if(lv_obj_check_type(child,&lv_label_class)||lv_obj_get_style_bg_opa(child,LV_PART_MAIN)!=LV_OPA_COVER)continue;
        ++n;
        *lit+=lv_color_eq(lv_obj_get_style_bg_color(child,LV_PART_MAIN),shade(child,LIT));
    }
    return n;
}

/* Story mode on the menu (story-mode-v1): the deck's STORY card and its
 * STORIES page, which loads another story, or restarts this one after a
 * second tap; the STORY screen's header; the story frame over the pet. */
static void story_mode_screens(void)
{
    const char *pet="glowberry-1a2b3c4d";
    unsigned back=returned;
    /* No STORY card until the gateway lists this pet's stories. */
    lv_screen_load(s_display_screen);show(PAGE_HOME);
    assert(!find_text(s_menu,"STORY"));
    pet_story_list_t list={.count=3};
    static const char *const titles[]={"The Lantern of Thornspire","Moonmirror Lake","The Caf\xc3\xa9 at the End"};
    for(unsigned i=0;i<3;++i){
        snprintf(list.stories[i].id,sizeof(list.stories[i].id),"6a3f0c2e-1d4b-4c8a-9e57-2b9d0f1a7c3%u",i);
        strcpy(list.stories[i].title,titles[i]);
    }
    strcpy(list.current,list.stories[0].id);
    pet_onboarding_set_story_list("another-pet-00000000",&list);show(PAGE_HOME);
    assert(!find_text(s_menu,"STORY"));
    pet_onboarding_set_story_list(pet,&list);show(PAGE_HOME);
    assert(find_text(s_menu,"STORY")&&find_text(s_menu,"THE LANTERN OF THORNSPIRE"));
    assert_ascii(s_screen);
    /* Its tap lists them, newest first, the current one lit. */
    lv_obj_send_event(lv_obj_get_parent(find_text(s_menu,"STORY")),LV_EVENT_CLICKED,NULL);
    assert(s_page==PAGE_STORIES&&find_text(s_screen,"THE CAFE AT THE END"));
    lv_obj_t *current=lv_obj_get_parent(find_text(s_screen,"THE LANTERN OF THORNSPIRE"));
    lv_obj_t *other=lv_obj_get_parent(find_text(s_screen,"MOONMIRROR LAKE"));
    assert(lv_obj_has_state(current,LV_STATE_CHECKED)&&!lv_obj_has_state(other,LV_STATE_CHECKED));
    assert_page_on_glass();assert_ascii(s_screen);
    /* The loaded one stays. Another loads, and the pet shows; a session that
     * cannot take it says so, and the page stays. */
    lv_obj_send_event(current,LV_EVENT_CLICKED,NULL);
    assert(!story_requests&&!strcmp(lv_label_get_text(s_status_label),"ALREADY LOADED"));
    story_result=ESP_ERR_INVALID_STATE;lv_obj_send_event(other,LV_EVENT_CLICKED,NULL);story_result=ESP_OK;
    assert(story_requests==1&&s_page==PAGE_STORIES&&!s_leaving);
    assert(!strcmp(lv_label_get_text(s_status_label),"OFFLINE - TRY AGAIN"));
    lv_obj_send_event(other,LV_EVENT_CLICKED,NULL);
    assert(story_requests==2&&!strcmp(story_type,"story.load")&&!strcmp(story_id,list.stories[1].id)&&s_leaving);
    run(400);assert(returned==back+1);
    /* RESTART wipes the story's memory, so it asks first; a second tap
     * restarts it, and the pet shows. */
    lv_screen_load(s_display_screen);show(PAGE_HOME);stories_clicked(NULL);
    click_text(s_screen,"RESTART");
    assert(story_requests==2&&find_text(s_screen,"CONFIRM")&&!find_text(s_screen,"RESTART"));
    assert(!strcmp(lv_label_get_text(s_status_label),"WIPES THE STORY'S MEMORY"));
    assert_page_on_glass();
    click_text(s_screen,"CONFIRM");
    assert(story_requests==3&&!strcmp(story_type,"story.restart")&&!story_id[0]&&s_leaving);
    run(400);assert(returned==back+2);
    /* Unconfirmed, it lapses after five seconds, and leaving forgets it. */
    lv_screen_load(s_display_screen);show(PAGE_HOME);stories_clicked(NULL);
    click_text(s_screen,"RESTART");run(RESTART_CONFIRM_MS+200);
    assert(find_text(s_screen,"RESTART")&&!s_restart_armed);
    click_text(s_screen,"RESTART");swipe_up();assert(s_page==PAGE_HOME&&!s_restart_armed);
    stories_clicked(NULL);click_text(s_screen,"RESTART");assert(story_requests==3&&s_restart_armed);
    /* A new list shows at once on the open page; with no story loaded there
     * is nothing to restart. */
    list.count=1;list.current[0]=0;pet_onboarding_set_story_list(pet,&list);run(200);
    assert(s_page==PAGE_STORIES&&!find_text(s_screen,"MOONMIRROR LAKE")&&!find_text(s_screen,"RESTART"));
    assert(!lv_obj_has_state(lv_obj_get_parent(find_text(s_screen,"THE LANTERN OF THORNSPIRE")),LV_STATE_CHECKED));
    list.count=0;pet_onboarding_set_story_list(pet,&list);run(200);
    assert(find_text(s_screen,"NO STORIES YET"));
    show(PAGE_HOME);assert(find_text(s_menu,"STORY")&&find_text(s_menu,"NONE"));

    /* The STORY screen's header: the title, NEXT and the goal, and a pip for
     * each milestone, lit once reached; the log below it. */
    pet_story_state_t state={.story=true,.reached=2,.of=5};
    strcpy(state.title,"The Lantern of Thornspire");strcpy(state.goal,"the middle of Moonmirror Lake");
    pet_onboarding_set_story_state(pet,&state);
    pet_onboarding_open_screen(PET_ONBOARDING_STORY);run(600);
    lv_obj_t *next=find_text(s_screen,"NEXT"),*head=lv_obj_get_parent(next);
    /* A line too long for the card ends in dots. */
    lv_obj_t *goal=lv_obj_get_child(head,lv_obj_get_index(next)+1);
    assert(!strncmp(lv_label_get_text(lv_obj_get_child(head,lv_obj_get_index(next)-1)),"THE LANTERN OF",14));
    assert(!strncmp(lv_label_get_text(goal),"THE MIDDLE OF",13));
    unsigned lit=0;
    assert(pips(head,&lit)==5&&lit==2);
    lv_obj_t *line=find_text(s_screen,"I jumped from a bridge - into the hay! ?");
    assert(line&&lv_obj_get_y(lv_obj_get_parent(lv_obj_get_parent(line)))>=lv_obj_get_y(head)+lv_obj_get_height(head));
    assert_page_on_glass();assert_ascii(s_screen);
    /* Each state replaces the last, at once; the story's end has no goal. */
    state.reached=5;state.goal[0]=0;pet_onboarding_set_story_state(pet,&state);run(200);
    head=lv_obj_get_parent(find_text(s_screen,"NEXT"));
    assert(find_text(s_screen,"THE END")&&pips(head,&lit)==5&&lit==5);
    /* Another pet's story, or none, leaves the screen as before. */
    pet_onboarding_set_story_state("another-pet-00000000",&state);run(200);
    assert(!find_text(s_screen,"NEXT"));
    state.title[0]=0;pet_onboarding_set_story_state(pet,&state);run(200);
    assert(!find_text(s_screen,"NEXT"));
    handle_clicked(NULL);run(400);assert(returned==back+3);

    /* The story frame over the pet's screen: hidden until shown, a STORY tab
     * inside the glass, nothing a finger can press, and drawn without fault. */
    lv_obj_t *frame=pet_onboarding_story_frame(other_screen);
    assert(lv_obj_has_flag(frame,LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(frame,LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *tab=lv_obj_get_parent(find_text(frame,"STORY"));
    assert(!lv_obj_has_flag(tab,LV_OBJ_FLAG_CLICKABLE));
    lv_obj_remove_flag(frame,LV_OBJ_FLAG_HIDDEN);lv_obj_update_layout(frame);
    assert_glass_bounds(tab);
    lv_obj_invalidate(other_screen);lv_refr_now(NULL);
    lv_obj_delete(frame);
    lv_screen_load(s_display_screen);
}

/* The deck has no story sliders: no STORY PULL or WAY BACK card under STORY. */
static void no_story_sliders(void)
{
    lv_screen_load(s_display_screen);
    show(PAGE_HOME);
    lv_obj_update_layout(s_screen);
    assert(find_text(s_menu, "STORY") && !find_text(s_menu, "STORY PULL") && !find_text(s_menu, "WAY BACK"));
}

/* How many pixels on the glass are `color`. */
static unsigned shown_in(lv_color_t color)
{
    const uint16_t wanted = lv_color_to_u16(color);
    const size_t pixels = (size_t)pet_board_current()->width * pet_board_current()->height;
    unsigned n = 0;
    for (size_t i = 0; i < pixels; ++i) n += shown[i] == wanted;
    return n;
}

/* The story frame over the pet, in each style: its rim green on the story's
 * path, yellow on a detour and red hurrying back, following each
 * story.state. */
static void frame_follows_path(void)
{
    for (uint8_t style = 0; style < PET_MENU_STYLES; ++style) {
        use_theme(style);
        lv_screen_load(other_screen);
        lv_obj_t *frame = pet_onboarding_story_frame(other_screen);
        lv_obj_remove_flag(frame, LV_OBJ_FLAG_HIDDEN);
        const lv_color_t colors[] = {shade(frame, GOOD), lv_color_hex(DETOUR_YELLOW), lv_color_hex(HURRY_RED)};
        pet_story_state_t state = {.story = true, .reached = 1, .of = 5};
        strcpy(state.title, "The Lantern of Thornspire");
        lv_refr_now(NULL);
        assert(shown_in(colors[0]) && !shown_in(colors[1]) && !shown_in(colors[2]));
        /* The path of each state: the story's (0), a detour (1), hurrying back (2). */
        static const unsigned path[] = {1, 0, 2, 1, 2, 2, 0};
        for (unsigned i = 0; i < sizeof(path) / sizeof(path[0]); ++i) {
            state.detour = path[i] == 1;
            state.hurry = path[i] == 2;
            pet_onboarding_set_story_state("glowberry-1a2b3c4d", &state);
            run(200);
            lv_refr_now(NULL);
            for (unsigned c = 0; c < 3; ++c)
                assert(c == path[i] ? shown_in(colors[c]) > 0 : !shown_in(colors[c]));
        }
        lv_obj_delete(frame);
    }
    use_theme(PET_MENU_STYLE_TERMINAL);
    lv_screen_load(s_display_screen);
}

/* A story played as a game (story-game-v1). BLESSINGS shows the game's own,
 * spent ones dimmed and empty cells dark; a tap chooses one, with its name
 * and USE, and USE uses it as the pet shows. CHARACTER shows the hero's
 * stats out of their maximum, and its conditions. Without them, placeholders. */
static void story_game_screens(void)
{
    const char *pet="glowberry-1a2b3c4d";
    unsigned back=returned;
    pet_story_state_t state={.story=true,.reached=3,.of=5,.has_hero=true,.hero={10,20,6,2,10},.condition_count=4,
        .has_blessings=true,.blessing_count=4};
    strcpy(state.title,"The Lantern of Thornspire");
    strcpy(state.conditions[0],"Slowed");strcpy(state.conditions[1],"Blessed by the moon");
    strcpy(state.conditions[2],"Poisoned");strcpy(state.conditions[3],"Brave");
    const pet_story_blessing_t blessings[]={{"healing-light","Healing Light",PET_BLESSING_HEART,1,false},
        {"thunderstorm","Thunderstorm",PET_BLESSING_STORM,0,false},{"quick-feather","Quick Feather",PET_BLESSING_FEATHER,3,false},
        {"attitude","Attitude",PET_BLESSING_MASK,5,true}};
    memcpy(state.blessings,blessings,sizeof(blessings));
    pet_onboarding_set_story_state(pet,&state);
    lv_screen_load(other_screen);
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);run(600);
    lv_obj_t *heart=lv_obj_get_parent(find_image(s_screen,&pet_icon_heart)),*grid=lv_obj_get_parent(heart);
    assert(lv_obj_get_child_count(grid)==9&&find_image(grid,&pet_icon_bolt)&&find_image(grid,&pet_icon_feather));
    assert(!find_image(grid,&pet_icon_flame)&&!strcmp(lv_label_get_text(s_status_label),"TAP A BLESSING"));
    /* The spent one is dimmed, and the six cells left are dark and do nothing. */
    lv_obj_t *spent_icon=find_image(grid,&pet_icon_bolt);
    assert(lv_color_eq(lv_obj_get_style_image_recolor(spent_icon,LV_PART_MAIN),shade(spent_icon,UNLIT)));
    unsigned empty=0;
    for(uint32_t i=0;i<9;++i)empty+=!lv_obj_has_flag(lv_obj_get_child(grid,(int32_t)i),LV_OBJ_FLAG_CLICKABLE);
    assert(empty==5&&!find_text(s_screen,"USE - 1 LEFT")&&find_image(grid,&pet_icon_blessing_mask));
    assert_page_on_glass();assert_ascii(s_screen);
    /* A tap chooses one: lit, its name, and USE with its uses left. */
    lv_obj_send_event(heart,LV_EVENT_CLICKED,NULL);
    heart=lv_obj_get_parent(find_image(s_screen,&pet_icon_heart));
    assert(lv_obj_has_state(heart,LV_STATE_CHECKED)&&find_text(s_screen,"USE - 1 LEFT"));
    assert(!strcmp(lv_label_get_text(s_status_label),"HEALING LIGHT"));
    assert_page_on_glass();assert_ascii(s_screen);
    /* USE asks for that blessing, and the pet shows to react. */
    click_text(s_screen,"USE - 1 LEFT");
    assert(!strcmp(blessing_used,"healing-light")&&!blessing_spoken&&s_leaving);
    run(400);assert(returned==back+1&&lv_screen_active()==other_screen);
    /* It stays chosen; a request that cannot be taken says so, and stays. */
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);run(600);
    blessing_fails=true;blessing_used[0]=0;click_text(s_screen,"USE - 1 LEFT");blessing_fails=false;
    assert(!blessing_used[0]&&!s_leaving&&!strcmp(lv_label_get_text(s_status_label),"BUSY - TRY AGAIN"));
    /* A spent blessing has nothing to use. */
    lv_obj_send_event(lv_obj_get_parent(find_image(s_screen,&pet_icon_bolt)),LV_EVENT_CLICKED,NULL);
    assert(find_text(s_screen,"SPENT")&&!find_text(s_screen,"USE - 1 LEFT"));
    assert(!strcmp(lv_label_get_text(s_status_label),"THUNDERSTORM"));
    /* A blessing that takes the owner's words asks them to speak, and says so. */
    lv_obj_send_event(lv_obj_get_parent(find_image(s_screen,&pet_icon_blessing_mask)),LV_EVENT_CLICKED,NULL);
    assert(find_text(s_screen,"SPEAK - 5 LEFT")&&!strcmp(lv_label_get_text(s_status_label),"ATTITUDE"));
    assert_page_on_glass();assert_ascii(s_screen);
    click_text(s_screen,"SPEAK - 5 LEFT");
    assert(!strcmp(blessing_used,"attitude")&&blessing_spoken&&s_leaving);
    run(400);assert(returned==back+2&&lv_screen_active()==other_screen);
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);run(600);
    /* Without the game's blessings, the empty grid, at once. */
    state.has_blessings=false;pet_onboarding_set_story_state(pet,&state);run(200);
    assert(s_page==PAGE_BLESSINGS&&!find_image(s_screen,&pet_icon_bolt)&&!find_text(s_screen,"SPENT"));
    assert(!strcmp(lv_label_get_text(s_status_label),"NO BLESSINGS"));
    handle_clicked(NULL);run(400);assert(returned==back+3);

    /* CHARACTER: the hero's stats, health out of its maximum and the others
     * out of 20, under its four conditions: one row of tags, as many as fit
     * and "+N" for the rest, so every bar keeps its height on the 240 px
     * glass too. */
    pet_onboarding_open_screen(PET_ONBOARDING_CHARACTER);run(600);
    assert(find_text(s_screen,"10/20")&&find_text(s_screen,"6/20")&&find_text(s_screen,"2/20"));
    lv_obj_t *slowed=find_text(s_screen,"SLOWED"),*tags=lv_obj_get_parent(slowed);
    assert(lv_obj_get_style_border_width(slowed,LV_PART_MAIN)==1);
    unsigned named=0;
    char rest[4]="",expected[4]="";
    for(uint32_t i=0;i<lv_obj_get_child_count(tags);++i){
        lv_obj_t *tag=lv_obj_get_child(tags,(int32_t)i);
        assert(lv_obj_get_y(tag)==lv_obj_get_y(slowed)&&lv_obj_get_x(tag)>=0&&lv_obj_get_x2(tag)<=lv_obj_get_width(tags));
        if(lv_label_get_text(tag)[0]=='+')strcpy(rest,lv_label_get_text(tag));
        else ++named;
    }
    if(named<4)snprintf(expected,sizeof(expected),"+%u",4-named);
    assert(named&&!strcmp(rest,expected));
    lv_obj_t *sheet=lv_obj_get_parent(find_text(s_screen,"HEALTH"));
    unsigned bars=0;
    for(uint32_t i=0;i<lv_obj_get_child_count(sheet);++i){
        lv_obj_t *child=lv_obj_get_child(sheet,(int32_t)i);
        if(!lv_obj_check_type(child,&lv_bar_class))continue;
        assert(lv_obj_get_height(child)>=4&&lv_bar_get_max_value(child)==20);
        assert(bars||lv_bar_get_value(child)==10);
        ++bars;
    }
    assert(bars==4);
    assert_page_on_glass();assert_ascii(s_screen);
    /* Without a hero, no stats. */
    state.has_hero=false;state.condition_count=0;pet_onboarding_set_story_state(pet,&state);run(200);
    assert(s_page==PAGE_CHARACTER&&!find_text(s_screen,"HEALTH")&&!find_text(s_screen,"10/20")&&!find_text(s_screen,"SLOWED"));
    handle_clicked(NULL);run(400);assert(returned==back+4);
    lv_screen_load(s_display_screen);
}

/* What a finger can press on the page, in tree order: each one's size, and
 * whether it is paper, or on paper, whose sheet a theme may draw inside the
 * box the page gave it. */
typedef struct {
    int32_t width, height;
    bool paper, on_paper;
} target_t;

static unsigned targets(lv_obj_t *obj, target_t *out, unsigned count, unsigned capacity)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && obj != s_screen && count < capacity) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        out[count].width = lv_obj_get_width(obj);
        out[count].height = lv_obj_get_height(obj);
        out[count].paper = lv_obj_has_flag(obj, PAPER);
        out[count].on_paper = parent && lv_obj_has_flag(parent, PAPER);
        ++count;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        count = targets(lv_obj_get_child(obj, (int32_t)i), out, count, capacity);
    return count;
}

/* Every page, built and drawn in the current style: inside the glass, every
 * label drawable, in the style's own face. Each page's touch targets match
 * those the other style built, in `seen`: the same size, but a sheet of paper
 * may take its margin, and its rolls, inside the box, and a row on paper is
 * as much narrower. */
static const page_t every_page[] = {
    PAGE_BOOT, PAGE_WIFI, PAGE_PASSWORD, PAGE_CODE, PAGE_HOME, PAGE_LIBRARY, PAGE_CONFIRM, PAGE_RESTART_CONFIRM,
    PAGE_ABOUT, PAGE_LINK, PAGE_BLESSINGS, PAGE_CHARACTER, PAGE_STORY, PAGE_STORIES,
};
#define PAGES (sizeof(every_page) / sizeof(every_page[0]))
#define TARGETS 48
static target_t seen[PAGES][TARGETS];
static unsigned seen_count[PAGES];

static void every_page_in_style(void)
{
    const int32_t sheet = 2 * (s_woodland.paper_margin + ROLL);
    for (unsigned i = 0; i < PAGES; ++i) {
        show_page(every_page[i], false);
        lv_obj_update_layout(s_screen);
        lv_obj_invalidate(s_screen);
        lv_refr_now(NULL);
        if (every_page[i] != PAGE_BOOT) {
            const lv_font_t *face = lv_obj_get_style_text_font(s_status_label, LV_PART_MAIN);
            assert(face == s_theme->font[TYPE_SMALL][SMALL_SCREEN]);
        }
        assert_page_on_glass();
        assert_ascii(s_screen);
        target_t found[TARGETS];
        const unsigned count = targets(s_screen, found, 0, TARGETS);
        assert(count < TARGETS);
        if (!seen_count[i]) {
            memcpy(seen[i], found, sizeof(found));
            seen_count[i] = count;
            continue;
        }
        assert(count == seen_count[i]);
        for (unsigned t = 0; t < count; ++t) {
            const int32_t width = LV_ABS(found[t].width - seen[i][t].width);
            const int32_t height = LV_ABS(found[t].height - seen[i][t].height);
            if (found[t].paper) assert(width <= sheet && height <= sheet);
            else if (found[t].on_paper) assert(width <= sheet && !height);
            else assert(!width && !height);
        }
    }
}

static lv_obj_t *style_card_shown(void)
{
    lv_obj_t *label = find_text(s_menu, "STYLE");
    assert(label);
    return lv_obj_get_parent(label);
}

/* STYLE switches the menu's look, and the device keeps it: the deck is built
 * again in the new look, scrolled as it was, STYLE still under the finger.
 * Every page builds in both looks; gestures still close a page; a look the
 * device cannot keep stays as it was; and the story frame over the pet
 * follows the look. */
static void themes(void)
{
    const unsigned back = returned;
    lv_screen_load(s_display_screen);
    show(PAGE_HOME);
    lv_obj_t *style = style_card_shown();
    assert(s_theme == &s_terminal && find_text(style, "TERMINAL") && lv_obj_get_height(style) >= 44);
    lv_obj_update_layout(s_screen);
    lv_obj_scroll_to_y(s_menu, lv_obj_get_y(style) - 100, LV_ANIM_OFF);
    lv_obj_update_layout(s_screen);
    const int32_t scrolled = lv_obj_get_scroll_y(s_menu), under = lv_obj_get_y(style) - scrolled;
    assert(scrolled > 0);
    lv_obj_send_event(style, LV_EVENT_CLICKED, NULL);
    assert(kept_style == PET_MENU_STYLE_WOODLAND && s_theme == &s_woodland);
    assert(s_page == PAGE_HOME && !s_leaving);
    lv_obj_update_layout(s_screen);
    style = style_card_shown();
    assert(find_text(style, "WOODLAND") && lv_obj_get_scroll_y(s_menu) == scrolled);
    assert(lv_obj_get_y(style) - scrolled == under);
    assert(lv_obj_get_style_text_font(s_status_label, LV_PART_MAIN) == &pet_woodland_font_12);
    /* Its log credits WOODLAND's faces. */
    assert(!strncmp(log_text(18), "fonts derived", 13) && !strncmp(log_text(19), "(c) 2023", 8));
    /* A look the device cannot keep stays as it was, and says so. */
    style_fails = true;
    lv_obj_send_event(style, LV_EVENT_CLICKED, NULL);
    style_fails = false;
    assert(s_theme == &s_woodland && kept_style == PET_MENU_STYLE_WOODLAND);
    assert(!strcmp(lv_label_get_text(s_status_label), "BUSY - TRY AGAIN"));
    /* Gestures still close the deck and a story screen. */
    drag(180, 344, 180, 160);
    run(400);
    assert(returned == back + 1);
    pet_onboarding_open_screen(PET_ONBOARDING_BLESSINGS);
    run(600);
    drag(300, 180, 60, 180);
    run(400);
    assert(returned == back + 2 && lv_screen_active() == other_screen);
    /* The story frame, drawn in WOODLAND, follows the look to TERMINAL. */
    lv_obj_t *frame = pet_onboarding_story_frame(other_screen);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(other_screen);
    lv_refr_now(NULL);
    assert(lv_obj_get_style_text_font(find_text(frame, "STORY"), LV_PART_MAIN) == &pet_woodland_font_12);
    lv_screen_load(s_display_screen);
    /* Every page, in each look, the second against the first. */
    every_page_in_style();
    use_theme(PET_MENU_STYLE_TERMINAL);
    every_page_in_style();
    assert(lv_obj_get_style_text_font(find_text(frame, "STORY"), LV_PART_MAIN) == &pet_menu_font_12);
    lv_obj_delete(frame);
    /* And back, from the deck. */
    use_theme(PET_MENU_STYLE_WOODLAND);
    show(PAGE_HOME);
    lv_obj_send_event(style_card_shown(), LV_EVENT_CLICKED, NULL);
    assert(kept_style == PET_MENU_STYLE_TERMINAL && s_theme == &s_terminal);
    assert(find_text(style_card_shown(), "TERMINAL"));
}

int main(void)
{
    lv_init();
    const pet_onboarding_control_t controls={
        .library=library_request,.select=select_request,
        .return_to_pet=return_request,.retry_sync=retry_request,
        .restart=restart_request,.join_wifi=join_wifi_request,.volume=volume_request,.brightness=brightness_request,
        .switch_pet=switch_request,.story=story_request,.use_blessing=blessing_request,.style=style_request};
    pet_onboarding_bind_control(&controls);
    /* The device kept WOODLAND: the menu starts in it. */
    pet_config_t config = {.brightness=70,.volume=55,.menu_style=PET_MENU_STYLE_WOODLAND};
    assert(pet_onboarding_start(&config, "https://fixture.invalid") == ESP_OK);
    lv_obj_update_layout(s_screen);
    const pet_board_profile_t *board = pet_board_current();
    /* Regression: rectangular hardware must not host a centered 360px circle. */
    assert(lv_obj_get_width(s_screen) == board->width && lv_obj_get_height(s_screen) == board->height);
    assert(lv_obj_get_x(s_screen) == 0 && lv_obj_get_y(s_screen) == 0);
    assert(lv_obj_get_width(s_bezel) == board->width - 6);
    assert(lv_obj_get_height(s_bezel) == board->height - 6);
    assert((lv_obj_get_style_radius(s_bezel, 0) == LV_RADIUS_CIRCLE) == board->round);
    assert(lv_indev_get_read_timer(touch_input)->period==8);
    lv_indev_data_t touch_data={0};
    resilient_touch_read(touch_input,&touch_data);
    assert(touch_data.state==LV_INDEV_STATE_RELEASED&&touch_reads==0);
    touch_gpio_level=0;touch_point_count=1;
    resilient_touch_read(touch_input,&touch_data);
    assert(touch_data.state==LV_INDEV_STATE_PRESSED&&touch_reads==1);
    assert(touch_data.point.x==40&&touch_data.point.y==90);
    /* One read without contact is noise and holds the press where it was;
     * PET_TOUCH_RELEASE_READS in a row end it. */
    touch_gpio_level=1;touch_read_result=ESP_FAIL;
    resilient_touch_read(touch_input,&touch_data);
    assert(touch_data.state==LV_INDEV_STATE_PRESSED&&touch_reads==2);
    assert(touch_data.point.x==40&&touch_data.point.y==90);
    resilient_touch_read(touch_input,&touch_data);
    assert(touch_data.state==LV_INDEV_STATE_RELEASED&&touch_reads==3);
    touch_read_result=ESP_OK;touch_semaphore_count=1;
    resilient_touch_read(touch_input,&touch_data);
    assert(touch_data.state==LV_INDEV_STATE_PRESSED&&touch_reads==4);
    touch_point_count=0;resilient_touch_read(touch_input,&touch_data);resilient_touch_read(touch_input,&touch_data);
    assert(touch_data.state==LV_INDEV_STATE_RELEASED);
    assert(s_page == PAGE_BOOT);
    assert_ascii(s_screen);
    /* The boot page: the boot mark lit in lime, centred and the
     * first part to fade in; the cursor after the log's last line; everything
     * inside the glass. */
    lv_obj_t *logo=NULL,*last_line=NULL;
    for(uint32_t i=0;i<lv_obj_get_child_count(s_screen);++i) {
        lv_obj_t *child=lv_obj_get_child(s_screen,(int32_t)i);
        if(lv_obj_check_type(child,&lv_image_class)){assert(!logo);logo=child;}
        if(lv_obj_check_type(child,&lv_label_class)&&!strncmp(lv_label_get_text(child),"> PETS ",7))last_line=child;
    }
    assert(logo&&lv_image_get_src(logo)==&pet_boot_mark&&!find_text(s_screen,"AIPETS"));
    assert(pet_boot_mark.header.cf==LV_COLOR_FORMAT_A8&&
           pet_boot_mark.data_size==(uint32_t)pet_boot_mark.header.w*pet_boot_mark.header.h);
    assert(lv_color_eq(lv_obj_get_style_image_recolor(logo,LV_PART_MAIN),shade(logo,LIT))&&
           lv_obj_get_style_image_recolor_opa(logo,LV_PART_MAIN)==LV_OPA_COVER);
    lv_anim_t *logo_reveal=lv_anim_get(logo,fade);
    assert(lv_obj_get_style_opa(logo,LV_PART_MAIN)==LV_OPA_TRANSP&&logo_reveal&&
           lv_anim_get_delay(logo_reveal)==120&&lv_anim_get_time(logo_reveal)==110);
    assert_page_on_glass();
    assert(lv_obj_get_x(logo)*2+(int32_t)pet_boot_mark.header.w==board->width);
    assert(last_line&&lv_obj_get_x(s_cursor)==lv_obj_get_x2(last_line)+4&&lv_obj_get_y(s_cursor)==lv_obj_get_y(last_line)+1);
    assert(s_theme==&s_woodland&&lv_obj_get_style_text_font(last_line,LV_PART_MAIN)==&pet_woodland_font_12);
    use_theme(PET_MENU_STYLE_TERMINAL); /* the flows below, in TERMINAL */
    assert(!pet_onboarding_ui_ready());
    poll_connection(NULL);
    assert(pet_onboarding_ui_ready());
    assert(native_heartbeats == 1); /* Native splash needs no initialized pet. */
    other_screen = lv_obj_create(NULL);
    lv_screen_load(other_screen);
    poll_connection(NULL);
    assert(native_heartbeats == 1); /* Hidden wizard cannot mask a stalled pet. */
    assert(!pet_onboarding_ui_ready());
    lv_screen_load(s_display_screen);
    poll_connection(NULL);
    assert(native_heartbeats == 2);
    assert(pet_onboarding_ui_ready());lv_tick_inc(2001);assert(!pet_onboarding_ui_ready());
    poll_connection(NULL);assert(pet_onboarding_ui_ready());
    /* The boot log waits for a pet; with none coming it moves on to setup. */
    assert(s_splash_timer && s_page == PAGE_BOOT);
    pet_onboarding_status_t nothing={0};
    pet_onboarding_update_status(&nothing);
    run(300);
    assert(s_page == PAGE_WIFI && last_command.scan && !s_splash_timer);
    finish_splash(NULL);
    assert(s_page == PAGE_WIFI && last_command.scan);
    pet_wifi_network_t networks[3] = {
        {.ssid="Saved home", .secured=true, .saved=true, .rssi=-50},
        {.ssid="Guest\nforged", .secured=false, .rssi=-70},
        {.ssid="Secure lab \xc3\xa9t\xc3\xa9", .secured=true, .rssi=-90}};
    scan_finished(networks, 3, ESP_OK);
    assert(lv_obj_get_child_count(s_network_list) == 3);
    assert_glass_bounds(s_network_list);
    assert_page_on_glass();
    assert_ascii(s_screen); /* Accents fold away and other UTF-8 shows as '?', never missing glyphs. */
    swipe_up();
    assert(s_page == PAGE_WIFI); /* Unpaired: the setup has nowhere to go back to. */
    unsigned before = enqueued;
    lv_obj_send_event(lv_obj_get_child(s_network_list,0),LV_EVENT_CLICKED,NULL);
    assert(enqueued==before+1&&!strcmp(last_command.ssid,"Saved home")&&!last_command.password[0]);
    before=enqueued;
    lv_obj_send_event(lv_obj_get_child(s_network_list,1),LV_EVENT_CLICKED,NULL);
    assert(enqueued==before+1&&!strcmp(last_command.ssid,"Guest\nforged")&&!last_command.password[0]);
    lv_obj_send_event(lv_obj_get_child(s_network_list,2),LV_EVENT_CLICKED,NULL);
    assert(s_page == PAGE_PASSWORD && !s_ssid && !strcmp(s_selected_ssid,"Secure lab \xc3\xa9t\xc3\xa9"));
    assert(!strcmp(s_typing,"KEY FOR Secure lab ete"));
    assert_glass_bounds(s_keyboard);
    assert_glass_bounds(s_password);
    assert_page_on_glass();
    assert_ascii(s_screen);
    assert(lv_obj_get_width(s_keyboard) == (board->round ? TEXT_KEYBOARD_W : board->width - 32));
    assert(lv_obj_get_height(s_keyboard) == TEXT_KEYBOARD_H * board->height / 360);
    assert(keyboard_max_columns(s_keyboard) == 8);
    assert(lv_textarea_get_password_mode(s_password));
    lv_obj_send_event(s_password_reveal,LV_EVENT_CLICKED,NULL);
    assert(!lv_textarea_get_password_mode(s_password));
    press_keyboard_key(s_keyboard, keyboard_mode_special);
    assert(lv_keyboard_get_mode(s_keyboard) == LV_KEYBOARD_MODE_SPECIAL);
    assert(keyboard_max_columns(s_keyboard) == 8);
    press_keyboard_key(s_keyboard, keyboard_mode_upper);
    assert(lv_keyboard_get_mode(s_keyboard) == LV_KEYBOARD_MODE_TEXT_UPPER);
    press_keyboard_key(s_keyboard, keyboard_mode_lower);
    assert(lv_keyboard_get_mode(s_keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);
    /* The word caps do what the symbols did. */
    lv_textarea_set_text(s_password, "ab");
    press_keyboard_key(s_keyboard, "c");
    press_keyboard_key(s_keyboard, key_space);
    press_keyboard_key(s_keyboard, key_delete);
    assert(!strcmp(lv_textarea_get_text(s_password), "abc"));
    before = enqueued;
    lv_textarea_set_text(s_password, "short");
    join_clicked(NULL);
    assert(enqueued == before);
    lv_textarea_set_text(s_password, "correct-password");
    press_keyboard_key(s_keyboard, key_enter);
    assert(enqueued == before + 1 && !strcmp(last_command.password, "correct-password"));
    assert(!lv_textarea_get_text(s_password)[0]);
    back_clicked(NULL);
    hidden_clicked(NULL);
    assert(s_hidden && s_page == PAGE_PASSWORD);
    assert(lv_textarea_get_password_mode(s_password)); /* Reveal resets on leave. */
    assert_page_on_glass();
    assert(lv_keyboard_get_textarea(s_keyboard) == s_ssid);
    press_keyboard_key(s_keyboard, key_enter); /* ok on the name moves to the key */
    assert(lv_keyboard_get_textarea(s_keyboard) == s_password);
    lv_textarea_set_text(s_ssid, "12345678901234567890123456789012");
    lv_textarea_set_text(s_password, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    join_clicked(NULL);
    assert(strlen(last_command.ssid) == 32 && strlen(last_command.password) == 64);
    before = enqueued;
    lv_textarea_set_text(s_password, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    join_clicked(NULL);
    assert(enqueued == before);
    /* Swiping up from the keyboard leaves the page. */
    lv_textarea_set_text(s_password, "");
    drag(180,250,180,130);
    assert(s_page == PAGE_WIFI);
    code_clicked(NULL);
    assert(s_page == PAGE_WIFI); /* no Wi-Fi: no code screen */
    online = true;
    code_clicked(NULL);
    assert(s_page == PAGE_CODE);
    assert_glass_bounds(s_keyboard);
    assert_page_on_glass();
    assert(lv_obj_get_width(s_keyboard) == (board->round ? CODE_KEYBOARD_W : board->width - 32));
    assert(lv_obj_get_height(s_keyboard) == CODE_KEYBOARD_H * board->height / 360);
    assert(lv_keyboard_get_mode(s_keyboard) == LV_KEYBOARD_MODE_USER_1);
    assert(keyboard_max_columns(s_keyboard) == 7);
    assert(lv_textarea_get_max_length(s_code) == PET_ENROLLMENT_CODE_LENGTH);
    press_keyboard_key(s_keyboard, "A");
    press_keyboard_key(s_keyboard, "2");
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(s_code_cell[1],0)),"2"));
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(s_code_cell[2],0)),"_"));
    lv_textarea_set_text(s_code, "A2B");
    lv_obj_send_event(s_code, LV_EVENT_READY, NULL);
    assert(!last_code[0]); /* incomplete input never submits */
    press_keyboard_key(s_keyboard, key_escape);
    assert(s_page == PAGE_WIFI); /* The escape cap replaces the duplicate button. */
    code_clicked(NULL);
    assert(s_page == PAGE_CODE);
    lv_textarea_set_text(s_code, "A2B3");
    press_keyboard_key(s_keyboard, key_enter);
    assert(!strcmp(last_code, "A2B3") && !lv_textarea_get_text(s_code)[0]);
    /* The cloud asked to wait: the code goes when the wait ends, and the
     * code page says so. */
    setup_status(PET_ENROLL_WAIT_CLOUD);
    assert(s_page == PAGE_CODE && !strcmp(resting_line(), "WAITING..."));
    setup_status(PET_ENROLL_NEEDS_CODE);
    assert(!strcmp(resting_line(), "READY FOR YOUR CODE"));
    swipe_up();
    assert(s_page == PAGE_WIFI);
    code_clicked(NULL);
    setup_status(PET_ENROLL_COMPLETE);
    assert(s_page == PAGE_HOME);
    back_clicked(NULL);
    code_clicked(NULL);
    assert(s_page == PAGE_HOME);
    show(PAGE_BOOT);
    setup_status(PET_ENROLL_COMPLETE);
    finish_splash(NULL);
    code_clicked(NULL);
    assert(s_page == PAGE_HOME); /* completion before code UI, including reboot */
    pet_control_library_t library={.count=2};
    strcpy(library.next_cursor,"next-page");
    strcpy(library.items[0].name,"Not available");
    strcpy(library.items[1].name,"Glowberry\nnot a forged row");
    strcpy(library.items[1].build_id,"00000000-0000-4000-8000-000000000021");
    memset(library.items[1].pack_version,'v',80);library.items[1].pack_version[80]=0;
    library.items[1].bytes=2100000;library.items[1].installable=true;
    pet_onboarding_status_t status={
        .wifi=PET_ONBOARDING_WIFI_CONNECTED,
        .cloud=PET_ONBOARDING_CLOUD_CONNECTED,
        .conversation=PET_ONBOARDING_CONVERSATION_IDLE,
        .installation=PET_ONBOARDING_INSTALL_READY,
        .installation_percent=100,.battery_valid=true,
        .battery_percent=72,.battery_charging=true,.has_pet=true,.pet_linked=true,
        .slots={{.state=PET_SLOT_ACTIVE,.bytes=2100000},{0}},
        .volume=55,.volume_valid=true,.brightness=70,.brightness_valid=true};
    strcpy(status.ssid,"Saved home");strcpy(status.pack_name,"Glowberry");
    strcpy(status.pack_version,"1.0.4");strcpy(status.build_id,"00000000-0000-4000-8000-000000000021");
    strcpy(status.firmware_version,"vnext-0.1.0-qol1");
    pet_onboarding_update_status(&status);
    show(PAGE_HOME);
    assert_ascii(s_screen);
    assert(!strcmp(lv_label_get_text(s_pets_value),"GLOWBERRY"));
    assert(strstr(log_text(3),"Glowberry")||strstr(log_text(3),"GLOWBERRY"));
    assert(strstr(log_text(1),"vnext-0.1.0-qol1"));
    assert(strstr(log_text(5),"A 2051K")&&strstr(log_text(5),"B 0K"));
    assert(strstr(log_text(18),"derived from Share Tech Mono")&&strstr(log_text(20),"SIL OFL 1.1: see ABOUT"));
    /* ABOUT tells why aipets.com exists, then the open-source notices: the
     * font's notice and license, verbatim, on the device. */
    assert(find_text(s_menu,"ABOUT")&&!find_text(s_menu,"LICENSES"));
    about_clicked(NULL);
    assert(s_page==PAGE_ABOUT);
    lv_obj_t *story=find_text(s_screen,about_story);
    assert(story&&find_text(s_screen,"aipets.com")&&find_text(s_screen,about_notices));
    /* The story's own line breaks fit the 360px layout's column: none added.
     * A smaller screen's column may wrap them further, and the page scrolls. */
    lv_obj_update_layout(s_screen);
    if (pet_board_current()->width >= 360)
        assert(lv_obj_get_height(story)==11*lv_font_get_line_height(FONT_TEXT));
    else
        assert(lv_obj_get_height(story)>=11*lv_font_get_line_height(FONT_TEXT));
    lv_obj_t *license=find_text(s_screen,pet_menu_font_license);
    assert(license&&lv_label_get_text(license)==pet_menu_font_license);
    assert(strstr(pet_menu_font_license,"Reserved Font Name 'Share'")&&strstr(pet_menu_font_license,"SIL OPEN FONT LICENSE"));
    assert_ascii(s_screen);
    assert_page_on_glass();
    /* A finger scrolls the page down to the notices, and pulling past their
     * end stays on the page; the handle goes back. */
    lv_obj_t *about=lv_obj_get_parent(license);
    drag(180,250,180,110);
    assert(s_page==PAGE_ABOUT&&lv_obj_get_scroll_y(about)>0);
    lv_obj_scroll_to_y(about,LV_COORD_MAX,LV_ANIM_OFF);
    drag(180,250,180,110);
    run(400);
    assert(s_page==PAGE_ABOUT&&lv_obj_get_scroll_bottom(about)==0);
    drag(180,340,180,230);
    assert(s_page==PAGE_HOME);
    run(1500); /* the status line types itself in again */
    assert(!strcmp(lv_label_get_text(s_status_label),"ALL SYSTEMS NOMINAL"));
    status.single_pet_slot=true;pet_onboarding_update_status(&status);
    assert(strstr(log_text(5),"1 slot")&&strstr(log_text(5),"no backup"));
    status.single_pet_slot=false;status.pet_capacity=3;status.pet_count=2;status.pet_index=2;
    pet_onboarding_update_status(&status);
    assert(strstr(log_text(5),"2 of 3 slots"));
    assert(!strcmp(lv_label_get_text(s_pets_label),"PET 2/2"));
    static const char *const wifi_words[]={"OFFLINE","JOINING","CONNECTED"};
    for(unsigned value=PET_ONBOARDING_WIFI_DISCONNECTED;value<=PET_ONBOARDING_WIFI_CONNECTED;++value) {
        status.wifi=(pet_onboarding_wifi_state_t)value;
        pet_onboarding_update_status(&status);
        assert(!strcmp(lv_label_get_text(s_wifi_state),wifi_words[value]));
    }
    assert(!strcmp(lv_label_get_text(s_status_label),"ALL SYSTEMS NOMINAL"));
    status.wifi=PET_ONBOARDING_WIFI_CONNECTING;pet_onboarding_update_status(&status);
    assert(!strcmp(lv_label_get_text(s_status_label),"WIFI OFFLINE"));
    status.wifi=PET_ONBOARDING_WIFI_CONNECTED;
    static const char *const cloud_words[]={"needs pairing","offline","syncing","online","needs help"};
    for(unsigned value=PET_ONBOARDING_CLOUD_PAIRING;value<=PET_ONBOARDING_CLOUD_ATTENTION;++value) {
        status.cloud=(pet_onboarding_cloud_state_t)value;
        pet_onboarding_update_status(&status);
        assert(!strcmp(log_text(9),cloud_words[value]));
    }
    assert(!strcmp(lv_label_get_text(s_status_label),"NEEDS ATTENTION"));
    status.cloud=PET_ONBOARDING_CLOUD_CONNECTED;
    static const char *const voice_words[]={"CONNECTING","READY","LISTENING","THINKING","SPEAKING","ERROR"};
    for(unsigned value=PET_ONBOARDING_CONVERSATION_UNAVAILABLE;value<=PET_ONBOARDING_CONVERSATION_ERROR;++value) {
        status.conversation=(pet_onboarding_conversation_state_t)value;
        pet_onboarding_update_status(&status);
        assert(!strcmp(lv_label_get_text(s_voice_value),voice_words[value]));
    }
    /* Why a pet cannot talk, in the order the owner can fix it. */
    status.conversation=PET_ONBOARDING_CONVERSATION_UNAVAILABLE;status.pet_linked=false;
    pet_onboarding_update_status(&status);
    assert(!strcmp(lv_label_get_text(s_voice_value),"NOT LINKED"));
    status.cloud=PET_ONBOARDING_CLOUD_PAIRING;pet_onboarding_update_status(&status);
    assert(!strcmp(lv_label_get_text(s_voice_value),"NOT PAIRED"));
    status.wifi=PET_ONBOARDING_WIFI_DISCONNECTED;pet_onboarding_update_status(&status);
    assert(!strcmp(lv_label_get_text(s_voice_value),"NO WIFI"));
    status.wifi=PET_ONBOARDING_WIFI_CONNECTED;status.cloud=PET_ONBOARDING_CLOUD_CONNECTED;status.pet_linked=true;
    static const char *const sync_words[]={"idle","queued","downloading","verifying","activating","ready","failed"};
    for(unsigned value=PET_ONBOARDING_INSTALL_IDLE;value<=PET_ONBOARDING_INSTALL_FAILED;++value) {
        status.installation=(pet_onboarding_install_state_t)value;
        status.installation_percent=value*100/PET_ONBOARDING_INSTALL_FAILED;
        pet_onboarding_update_status(&status);
        assert(strstr(log_text(13),sync_words[value]));
    }
    status.installation=PET_ONBOARDING_INSTALL_DOWNLOADING;status.installation_percent=42;
    pet_onboarding_update_status(&status);
    assert(!strcmp(lv_label_get_text(s_status_label),"RECEIVING PET 42%")&&!lv_obj_has_flag(s_progress,LV_OBJ_FLAG_HIDDEN));
    status.conversation=PET_ONBOARDING_CONVERSATION_IDLE;
    status.installation=PET_ONBOARDING_INSTALL_READY;
    status.installation_percent=100;
    pet_onboarding_update_status(&status);
    assert(lv_obj_has_flag(s_progress,LV_OBJ_FLAG_HIDDEN));

    /* Volume: heard at once while dragged, kept once it settles, muted by
     * the speaker and restored by it. */
    assert(s_slider[SETTING_VOLUME]&&s_slider[SETTING_BRIGHTNESS]);
    assert(!strcmp(lv_label_get_text(s_slider_value[SETTING_VOLUME]),"55%"));
    lv_slider_set_value(s_slider[SETTING_VOLUME],73,LV_ANIM_OFF);
    lv_obj_send_event(s_slider[SETTING_VOLUME],LV_EVENT_VALUE_CHANGED,NULL);
    assert(volume_now==70&&volume_kept==-1&&!strcmp(lv_label_get_text(s_slider_value[SETTING_VOLUME]),"70%"));
    status.volume=55;pet_onboarding_update_status(&status); /* a stale snapshot never undoes the owner */
    assert(s_setting[SETTING_VOLUME]==70);
    run(1000);
    assert(volume_kept==70);
    mute_clicked(NULL);
    assert(volume_now==0&&!strcmp(lv_label_get_text(s_slider_value[SETTING_VOLUME]),"OFF"));
    mute_clicked(NULL);
    assert(volume_now==70);
    run(1000);
    assert(volume_kept==70);
    status.volume=40;pet_onboarding_update_status(&status); /* once kept, the device's level shows */
    assert(!strcmp(lv_label_get_text(s_slider_value[SETTING_VOLUME]),"40%"));
    /* Brightness never goes below the floor that keeps the screen readable. */
    lv_slider_set_value(s_slider[SETTING_BRIGHTNESS],2,LV_ANIM_OFF);
    lv_obj_send_event(s_slider[SETTING_BRIGHTNESS],LV_EVENT_VALUE_CHANGED,NULL);
    assert(brightness_now==PET_BRIGHTNESS_MIN);
    lv_slider_set_value(s_slider[SETTING_BRIGHTNESS],100,LV_ANIM_OFF);
    lv_obj_send_event(s_slider[SETTING_BRIGHTNESS],LV_EVENT_VALUE_CHANGED,NULL);
    assert(brightness_now==100&&brightness_kept==-1);

    retry_clicked(NULL);assert(retries==1);
    assert(!strcmp(lv_label_get_text(s_status_label),"SYNC QUEUED"));
    run(3600);
    assert(!strcmp(lv_label_get_text(s_status_label),"ALL SYSTEMS NOMINAL")); /* the message stands, then rests */
    restart_prompt_clicked(NULL);assert(s_page==PAGE_RESTART_CONFIRM);
    assert_page_on_glass();
    restart_clicked(NULL);assert(restarts==1);
    home_clicked(NULL);assert(s_page==PAGE_HOME);
    library_clicked(NULL);
    pet_onboarding_set_library(&library);
    assert(s_page==PAGE_LIBRARY&&lv_dropdown_get_option_count(s_library_dropdown)==2);
    lv_dropdown_open(s_library_dropdown);center_pet_list(NULL);
    assert_glass_bounds(lv_dropdown_get_list(s_library_dropdown));lv_dropdown_close(s_library_dropdown);
    choose_pet_clicked(NULL);assert(s_page==PAGE_LIBRARY&&!selected_build[0]);
    lv_dropdown_set_selected(s_library_dropdown,1);choose_pet_clicked(NULL);
    assert(s_page==PAGE_CONFIRM);
    assert_page_on_glass();
    assert_ascii(s_screen);
    swipe_up();assert(s_page==PAGE_LIBRARY);
    lv_dropdown_set_selected(s_library_dropdown,1);choose_pet_clicked(NULL);
    confirm_pet_clicked(NULL);assert(s_page==PAGE_HOME&&!strcmp(selected_build,library.items[1].build_id));
    pet_onboarding_set_library(&library);next_library_clicked(NULL);assert(requested_next);

    /* Opened from the pet, the deck swipes shut: the tube powers off, then the
     * pet shows. Sub-pages swipe back one step. */
    online=false;setup_status(PET_ENROLL_WAIT_WIFI);pet_onboarding_open_from_ui();
    assert(s_page==PAGE_HOME&&s_tube[0]);
    run(400);
    back_clicked(NULL);assert(s_page==PAGE_WIFI);
    /* Swipes up from the handle travel far enough for LVGL's 50px gesture on
     * the 240px screen too. */
    drag(180,340,180,160);
    assert(s_page==PAGE_HOME&&!returned);
    lv_slider_set_value(s_slider[SETTING_VOLUME],30,LV_ANIM_OFF);
    lv_obj_send_event(s_slider[SETTING_VOLUME],LV_EVENT_VALUE_CHANGED,NULL);
    assert(volume_now==30&&volume_kept!=30);
    drag(180,344,180,160);
    assert(s_leaving&&!returned&&volume_kept==30); /* a level still settling is kept on the way out */
    run(400);
    assert(returned==1&&lv_screen_active()==other_screen&&!s_leaving);
    lv_screen_load(s_display_screen);
    handle_clicked(NULL); /* tapping the handle does what swiping up does */
    run(400);
    assert(returned==2);
    lv_screen_load(s_display_screen);
    /* Pulling up past the end of the log closes the deck; a long pull from
     * the top only scrolls it. */
    lv_obj_update_layout(s_screen);
    drag(180,260,180,60);
    run(400);
    assert(returned==2&&lv_obj_get_scroll_y(s_menu)>0&&!s_leaving);
    lv_obj_scroll_to_y(s_menu,LV_COORD_MAX,LV_ANIM_OFF);
    drag(180,260,180,40);
    run(400);
    assert(returned==3);
    lv_screen_load(s_display_screen);
    /* When the pet shows from another task, the tube stays dark until it
     * does, and powers the deck back on if it never shows. */
    return_later=true;
    swipe_up();run(400);
    assert(returned==4&&lv_screen_active()==s_display_screen&&s_leaving&&s_tube[0]&&
           lv_obj_get_height(s_tube[0])==board->height/2);
    run(2100);
    assert(!s_leaving&&s_page==PAGE_HOME&&s_tube[2]&&!s_leave_timer);
    run(400);
    swipe_up();run(400);
    assert(returned==5&&s_leaving);
    lv_screen_load(other_screen);run(120); /* the pet takes the screen late */
    assert(!s_leaving&&!s_leave_timer&&lv_screen_active()==other_screen);
    return_later=false;
    lv_screen_load(s_display_screen);
    /* A level the command queue had no room for is kept on a later try. */
    volume_fails=true;
    lv_slider_set_value(s_slider[SETTING_VOLUME],80,LV_ANIM_OFF);
    lv_obj_send_event(s_slider[SETTING_VOLUME],LV_EVENT_VALUE_CHANGED,NULL);
    run(1000);
    assert(s_setting_unsaved[SETTING_VOLUME]&&volume_kept!=80);
    volume_fails=false;run(1000);
    assert(!s_setting_unsaved[SETTING_VOLUME]&&volume_kept==80);
    /* A long network list scrolls under a swipe; the handle still goes back. */
    pet_wifi_network_t many[7];
    for(unsigned i=0;i<7;++i){memset(&many[i],0,sizeof(many[i]));snprintf(many[i].ssid,sizeof(many[i].ssid),"net-%u",i);many[i].rssi=-60;many[i].secured=true;}
    scan_finished(many,7,ESP_OK);
    back_clicked(NULL);assert(s_page==PAGE_WIFI);
    drag(180,200,180,90);
    assert(s_page==PAGE_WIFI&&lv_obj_get_scroll_y(s_network_list)>0);
    drag(180,340,180,230);
    assert(s_page==PAGE_HOME);
    scan_finished(networks,3,ESP_OK);
    /* A device with a pet but no pairing still reaches pairing: its own
     * link code first, the typed code as the fallback. */
    s_enrolled=false;status.cloud=PET_ONBOARDING_CLOUD_PAIRING;pet_onboarding_update_status(&status);
    show(PAGE_WIFI);show(PAGE_HOME);
    assert(find_text(s_menu,"LINK")&&!find_text(s_menu,"SYNC")&&!find_text(s_menu,"PAIR"));
    online=true;code_clicked(NULL);assert(s_page==PAGE_CODE);
    swipe_up();assert(s_page==PAGE_WIFI&&find_text(s_screen,"NEXT"));
    link_flows();
    status.cloud=PET_ONBOARDING_CLOUD_CONNECTED;pet_onboarding_update_status(&status);
    assert(s_enrolled);show(PAGE_HOME);assert(find_text(s_menu,"SYNC"));
    /* Moving to another account shows only on a Pocket Terminal built with
     * CONFIG_PET_DEVICE_RELINK, which is off by default. */
    assert(!find_text(s_menu,"LINK TO ANOTHER ACCOUNT")==!(CONFIG_PET_POCKET_TERMINAL&&CONFIG_PET_DEVICE_RELINK));
    online=false;
    hidden_clicked(NULL);lv_textarea_set_text(s_password,"private-password");
    status.retry_seconds=17;strcpy(status.support_error_code,"HTTP_BACKOFF");
    pet_onboarding_update_status(&status); /* Status must not dismiss typed credentials. */
    pet_onboarding_set_library(&library); /* A delayed library response must not either. */
    assert(s_page==PAGE_PASSWORD&&!strcmp(lv_textarea_get_text(s_password),"private-password"));
    pet_onboarding_status_t invalid=status;
    invalid.wifi=(pet_onboarding_wifi_state_t)-1;
    memset(invalid.pack_name,'X',sizeof(invalid.pack_name));
    pet_onboarding_update_status(&invalid); /* Reject malformed snapshots atomically. */
    invalid=status;invalid.pet_capacity=3;invalid.pet_count=4;pet_onboarding_update_status(&invalid);
    invalid=status;invalid.volume=101;pet_onboarding_update_status(&invalid);
    invalid=status;invalid.brightness=PET_BRIGHTNESS_MIN-1;pet_onboarding_update_status(&invalid);
    assert(s_page==PAGE_PASSWORD&&!strcmp(lv_textarea_get_text(s_password),"private-password"));
    assert(!strcmp(s_device_status.pack_name,"Glowberry")&&s_device_status.pet_count==2&&s_device_status.volume==40);
    swipe_up();assert(s_page==PAGE_WIFI);
    swipe_up();assert(s_page==PAGE_HOME);
    assert(strstr(log_text(14),"HTTP_BACKOFF"));
    show(PAGE_HOME);retry_clicked(NULL);assert(retries==2);
    restart_prompt_clicked(NULL);assert(s_page==PAGE_RESTART_CONFIRM);
    swipe_up();assert(s_page==PAGE_HOME);
    restart_prompt_clicked(NULL);restart_clicked(NULL);assert(restarts==2);
    /* Without a pet the deck is the whole menu: swiping up stays. */
    status.has_pet=false;pet_onboarding_update_status(&status);show(PAGE_HOME);
    swipe_up();run(400);assert(s_page==PAGE_HOME&&returned==5);
    story_screens(&status);
    story_mode_screens();
    no_story_sliders();
    frame_follows_path();
    story_game_screens();
    themes();
    puts("onboarding: real LVGL deck, swipe-up (handle, pull past the log, late pet), settings with retries, pairing, link code, about, pet picker, story screens, the story log, story mode, story games and round bounds passed");
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    pet_usb_wifi_request_t request = {.action = PET_USB_WIFI_INFO,
        .request_id = "5b8e2bfc-5573-4acb-8ac7-9aa7fbb0433c", .mac = "2a:00:00:00:00:01",
        .ssid = "Open network"};
    online = true;
    usb_wifi_request(&request, NULL);
    assert(strstr(usb_output, "Joined network") && strstr(usb_output, "Saved network"));
    assert(!strstr(usb_output, "password"));
    unsigned restarts_before_usb = restarts;
    request.action = PET_USB_WIFI_SAVED;
    usb_wifi_request(&request, NULL);
    assert(strstr(usb_output, "SAVED_NETWORK_MISSING") && !usb_stores && restarts == restarts_before_usb);
    request.action = PET_USB_WIFI_SAVE;
    request.mac[0] = '0';
    usb_wifi_request(&request, NULL);
    assert(strstr(usb_output, "WRONG_DEVICE") && !usb_stores);
    request.mac[0] = '2';
    s_device_status.installation = PET_ONBOARDING_INSTALL_DOWNLOADING;
    usb_wifi_request(&request, NULL);
    assert(strstr(usb_output, "BUSY") && !usb_stores);
    s_device_status.installation = 0;
    store_fails = true;
    usb_wifi_request(&request, NULL);
    assert(strstr(usb_output, "SAVE_FAILED") && restarts == restarts_before_usb);
    store_fails = false;
    usb_saving = true;
    usb_wifi_request(&request, NULL);
    /* The saved network joins live once its reply has left, with no restart. */
    assert(usb_stores == 2 && !stored_password[0] && joins == 1 && restarts == restarts_before_usb);
    join_fails = true;
    usb_wifi_request(&request, NULL);
    assert(joins == 2 && restarts == restarts_before_usb + 1);
    join_fails = false;
    /* Forget removes a saved network by name; the device stays on its network. */
    request.action = PET_USB_WIFI_FORGET;
    strcpy(request.ssid, "Office");
    usb_wifi_request(&request, NULL);
    assert(strstr(usb_output, "\"status\":\"forgotten\"") && !strcmp(forgotten_ssid, "Office") &&
           wifi_refreshes == 1 && joins == 2 && restarts == restarts_before_usb + 1);
    strcpy(request.ssid, "Only network");
    usb_wifi_request(&request, NULL);
    assert(strstr(usb_output, "LAST_NETWORK") && wifi_refreshes == 1);
    /* A disconnected partial credential cannot survive an idle deadline. */
    const char partial[] = "AIPETS_WIFI {\"password\":\"unfinished-secret";
    pet_usb_wifi_feed(&s_usb_wifi, (const uint8_t *)partial, strlen(partial), usb_wifi_request, NULL);
    s_usb_wifi_last_input = lv_tick_get();
    lv_tick_inc(2001);
    usb_wifi_poll();
    pet_usb_wifi_t cleared = {0};
    assert(!memcmp(&s_usb_wifi, &cleared, sizeof(cleared)));
    puts("USB Wi-Fi: identity, saved/open networks, failures, ack before a live join (restart if it can't start) and idle clearing passed");
#endif
    lv_deinit();
    return 0;
}
