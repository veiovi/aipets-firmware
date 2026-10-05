#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_animation_profile.h"
#include "pet_ai.h"
#include "pet_voice.h"
#include "pet_face_profile.h"
#include "pet_speech_mouth.h"

#define PET_SSID_MAX 33
#define PET_PASSWORD_MAX 65
#define PET_WIFI_PROFILE_MAX 8
#define PET_GATEWAY_MAX 128
#define PET_TOKEN_MAX 257
#define PET_AI_PET_ID_MAX 81
#define PET_CONFIG_FINGERPRINT_MAX 65
#define PET_OPERATION_ID_MAX 37
#define PET_RECORDING_TIMEOUT_DEFAULT_SECONDS 30
#define PET_RECORDING_TIMEOUT_COUNT 6
#define PET_BRIGHTNESS_DEFAULT 100
#define PET_BRIGHTNESS_MIN 10
#define PET_SHAKE_SENSITIVITY_DEFAULT 75

typedef struct {
    char ssid[PET_SSID_MAX];
    uint8_t priority;
} pet_wifi_network_metadata_t;

typedef enum {
    PET_SPEECH_PROFILE_CARTESIA_BATCH = 0,
    PET_SPEECH_PROFILE_CARTESIA_REALTIME,
    PET_SPEECH_PROFILE_FISH_DIRECT,
    PET_SPEECH_PROFILE_COUNT,
} pet_speech_profile_t;

static inline const char *pet_speech_profile_name(pet_speech_profile_t profile)
{
    return profile == PET_SPEECH_PROFILE_FISH_DIRECT ? "fish-direct" :
           profile == PET_SPEECH_PROFILE_CARTESIA_REALTIME ? "cartesia-realtime" :
           "cartesia-batch";
}

typedef struct {
    char wifi_ssid[PET_SSID_MAX];
    char wifi_password[PET_PASSWORD_MAX];
    char wifi_fallback_ssid[PET_SSID_MAX];
    char wifi_fallback_password[PET_PASSWORD_MAX];
    char gateway_host[PET_GATEWAY_MAX];
    char pairing_token[PET_TOKEN_MAX];
    uint32_t credential_version;
    char pending_credential_token[PET_TOKEN_MAX];
    uint32_t pending_credential_version;
    char pending_credential_operation_id[PET_OPERATION_ID_MAX];
    uint32_t settings_version;
    uint8_t config_schema_version;
    char config_fingerprint[PET_CONFIG_FINGERPRINT_MAX];
    char pending_wifi_operation_id[PET_OPERATION_ID_MAX];
    char pending_wifi_ssid[PET_SSID_MAX];
    uint32_t speech_profile_version;
    uint16_t gateway_port;
    bool gateway_secure;
    uint8_t volume;
    uint8_t brightness;
    uint8_t shake_sensitivity;
    uint16_t recording_timeout_seconds;
    pet_animation_profile_t animation_profile;
    char face_id[PET_FACE_ID_MAX];
    char ai_pet_id[PET_AI_PET_ID_MAX];
    bool has_legacy_face_selection;
    pet_voice_t voice;
    pet_ai_mode_t ai_mode;
    pet_realtime_model_t realtime_model;
    pet_realtime_voice_t realtime_voice;
    pet_realtime_boost_t realtime_boost;
    pet_cartesia_voice_gender_t cartesia_voice_gender;
    pet_face_profiles_t face_profiles;
    bool has_legacy_ai_profile;
    pet_speech_mouth_mode_t speech_mouth_mode;
    pet_speech_profile_t speech_profile;
    /* Mouth timing from the cloud (config v2 speechMouthOffsetMs), kept so an
     * offline boot uses it. `has_` records that the last accepted desired
     * config carried the field: only then is it reported back. */
    bool has_speech_mouth_offset;
    int16_t speech_mouth_offset_ms;
} pet_config_t;

esp_err_t pet_config_init(void);
esp_err_t pet_config_load(pet_config_t *config);
bool pet_config_ready(const pet_config_t *config);
bool pet_config_ai_pet_id_valid(const char *id);
esp_err_t pet_config_store_wifi(const char *ssid, const char *password);
/* Explicit credentials: an empty password selects an open network, never recall. */
esp_err_t pet_config_store_wifi_explicit(const char *ssid, const char *password);
esp_err_t pet_config_store_wifi_fallback(const char *ssid, const char *password);
esp_err_t pet_config_restore_wifi_fallback(void);
esp_err_t pet_config_forget_wifi(const char *ssid);
esp_err_t pet_config_set_wifi_priority(const char *ssid, uint8_t priority);
esp_err_t pet_config_list_wifi_networks(pet_wifi_network_metadata_t *networks,
                                        size_t capacity, size_t *count);
esp_err_t pet_config_store_wifi_operation(const char *operation_id,
                                          const char *ssid);
esp_err_t pet_config_clear_wifi_operation(void);
esp_err_t pet_config_stage_credential(const char *operation_id,
                                      uint32_t version,
                                      const char *credential);
esp_err_t pet_config_commit_credential(const char *operation_id,
                                       uint32_t version);
bool pet_config_wifi_saved(const char *ssid);
esp_err_t pet_config_recall_wifi(const char *ssid, char *password,
                                 size_t capacity);
esp_err_t pet_config_store_volume(uint8_t volume);
esp_err_t pet_config_store_brightness(uint8_t brightness);
esp_err_t pet_config_store_shake_sensitivity(uint8_t sensitivity);
esp_err_t pet_config_store_face_profiles(const pet_face_profiles_t *profiles);
bool pet_config_recording_timeout_valid(uint16_t seconds);
uint16_t pet_config_recording_timeout_for_index(uint8_t index);
uint8_t pet_config_recording_timeout_index(uint16_t seconds);
esp_err_t pet_config_store_recording_timeout(uint16_t seconds);
esp_err_t pet_config_store_animation_profile(pet_animation_profile_t profile);
esp_err_t pet_config_store_face_id(const char *face_id);
esp_err_t pet_config_store_voice(pet_voice_t voice);
esp_err_t pet_config_store_synced_settings(uint32_t version, uint8_t volume,
                                           uint16_t recording_timeout_seconds,
                                           const char *face_id,
                                           const char *ai_pet_id,
                                           pet_voice_t voice);
esp_err_t pet_config_store_synced_settings_v2(uint32_t version,
                                              const char *fingerprint,
                                              uint8_t volume,
                                              uint8_t brightness,
                                              uint8_t shake_sensitivity,
                                              uint16_t recording_timeout_seconds,
                                              pet_animation_profile_t animation_profile,
                                              const char *face_id,
                                              const char *ai_pet_id,
                                              pet_speech_profile_t speech_profile);
esp_err_t pet_config_store_ai_mode(pet_ai_mode_t mode);
esp_err_t pet_config_store_realtime_model(pet_realtime_model_t model);
esp_err_t pet_config_store_realtime_voice(pet_realtime_voice_t voice);
esp_err_t pet_config_store_realtime_boost(pet_realtime_boost_t boost);
esp_err_t pet_config_store_cartesia_voice_gender(pet_cartesia_voice_gender_t gender);
esp_err_t pet_config_store_speech_mouth_mode(pet_speech_mouth_mode_t mode);
/* `present` false forgets it: the cloud did not send the field. */
esp_err_t pet_config_store_speech_mouth_offset(bool present, int16_t offset_ms);
esp_err_t pet_config_store_speech_profile(uint32_t version, pet_speech_profile_t mode);
void pet_config_start_console(void (*changed_callback)(void));
