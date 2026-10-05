#pragma once

#include "pet_config.h"
#include "pet_control_wire.h"

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

/* Starts an asset-independent on-device Wi-Fi and website-code wizard.
 * No conversation or pet renderer is initialized by this shell. */
esp_err_t pet_onboarding_start(const pet_config_t *config, const char *https_origin);
typedef struct {
    bool (*library)(bool next_page);
    bool (*select)(const char *build_id);
    void (*return_to_pet)(void);
    bool (*retry_sync)(void);
    bool (*restart)(void);
    /* Optional: the menu's volume and brightness controls, each shown only
     * when bound. `save` is false while the owner is still choosing (apply
     * it now) and true once they settle on it (keep it). */
    bool (*volume)(uint8_t volume, bool save);
    bool (*brightness)(uint8_t brightness, bool save);
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
