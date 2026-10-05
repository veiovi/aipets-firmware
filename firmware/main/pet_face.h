#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_animation_profile.h"
#include "pet_network.h"
#include "pet_voice.h"
#include "pet_ai.h"
#include "pet_battery.h"
#include "pet_face_profile.h"
#include "pet_speech_mouth.h"

typedef enum {
    PET_FACE_BOOTING,
    PET_FACE_PROVISIONING,
    PET_FACE_CONNECTING,
    PET_FACE_IDLE,
    PET_FACE_LISTENING,
    PET_FACE_THINKING,
    PET_FACE_SPEAKING,
    PET_FACE_OFFLINE,
    PET_FACE_ERROR,
} pet_face_state_t;

typedef enum {
    PET_EXPRESSION_IDLE,
    PET_EXPRESSION_HAPPY,
    PET_EXPRESSION_CURIOUS,
    PET_EXPRESSION_SURPRISED,
    PET_EXPRESSION_SLEEPY,
    PET_EXPRESSION_CONCERNED,
    PET_EXPRESSION_EXCITED,
    PET_EXPRESSION_SHY,
    PET_EXPRESSION_ANGRY,
    PET_EXPRESSION_CONFUSED,
    PET_EXPRESSION_DETERMINED,
    PET_EXPRESSION_DISGUST,
    PET_EXPRESSION_EMBARRASSED,
    PET_EXPRESSION_FEAR,
    PET_EXPRESSION_SAD,
    PET_EXPRESSION_COUNT,
} pet_expression_t;

/* The original eight wire values retain their numeric IDs. Additional
 * expressions are available to packs that include their matching emotion;
 * the procedural boot face safely treats them as neutral. */
bool pet_expression_from_wire(const char *name, pet_expression_t *value);

typedef void (*pet_face_tap_callback_t)(void);
typedef void (*pet_face_settings_callback_t)(void);
typedef void (*pet_face_settings_closed_callback_t)(bool user_initiated);
typedef void (*pet_face_volume_callback_t)(uint8_t volume);
typedef void (*pet_face_brightness_callback_t)(uint8_t brightness);
typedef void (*pet_face_shake_sensitivity_callback_t)(uint8_t sensitivity);
typedef enum {
    PET_FACE_CHANGE_SETTINGS = 0,
    PET_FACE_CHANGE_SWIPE,
} pet_face_change_source_t;
typedef void (*pet_face_id_callback_t)(const char *face_id,
                                       pet_face_change_source_t source);
typedef void (*pet_face_recording_timeout_callback_t)(uint16_t seconds);
typedef void (*pet_face_animation_profile_callback_t)(pet_animation_profile_t profile);
typedef void (*pet_face_voice_callback_t)(pet_voice_t voice);
typedef void (*pet_face_ai_mode_callback_t)(pet_ai_mode_t mode);
typedef void (*pet_face_speech_profile_callback_t)(pet_speech_profile_t profile);
typedef void (*pet_face_cartesia_voice_gender_callback_t)(pet_cartesia_voice_gender_t gender);
typedef void (*pet_face_realtime_model_callback_t)(pet_realtime_model_t model);
typedef void (*pet_face_realtime_voice_callback_t)(pet_realtime_voice_t voice);
typedef void (*pet_face_gesture_callback_t)(uint8_t gesture);
typedef void (*pet_face_realtime_boost_callback_t)(pet_realtime_boost_t boost);
typedef void (*pet_face_speech_mouth_callback_t)(pet_speech_mouth_mode_t mode);
typedef void (*pet_face_wifi_join_callback_t)(const char *ssid, const char *password);
/* +1: swipe left, next installed pet; -1: swipe right, previous one. */
typedef void (*pet_face_swipe_callback_t)(int direction);

typedef struct {
    pet_face_tap_callback_t tapped;
    /* When set, horizontal swipes switch installed pets instead of faces. */
    pet_face_swipe_callback_t pet_swiped;
    pet_face_settings_callback_t settings_opened;
    pet_face_settings_closed_callback_t settings_closed;
    pet_face_settings_callback_t wifi_scan_requested;
    pet_face_volume_callback_t volume_changed;
    pet_face_brightness_callback_t brightness_changed;
    pet_face_shake_sensitivity_callback_t shake_sensitivity_changed;
    pet_face_id_callback_t face_changed;
    pet_face_recording_timeout_callback_t recording_timeout_changed;
    pet_face_animation_profile_callback_t animation_profile_changed;
    pet_face_voice_callback_t voice_changed;
    pet_face_ai_mode_callback_t ai_mode_changed;
    pet_face_speech_profile_callback_t speech_profile_changed;
    pet_face_cartesia_voice_gender_callback_t cartesia_voice_gender_changed;
    pet_face_realtime_model_callback_t realtime_model_changed;
    pet_face_realtime_voice_callback_t realtime_voice_changed;
    pet_face_gesture_callback_t gesture_requested;
    pet_face_realtime_boost_callback_t realtime_boost_changed;
    pet_face_speech_mouth_callback_t speech_mouth_changed;
    pet_face_wifi_join_callback_t wifi_join_requested;
} pet_face_callbacks_t;

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
                        pet_speech_mouth_mode_t initial_speech_mouth_mode);
void pet_face_set_state(pet_face_state_t state);
void pet_face_show(void); /* Return from the native setup/library screen. */
void pet_face_set_expression(pet_expression_t expression);
/* The face reads the speech level on each animation tick of this period. */
#define PET_FACE_TICK_MS 33
void pet_face_set_audio_level(uint8_t level);
void pet_face_set_speech_articulation(uint8_t level);
void pet_face_begin_speech_stream(pet_speech_mouth_mode_t mode);
/* Starts a whole-face gesture and returns the actual active gesture id.
 * Reduced-motion policy may substitute a different gesture. NONE means the
 * request was invalid, unavailable, or suppressed while settings are open. */
uint8_t pet_face_trigger_gesture(uint8_t gesture);
bool pet_face_can_trigger_ambient_gesture(void);
/* Pocket Terminal. The pet's touch reaction, as when the screen is pressed;
 * false while settings, a system state or no pet is showing. */
bool pet_face_touch_reaction(void);
/* Pocket Terminal. Types a one-line terminal message across the lower face,
 * one character per 24 ms, holds it, then clears it. Longer text is cut. */
#define PET_FACE_TERMINAL_MAX 26
void pet_face_announce(const char *text);
bool pet_face_shake_detection_enabled(void);
pet_face_state_t pet_face_get_state(void);
uint32_t pet_face_render_heartbeat(void);
/* Control-task health evidence: a visible, bound pack UI with a recent actual
 * LVGL timer tick. Acquires the display lock; do not call from an LVGL callback. */
bool pet_face_pack_ready(void);
esp_err_t pet_face_set_id(const char *face_id);
esp_err_t pet_face_get_id(char *face_id, size_t capacity);
esp_err_t pet_face_get_agent_id(char *id, size_t capacity);
void pet_face_id_save_failed(void);
esp_err_t pet_face_set_gender(pet_face_gender_t gender);
pet_face_gender_t pet_face_get_gender(void);
esp_err_t pet_face_set_brightness(uint8_t brightness);
void pet_face_brightness_save_failed(void);
esp_err_t pet_face_set_shake_sensitivity(uint8_t sensitivity);
void pet_face_shake_sensitivity_save_failed(void);
void pet_face_set_battery(const pet_battery_snapshot_t *snapshot);
esp_err_t pet_face_set_recording_timeout(uint16_t seconds);
void pet_face_recording_timeout_save_failed(void);
esp_err_t pet_face_set_animation_profile(pet_animation_profile_t profile);
pet_animation_profile_t pet_face_get_animation_profile(void);
void pet_face_animation_profile_save_failed(void);
esp_err_t pet_face_set_voice(pet_voice_t voice);
pet_voice_t pet_face_get_voice(void);
void pet_face_voice_save_failed(void);
esp_err_t pet_face_set_ai_mode(pet_ai_mode_t mode);
esp_err_t pet_face_set_speech_profile(pet_speech_profile_t mode,bool allow_cartesia_batch,bool allow_cartesia_realtime,bool allow_fish);
void pet_face_speech_profile_save_failed(void);
esp_err_t pet_face_set_cartesia_voice_gender(pet_cartesia_voice_gender_t gender);
esp_err_t pet_face_set_realtime_model(pet_realtime_model_t model);
esp_err_t pet_face_set_realtime_voice(pet_realtime_voice_t voice);
esp_err_t pet_face_set_realtime_boost(pet_realtime_boost_t boost);
esp_err_t pet_face_set_speech_mouth_mode(pet_speech_mouth_mode_t mode);
pet_speech_mouth_mode_t pet_face_get_speech_mouth_mode(void);
void pet_face_speech_mouth_save_failed(void);
void pet_face_ai_setting_save_failed(void);
void pet_face_set_wifi_networks(const pet_wifi_network_t *networks, size_t count,
                                esp_err_t result);
void pet_face_set_wifi_status(const char *status);
