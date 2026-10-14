#pragma once

#include "lvgl.h"
#include "pet_config.h"
#include "pet_control_wire.h"
#include "pet_session_wire.h"

typedef enum {
    PET_ONBOARDING_WIFI_DISCONNECTED,
    PET_ONBOARDING_WIFI_CONNECTING,
    PET_ONBOARDING_WIFI_CONNECTED,
} pet_onboarding_wifi_state_t;

typedef enum {
    PET_ONBOARDING_CLOUD_PAIRING,
    PET_ONBOARDING_CLOUD_OFFLINE,
    PET_ONBOARDING_CLOUD_SYNCING,
    PET_ONBOARDING_CLOUD_CONNECTED,
    PET_ONBOARDING_CLOUD_ATTENTION,
} pet_onboarding_cloud_state_t;

typedef enum {
    PET_ONBOARDING_CONVERSATION_UNAVAILABLE,
    PET_ONBOARDING_CONVERSATION_IDLE,
    PET_ONBOARDING_CONVERSATION_LISTENING,
    PET_ONBOARDING_CONVERSATION_THINKING,
    PET_ONBOARDING_CONVERSATION_SPEAKING,
    PET_ONBOARDING_CONVERSATION_ERROR,
} pet_onboarding_conversation_state_t;

typedef enum {
    PET_ONBOARDING_INSTALL_IDLE,
    PET_ONBOARDING_INSTALL_REQUESTED,
    PET_ONBOARDING_INSTALL_DOWNLOADING,
    PET_ONBOARDING_INSTALL_VERIFYING,
    PET_ONBOARDING_INSTALL_ACTIVATING,
    PET_ONBOARDING_INSTALL_READY,
    PET_ONBOARDING_INSTALL_FAILED,
} pet_onboarding_install_state_t;

typedef struct {
    pet_slot_state_t state;
    uint32_t bytes;
} pet_onboarding_slot_status_t;

typedef struct {
    pet_onboarding_wifi_state_t wifi;
    pet_onboarding_cloud_state_t cloud;
    pet_onboarding_conversation_state_t conversation;
    pet_onboarding_install_state_t installation;
    char ssid[PET_SSID_MAX];
    char pack_name[81];
    char pack_version[81];
    char build_id[37];
    char firmware_version[33];
    char support_error_code[33];
    pet_onboarding_slot_status_t slots[2];
    unsigned installation_percent;
    unsigned retry_seconds;
    uint8_t battery_percent;
    bool battery_valid;
    bool battery_charging;
    bool has_pet;
    bool single_pet_slot;
    /* Installed pets when the device keeps several (Pocket Terminal):
     * pet_capacity slots, pet_count of them ready, and the pet on screen
     * at 1-based pet_index. All zero on builds that keep one pet at a time. */
    uint8_t pet_capacity;
    uint8_t pet_count;
    uint8_t pet_index;
    /* The pet's own name in terminal style ("LAVENDER_DRAGON"), when known;
     * otherwise the menu shows pack_name, its face ID. */
    char pet_name[17];
    /* The pet on screen can talk on this account once its session is ready
     * (the VOICE tile shows CONNECTING until then). False only for a pet with
     * no cloud release for this account, or one the cloud refused to select:
     * the tile shows NOT LINKED. */
    bool pet_linked;
    /* Speaker volume 0-100 and screen brightness PET_BRIGHTNESS_MIN-100,
     * each when valid. */
    uint8_t volume;
    bool volume_valid;
    uint8_t brightness;
    bool brightness_valid;
} pet_onboarding_status_t;

/* Starts an asset-independent on-device Wi-Fi and website-code wizard, in
 * the config's menu_style. No
 * conversation or pet renderer is initialized by this shell. */
esp_err_t pet_onboarding_start(const pet_config_t *config, const char *https_origin);
typedef struct {
    bool (*library)(bool next_page);
    bool (*select)(const char *build_id);
    void (*return_to_pet)(void);
    bool (*retry_sync)(void);
    bool (*restart)(void);
    /* Optional: joins the Wi-Fi network just stored over USB or on the Wi-Fi
     * page without a restart. Without it, storing one restarts the device. */
    bool (*join_wifi)(void);
    /* Optional: the menu's volume and brightness controls, each shown only
     * when bound. `save` is false while the owner is still choosing (apply
     * it now) and true once they settle on it (keep it). */
    bool (*volume)(uint8_t volume, bool save);
    bool (*brightness)(uint8_t brightness, bool save);
    /* Optional: the deck's pet picker, shown only when bound. Shows the next
     * installed pet (+1) or the previous one (-1); the deck then closes. */
    bool (*switch_pet)(int direction);
    /* Optional: the deck's STORIES page sends "story.load" of `story_id` or
     * "story.restart" (pet_network_story_request); ESP_ERR_INVALID_STATE when
     * the session cannot take it. */
    esp_err_t (*story)(const char *type, const char *story_id);
    /* Optional: BLESSINGS' USE asks the pet to use the blessing with this id
     * (story-game-v1); the pet then shows. With `speak` the blessing takes the
     * owner's words (story-blessings-v2), so the pet listens for them first.
     * False when it cannot be asked. */
    bool (*use_blessing)(const char *id, bool speak);
    /* Optional: the deck's STYLE card, shown only when bound. Keeps the
     * menu's look, a PET_MENU_STYLE_*, which the deck shows at once; false
     * when it cannot be kept, and the look stays. */
    bool (*style)(uint8_t style);
} pet_onboarding_control_t;
/* Register before start. Callbacks only enqueue work and never wait in LVGL;
 * return_to_pet shows the pet before it returns. */
void pet_onboarding_bind_control(const pet_onboarding_control_t *control);
void pet_onboarding_set_library(const pet_control_library_t *library);
void pet_onboarding_update_status(const pet_onboarding_status_t *status);
bool pet_onboarding_setup_ready(void);
/* Visible setup/recovery LVGL timer has actually run recently. A created but
 * stalled/hidden screen is not UI health evidence. */
bool pet_onboarding_ui_ready(void);
/* Called by the face settings callback while already holding the LVGL lock.
 * Swiping up on the menu leaves it again (return_to_pet). */
void pet_onboarding_open_from_ui(void);

/* The story screens a swipe on the pet opens (Pocket Terminal), like
 * pet_onboarding_open_from_ui. Each closes with the opposite swipe, or a tap
 * on its chevron. */
typedef enum {
    PET_ONBOARDING_BLESSINGS, /* a swipe from the left edge */
    PET_ONBOARDING_CHARACTER, /* a swipe from the right edge */
    PET_ONBOARDING_STORY,     /* a swipe up from the bottom */
} pet_onboarding_screen_t;
void pet_onboarding_open_screen(pet_onboarding_screen_t screen);
/* The story log of the pet with this face ID, oldest line first (the
 * story.log message): it replaces the last one, and `count` 0 clears it. The
 * log shows only while that pet is on screen. Any task; lines are copied. */
void pet_onboarding_set_story_log(const char *face_id, const char *const *lines, size_t count);
/* The story.state and story.list of the pet with this face ID, each replacing
 * the last: the STORY screen's header and the deck's STORIES page, shown only
 * while that pet is on screen. Any task; they are copied. */
void pet_onboarding_set_story_state(const char *face_id, const pet_story_state_t *state);
void pet_onboarding_set_story_list(const char *face_id, const pet_story_list_t *list);
/* The story frame over the pet's screen `parent`, hidden: a pixel rim on the
 * glass's edge and a STORY tab, in the menu's style, which it follows. Its
 * rim is green on the story's path, yellow on a detour and red hurrying back,
 * as the latest story.state says. It is drawn again only when one of them
 * changes; where the pet's frames repaint it, it is a few plain rectangles. */
lv_obj_t *pet_onboarding_story_frame(lv_obj_t *parent);
