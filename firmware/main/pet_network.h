#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_config.h"
#include "pet_battery.h"
#include "pet_face_profile.h"
#include "pet_ota.h"
#include "pet_control_wire.h"

typedef struct {
    uint32_t version;
    uint8_t volume;
    uint16_t recording_timeout_seconds;
    char face_id[PET_FACE_ID_MAX];
    pet_voice_t legacy_voice;
    char ai_pet_id[81];
} pet_synced_settings_t;
typedef struct {
    uint32_t version;
    char fingerprint[PET_CONFIG_FINGERPRINT_MAX];
    uint8_t volume;
    uint8_t brightness;
    uint8_t shake_sensitivity;
    uint16_t recording_timeout_seconds;
    pet_animation_profile_t animation_profile;
    char face_id[PET_FACE_ID_MAX];
    char ai_pet_id[PET_AI_PET_ID_MAX];
    pet_speech_profile_t speech_profile;
    /* speechMouthOffsetMs: 0 when the document leaves it out. `has_` keeps
     * whether it was sent, so the echo matches exactly. */
    bool has_speech_mouth_offset;
    int16_t speech_mouth_offset_ms;
} pet_synced_config_v2_t;
typedef struct { uint32_t version;pet_speech_profile_t profile;bool allow_cartesia_batch;bool allow_cartesia_realtime;bool allow_fish;char ai_pet_id[PET_AI_PET_ID_MAX]; } pet_speech_preference_t;
typedef enum { PET_WIFI_ADMIN_ADD=0,PET_WIFI_ADMIN_UPDATE,PET_WIFI_ADMIN_CONNECT,PET_WIFI_ADMIN_FORGET,PET_WIFI_ADMIN_PRIORITY } pet_wifi_admin_action_t;
typedef struct { char operation_id[PET_OPERATION_ID_MAX];pet_wifi_admin_action_t action;char ssid[PET_SSID_MAX];char password[PET_PASSWORD_MAX];uint8_t priority; } pet_wifi_admin_command_t;
typedef struct { char operation_id[PET_OPERATION_ID_MAX];uint32_t version;char credential[PET_TOKEN_MAX]; } pet_credential_prepare_t;
typedef struct { char operation_id[PET_OPERATION_ID_MAX];uint32_t version; } pet_credential_commit_t;

typedef struct {
    void (*connected)(bool ready);
    void (*state)(const char *state);
    void (*expression)(const char *expression);
    void (*gesture)(uint8_t gesture);
    esp_err_t (*audio_start)(uint32_t stream_id, uint32_t sample_rate);
    esp_err_t (*audio_chunk)(uint32_t stream_id, uint32_t sequence, const uint8_t *pcm, size_t length);
    void (*audio_end)(uint32_t stream_id, uint32_t final_sequence);
    void (*session_error)(const char *code, const char *message, bool recoverable);
    void (*face_profile)(const pet_face_profile_t *profile);
    /* Return true only after the settings event has been accepted by the
     * application queue. The network cache advances only after a later
     * durable-apply acknowledgement. */
    bool (*settings_received)(const pet_synced_settings_t *settings);
    bool (*config_v2_received)(const pet_synced_config_v2_t *settings);
    bool (*wifi_admin_received)(const pet_wifi_admin_command_t *command);
    bool (*credential_prepare_received)(const pet_credential_prepare_t *command);
    bool (*credential_commit_received)(const pet_credential_commit_t *command);
    bool (*speech_preference_received)(const pet_speech_preference_t *preference);
    esp_err_t (*ota_update)(const pet_ota_request_t *request);
} pet_network_callbacks_t;

#define PET_WIFI_SCAN_MAX 12

typedef struct {
    char ssid[PET_SSID_MAX];
    int8_t rssi;
    bool secured;
    bool saved;
} pet_wifi_network_t;

typedef void (*pet_network_scan_callback_t)(const pet_wifi_network_t *networks,
                                            size_t count, esp_err_t result);

esp_err_t pet_network_start(const pet_config_t *config, const pet_network_callbacks_t *callbacks);
/* Station + scan service with no dependency on enrollment or conversation. */
esp_err_t pet_network_start_wifi_only(const pet_config_t *config);
/* Register HTTPS settings callbacks and reuse telemetry before Brain admission. */
esp_err_t pet_network_start_management(const pet_network_callbacks_t *callbacks);
bool pet_network_wifi_is_ready(void);
/* vNext worker owns these calls. Freeze/stop completes before changing bound
 * identity/config; Wi-Fi remains running. No automatic bound reconnect. */
esp_err_t pet_network_start_bound(const pet_config_t *config,const pet_network_callbacks_t *callbacks,
                                   const char *device_id,const pet_control_context_t *context);
void pet_network_freeze_bound(void);
void pet_network_stop(void);
bool pet_network_is_ready(void);
/* Three-pet switching over the open bound session (session-rebind-v1,
 * pet_session_wire.h). bound_to: the session is ready and bound to exactly
 * this binding and configuration. can_rebind: a ready bound session whose
 * gateway listed the capability, with no rebind awaiting its answer. rebind
 * sends device.binding.changed; readiness then waits for
 * gateway.binding.accepted, which calls connected(true). A refusal or a closed
 * socket calls session_error or connected(false), and the caller reconnects.
 * Anything but ESP_OK means no rebind was sent. */
bool pet_network_bound_to(const pet_control_context_t *context);
bool pet_network_can_rebind(void);
esp_err_t pet_network_rebind(const pet_control_context_t *context);
/* Called only by the existing control worker while it owns the resource. */
bool pet_network_management_request(char *out, size_t capacity);
bool pet_network_management_response(const char *json, size_t bytes);
void pet_network_set_brain_token(const char *token);
esp_err_t pet_network_set_voice(pet_voice_t voice);
void pet_network_set_ai_pet_id(const char *ai_pet_id);
esp_err_t pet_network_get_ai_pet_id(char *ai_pet_id, size_t capacity);
esp_err_t pet_network_settings_changed(const pet_config_t *config, const char *ai_pet_id);
esp_err_t pet_network_synced_settings_result(
    const pet_synced_settings_t *settings, bool applied);
esp_err_t pet_network_config_v2_result(const pet_synced_config_v2_t *settings,
                                       bool applied, const char *error_code,
                                       const char *field);
esp_err_t pet_network_wifi_admin_result(const char *operation_id,
                                        const char *ssid,
                                        const char *status,
                                        const char *error_code,
                                        uint8_t priority);
esp_err_t pet_network_credential_prepared(const char *operation_id,
                                          uint32_t version);
esp_err_t pet_network_credential_committed(const char *operation_id,
                                           uint32_t version);
void pet_network_set_battery_snapshot(const pet_battery_snapshot_t *snapshot);
esp_err_t pet_network_speech_profile_changed(pet_speech_profile_t mode);
esp_err_t pet_network_speech_profile_result(const pet_speech_preference_t *preference,bool applied);
esp_err_t pet_network_set_ai_preferences(pet_ai_mode_t mode, pet_realtime_model_t model,
                                         pet_realtime_voice_t voice,
                                         pet_cartesia_voice_gender_t cartesia_voice_gender);
esp_err_t pet_network_set_profile_preferences(const pet_face_profile_t *profile);
/* Stage profile fields without sending a second device.hello. This lets a
 * local face change publish its optimistic settings transaction first, so an
 * older gateway face cannot race the new selection back onto the device. */
esp_err_t pet_network_stage_profile_preferences(const pet_face_profile_t *profile);
void pet_network_announce_profile_preferences(void);
esp_err_t pet_network_set_face_context(const char *active_face_id,
                                       const char installed_face_ids[][PET_FACE_ID_MAX],
                                       size_t installed_face_count,
                                       const pet_face_profiles_t *profiles);
esp_err_t pet_network_scan(pet_network_scan_callback_t callback);
esp_err_t pet_network_input_start(uint32_t stream_id, const char *ai_pet_id);
/* A ready session whose gateway listed PET_STORY_CAPABILITY. pet_network_story
 * then asks the pet for a story from its world on `stream_id`: the gateway
 * answers as it answers a spoken turn (thinking, speech, idle), and
 * pet_network_cancel(stream_id) stops it. */
bool pet_network_can_story(void);
esp_err_t pet_network_story(uint32_t stream_id, const char *ai_pet_id);
esp_err_t pet_network_send_microphone(uint32_t stream_id, uint32_t sequence,
                                      const int16_t *pcm, size_t samples);
esp_err_t pet_network_input_end(uint32_t stream_id, uint32_t final_sequence, uint32_t duration_ms);
esp_err_t pet_network_cancel(uint32_t stream_id);
uint32_t pet_network_dropped_chunks(void);
esp_err_t pet_network_ota_status(const char *release_id, const char *status,
                                 unsigned progress, const char *error_code);
