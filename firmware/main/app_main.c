#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "face_core.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pet_audio.h"
#include "pet_battery.h"
#include "pet_config.h"
#include "pet_diagnostics.h"
#include "pet_face.h"
#include "pet_face_pack.h"
#include "pet_face_profile.h"
#include "pet_motion.h"
#include "pet_network.h"
#include "pet_ota.h"
#include "pet_onboarding.h"
#include "pet_vnext.h"
#include "pet_sfx.h"
#include "pet_tap_policy.h"

typedef enum {
    APP_EVENT_TAP,
    APP_EVENT_CONNECTED,
    APP_EVENT_DISCONNECTED,
    APP_EVENT_CAPTURE_DONE,
    APP_EVENT_CAPTURE_ERROR,
    APP_EVENT_PLAYBACK_DONE,
    APP_EVENT_GATEWAY_ERROR,
    APP_EVENT_SETTINGS_OPENED,
    APP_EVENT_SETTINGS_CLOSED,
    APP_EVENT_WIFI_SCAN,
    APP_EVENT_WIFI_JOIN,
    APP_EVENT_VOLUME_CHANGED,
    APP_EVENT_BRIGHTNESS_CHANGED,
    APP_EVENT_SHAKE_SENSITIVITY_CHANGED,
    APP_EVENT_FACE_ID_CHANGED,
    APP_EVENT_RECORDING_TIMEOUT_CHANGED,
    APP_EVENT_ANIMATION_PROFILE_CHANGED,
    APP_EVENT_VOICE_CHANGED,
    APP_EVENT_AI_MODE_CHANGED,
    APP_EVENT_SPEECH_PROFILE_CHANGED,
    APP_EVENT_CARTESIA_VOICE_GENDER_CHANGED,
    APP_EVENT_REALTIME_MODEL_CHANGED,
    APP_EVENT_REALTIME_VOICE_CHANGED,
    APP_EVENT_GESTURE,
    APP_EVENT_MOTION_GESTURE,
    APP_EVENT_REALTIME_BOOST_CHANGED,
    APP_EVENT_SPEECH_MOUTH_CHANGED,
    APP_EVENT_HEALTH_RECOVERY,
    APP_EVENT_SESSION_RESET,
    APP_EVENT_FACE_PROFILE_RESTORED,
    APP_EVENT_SYNCED_SETTINGS,
    APP_EVENT_SYNCED_CONFIG_V2,
    APP_EVENT_SYNCED_SPEECH_PROFILE,
    APP_EVENT_WIFI_ADMIN,
    APP_EVENT_CREDENTIAL_PREPARE,
    APP_EVENT_CREDENTIAL_COMMIT,
    APP_EVENT_LISTEN_CUE_DONE,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    uint32_t stream_id;
    uint32_t final_sequence;
    uint32_t duration_ms;
    esp_err_t result;
    uint8_t volume;
    uint8_t brightness;
    uint8_t shake_sensitivity;
    char face_id[PET_FACE_ID_MAX];
    pet_face_change_source_t face_change_source;
    uint16_t recording_timeout_seconds;
    pet_animation_profile_t animation_profile;
    pet_voice_t voice;
    pet_ai_mode_t ai_mode;
    pet_speech_profile_t speech_profile;
    pet_cartesia_voice_gender_t cartesia_voice_gender;
    pet_realtime_model_t realtime_model;
    pet_realtime_voice_t realtime_voice;
    uint8_t gesture;
    pet_motion_trigger_t motion_trigger;
    pet_realtime_boost_t realtime_boost;
    pet_speech_mouth_mode_t speech_mouth_mode;
    pet_diagnostics_recovery_t recovery_reason;
    bool recoverable;
    bool no_voice_detected;
    bool settings_close_user_initiated;
    char ssid[PET_SSID_MAX];
    char password[PET_PASSWORD_MAX];
    pet_face_profile_t face_profile;
    pet_synced_settings_t synced_settings;
    pet_synced_config_v2_t synced_config_v2;
    pet_speech_preference_t speech_preference;
    pet_wifi_admin_command_t wifi_admin;
    pet_credential_prepare_t credential_prepare;
    pet_credential_commit_t credential_commit;
} app_event_t;

static const char *TAG = "aipet";
static QueueHandle_t s_app_events;
static uint32_t s_active_input_stream;
static uint32_t s_output_stream;
static uint32_t s_output_sequence;
static volatile bool s_cancel_capture;
static volatile bool s_gateway_error_pending;
static pet_ai_mode_t s_ai_mode;
static pet_cartesia_voice_gender_t s_cartesia_voice_gender;
static pet_realtime_model_t s_realtime_model;
static pet_realtime_voice_t s_realtime_voice;
static pet_realtime_boost_t s_realtime_boost;
static pet_speech_mouth_mode_t s_speech_mouth_mode;
static pet_face_profiles_t s_face_profiles;
static pet_face_profile_t s_active_face_profile;
static pet_face_gender_t s_active_face_gender;
static bool s_active_face_profile_stored;
static uint8_t s_face_swipe_sfx_index;
/* Nonzero while the listening cue plays; the microphone opens after it. */
static uint32_t s_listen_token;

#define WAKE_SOUND_DELAY_MS 400

static const char *app_event_name(app_event_type_t type)
{
    static const char *const names[] = {
        [APP_EVENT_TAP] = "tap",
        [APP_EVENT_CONNECTED] = "connected",
        [APP_EVENT_DISCONNECTED] = "disconnected",
        [APP_EVENT_CAPTURE_DONE] = "capture-done",
        [APP_EVENT_CAPTURE_ERROR] = "capture-error",
        [APP_EVENT_PLAYBACK_DONE] = "playback-done",
        [APP_EVENT_GATEWAY_ERROR] = "gateway-error",
        [APP_EVENT_SETTINGS_OPENED] = "settings-opened",
        [APP_EVENT_SETTINGS_CLOSED] = "settings-closed",
        [APP_EVENT_WIFI_SCAN] = "wifi-scan",
        [APP_EVENT_WIFI_JOIN] = "wifi-join",
        [APP_EVENT_VOLUME_CHANGED] = "volume-changed",
        [APP_EVENT_BRIGHTNESS_CHANGED] = "brightness-changed",
        [APP_EVENT_SHAKE_SENSITIVITY_CHANGED] = "shake-sensitivity-changed",
        [APP_EVENT_FACE_ID_CHANGED] = "face-changed",
        [APP_EVENT_RECORDING_TIMEOUT_CHANGED] = "recording-timeout-changed",
        [APP_EVENT_ANIMATION_PROFILE_CHANGED] = "animation-profile-changed",
        [APP_EVENT_VOICE_CHANGED] = "voice-changed",
        [APP_EVENT_AI_MODE_CHANGED] = "ai-mode-changed",
        [APP_EVENT_SPEECH_PROFILE_CHANGED] = "speech-profile-changed",
        [APP_EVENT_CARTESIA_VOICE_GENDER_CHANGED] = "cartesia-voice-gender-changed",
        [APP_EVENT_REALTIME_MODEL_CHANGED] = "realtime-model-changed",
        [APP_EVENT_REALTIME_VOICE_CHANGED] = "realtime-voice-changed",
        [APP_EVENT_GESTURE] = "gesture",
        [APP_EVENT_MOTION_GESTURE] = "motion-gesture",
        [APP_EVENT_REALTIME_BOOST_CHANGED] = "realtime-boost-changed",
        [APP_EVENT_SPEECH_MOUTH_CHANGED] = "speech-mouth-changed",
        [APP_EVENT_HEALTH_RECOVERY] = "health-recovery",
        [APP_EVENT_SESSION_RESET] = "session-reset",
        [APP_EVENT_FACE_PROFILE_RESTORED] = "face-profile-restored",
        [APP_EVENT_SYNCED_SETTINGS] = "synced-settings",
        [APP_EVENT_SYNCED_CONFIG_V2] = "synced-config-v2",
        [APP_EVENT_SYNCED_SPEECH_PROFILE] = "synced-speech-profile",
        [APP_EVENT_WIFI_ADMIN] = "wifi-admin",
        [APP_EVENT_CREDENTIAL_PREPARE] = "credential-prepare",
        [APP_EVENT_CREDENTIAL_COMMIT] = "credential-commit",
        [APP_EVENT_LISTEN_CUE_DONE] = "listen-cue-done",
    };
    unsigned index = (unsigned)type;
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}

static bool post_event(app_event_t event)
{
    if (s_app_events && xQueueSend(s_app_events, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "app event queue full; dropped event %d", event.type);
        pet_diagnostics_note_dropped_app_event();
        return false;
    }
    return s_app_events != NULL;
}

static esp_err_t face_identity(const char *requested_id, char *face_id,
                               size_t capacity, pet_face_gender_t *gender_out)
{
    if (!requested_id || !face_id || !capacity || !gender_out ||
        !pet_face_id_valid(requested_id)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(pet_face_pack_identity(requested_id, gender_out), TAG,
                        "open full-frame face identity");
    strlcpy(face_id, requested_id, capacity);
    return ESP_OK;
}

static esp_err_t resolve_boot_face(const char *preferred_id, char *face_id,
                                   size_t capacity,
                                   pet_face_gender_t *gender_out)
{
    if (!face_id || !capacity || !gender_out) return ESP_ERR_INVALID_ARG;
    /* Full-frame packs are embedded in the application. The retired assets
     * partition remains untouched for recovery provenance. Persisted stable
     * IDs still select the same Pablo/Angel/Luna names. */
    const char *requested = preferred_id && pet_face_id_valid(preferred_id) ?
        preferred_id : "pablo";
    if (face_identity(requested, face_id, capacity, gender_out) == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "saved face %s unavailable in full-frame release; using Pablo",
             requested);
    return face_identity("pablo", face_id, capacity, gender_out);
}

static void apply_profile_to_config(pet_config_t *config,
                                    const pet_face_profile_t *profile)
{
    if (!config || !profile) return;
    config->voice = profile->voice;
    config->ai_mode = profile->ai_mode;
    config->realtime_model = profile->realtime_model;
    config->realtime_voice = profile->realtime_voice;
    config->cartesia_voice_gender = profile->cartesia_voice_gender;
}

static void refresh_network_face_context(void)
{
    char ids[PET_FACE_CATALOG_CAPACITY][PET_FACE_ID_MAX] = {0};
    pet_face_catalog_item_t items[PET_FACE_CATALOG_CAPACITY];
    size_t count = 0;
    if (pet_face_pack_list(items, PET_FACE_CATALOG_CAPACITY, &count) == ESP_OK) {
        for (size_t index = 0;
             index < count && index < PET_FACE_CATALOG_CAPACITY; ++index) {
            strlcpy(ids[index], items[index].id, sizeof(ids[index]));
        }
    }
    esp_err_t err = pet_network_set_face_context(s_active_face_profile.face_id,
                                                  ids, count,
                                                  &s_face_profiles);
    if (err != ESP_OK) ESP_LOGW(TAG, "could not update face sync context: %s",
                                esp_err_to_name(err));
}

static esp_err_t persist_active_profile(void)
{
    pet_face_profiles_t next = s_face_profiles;
    if (!pet_face_profiles_put(&next, &s_active_face_profile)) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pet_config_store_face_profiles(&next);
    if (err == ESP_OK) {
        s_face_profiles = next;
        const pet_face_profile_t *stored = pet_face_profiles_find_const(
            &s_face_profiles, s_active_face_profile.face_id);
        if (stored) s_active_face_profile = *stored;
        s_active_face_profile_stored = true;
    }
    return err;
}

static void mirror_active_profile_for_rollback(void)
{
    pet_config_store_voice(s_active_face_profile.voice);
    pet_config_store_ai_mode(s_active_face_profile.ai_mode);
    pet_config_store_realtime_model(s_active_face_profile.realtime_model);
    pet_config_store_realtime_voice(s_active_face_profile.realtime_voice);
    pet_config_store_cartesia_voice_gender(
        s_active_face_profile.cartesia_voice_gender);
}

static esp_err_t apply_active_profile_internal(bool announce)
{
    s_ai_mode = s_active_face_profile.ai_mode;
    s_realtime_model = s_active_face_profile.realtime_model;
    s_realtime_voice = s_active_face_profile.realtime_voice;
    s_cartesia_voice_gender = s_active_face_profile.cartesia_voice_gender;
    ESP_RETURN_ON_ERROR(pet_face_set_gender(s_active_face_gender), TAG,
                        "apply face gender");
    ESP_RETURN_ON_ERROR(pet_face_set_ai_mode(s_ai_mode), TAG,
                        "apply face AI mode");
    ESP_RETURN_ON_ERROR(pet_face_set_realtime_model(s_realtime_model), TAG,
                        "apply face realtime model");
    ESP_RETURN_ON_ERROR(pet_face_set_voice(s_active_face_profile.voice), TAG,
                        "apply face classic voice");
    ESP_RETURN_ON_ERROR(pet_face_set_realtime_voice(s_realtime_voice), TAG,
                        "apply face realtime voice");
    ESP_RETURN_ON_ERROR(pet_face_set_cartesia_voice_gender(
                            s_cartesia_voice_gender), TAG,
                        "apply face Cartesia voice");
    refresh_network_face_context();
    ESP_RETURN_ON_ERROR((announce ? pet_network_set_profile_preferences(
                                      &s_active_face_profile) :
                                  pet_network_stage_profile_preferences(
                                      &s_active_face_profile)), TAG,
                        "apply gateway face preferences");
    mirror_active_profile_for_rollback();
    return ESP_OK;
}

static esp_err_t apply_active_profile(void)
{
    return apply_active_profile_internal(true);
}

static esp_err_t commit_active_profile(
    const pet_face_profile_t *previous_profile)
{
    if (!previous_profile) return ESP_ERR_INVALID_ARG;
    pet_face_profiles_t previous_profiles = s_face_profiles;
    bool previous_stored = s_active_face_profile_stored;
    esp_err_t err = persist_active_profile();
    if (err == ESP_OK) err = apply_active_profile();
    if (err == ESP_OK) return ESP_OK;
    s_face_profiles = previous_profiles;
    s_active_face_profile = *previous_profile;
    s_active_face_profile_stored = previous_stored;
    esp_err_t rollback_err = pet_config_store_face_profiles(&s_face_profiles);
    if (rollback_err == ESP_OK) rollback_err = apply_active_profile();
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "face profile rollback failed: %s",
                 esp_err_to_name(rollback_err));
    }
    return err;
}

static esp_err_t select_active_profile(const char *requested_id)
{
    char face_id[PET_FACE_ID_MAX];
    pet_face_gender_t gender;
    ESP_RETURN_ON_ERROR(face_identity(requested_id, face_id, sizeof(face_id), &gender),
                        TAG, "resolve face identity");
    const pet_face_profile_t *stored = pet_face_profiles_find_const(
        &s_face_profiles, face_id);
    if (stored) {
        s_active_face_profile = *stored;
        s_active_face_profile_stored = true;
    } else {
        pet_face_profile_defaults(&s_active_face_profile, face_id, gender);
        s_active_face_profile_stored = false;
    }
    s_active_face_gender = gender;
    pet_face_profile_constrain(&s_active_face_profile, gender);
    return ESP_OK;
}

static pet_face_gender_t gender_for_face_id(const char *face_id)
{
    pet_face_gender_t gender = PET_FACE_GENDER_NEUTRAL;
    if (pet_face_pack_identity(face_id, &gender) == ESP_OK) return gender;
    return PET_FACE_GENDER_NEUTRAL;
}

static void diagnostics_recovery_requested(pet_diagnostics_recovery_t reason)
{
    if (!post_event((app_event_t){
            .type = APP_EVENT_HEALTH_RECOVERY,
            .recovery_reason = reason,
        }) && reason == PET_DIAGNOSTICS_RECOVERY_DISPLAY_STALLED) {
        ESP_LOGE(TAG, "display recovery event could not be queued; restarting");
        esp_restart();
    }
}

static void boot_button_task(void *arg)
{
    (void)arg;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    bool armed = false;
    bool fired = false;
    uint32_t pressed_ms = 0;
    for (;;) {
        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (!armed) {
            if (!pressed) armed = true;
        } else if (pressed) {
            if (!fired) {
                pressed_ms += 50;
                if (pressed_ms >= 2000) {
                    fired = true;
                    ESP_LOGW(TAG, "BOOT button long press: resetting conversation session");
                    post_event((app_event_t){ .type = APP_EVENT_SESSION_RESET });
                }
            }
        } else {
            pressed_ms = 0;
            fired = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void wake_sound_task(void *arg)
{
    (void)arg;
    /* Let the codec, amplifier, display, and shared peripheral bus settle.
     * The custom wake cue has a gentle onset, so playing it after bring-up
     * avoids clipping its opening while keeping networking asynchronous. */
    vTaskDelay(pdMS_TO_TICKS(WAKE_SOUND_DELAY_MS));
    esp_err_t err = pet_sfx_play(PET_SFX_WAKE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "wake sound failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void face_tapped(void)
{
    post_event((app_event_t){ .type = APP_EVENT_TAP });
}

static void settings_opened(void)
{
    post_event((app_event_t){ .type = APP_EVENT_SETTINGS_OPENED });
}

static void settings_closed(bool user_initiated)
{
    post_event((app_event_t){
        .type = APP_EVENT_SETTINGS_CLOSED,
        .settings_close_user_initiated = user_initiated,
    });
}

static void wifi_scan_requested(void)
{
    post_event((app_event_t){ .type = APP_EVENT_WIFI_SCAN });
}

static void volume_changed(uint8_t volume)
{
    post_event((app_event_t){ .type = APP_EVENT_VOLUME_CHANGED, .volume = volume });
}

static void brightness_changed(uint8_t brightness)
{
    post_event((app_event_t){
        .type = APP_EVENT_BRIGHTNESS_CHANGED,
        .brightness = brightness,
    });
}

static void shake_sensitivity_changed(uint8_t sensitivity)
{
    post_event((app_event_t){
        .type = APP_EVENT_SHAKE_SENSITIVITY_CHANGED,
        .shake_sensitivity = sensitivity,
    });
}

static void battery_updated(const pet_battery_snapshot_t *snapshot)
{
    pet_face_set_battery(snapshot);
    pet_network_set_battery_snapshot(snapshot);
}

static bool motion_detection_enabled(void)
{
    bool enabled = pet_face_shake_detection_enabled();
    pet_face_state_t state = pet_face_get_state();
    static bool logged;
    static bool previous_enabled;
    static pet_face_state_t previous_state;
    if (!logged || enabled != previous_enabled || state != previous_state) {
        ESP_LOGI(TAG, "motion gate eligible=%s faceState=%u",
                 enabled ? "yes" : "no", (unsigned)state);
        logged = true;
        previous_enabled = enabled;
        previous_state = state;
    }
    return enabled;
}

static void face_changed(const char *face_id, pet_face_change_source_t source)
{
    if (!pet_face_id_valid(face_id)) return;
    app_event_t event = {
        .type = APP_EVENT_FACE_ID_CHANGED,
        .face_change_source = source,
    };
    strlcpy(event.face_id, face_id, sizeof(event.face_id));
    post_event(event);
}

static void recording_timeout_changed(uint16_t seconds)
{
    post_event((app_event_t){
        .type = APP_EVENT_RECORDING_TIMEOUT_CHANGED,
        .recording_timeout_seconds = seconds,
    });
}

static void animation_profile_changed(pet_animation_profile_t profile)
{
    post_event((app_event_t){
        .type = APP_EVENT_ANIMATION_PROFILE_CHANGED,
        .animation_profile = profile,
    });
}

static void voice_changed(pet_voice_t voice)
{
    post_event((app_event_t){ .type = APP_EVENT_VOICE_CHANGED, .voice = voice });
}

static bool synced_settings_received(const pet_synced_settings_t *settings)
{
    if (!settings) return false;
    return post_event((app_event_t){
        .type = APP_EVENT_SYNCED_SETTINGS,
        .synced_settings = *settings,
    });
}

static bool synced_config_v2_received(const pet_synced_config_v2_t *settings)
{
    if (!settings) return false;
    return post_event((app_event_t){
        .type = APP_EVENT_SYNCED_CONFIG_V2,
        .synced_config_v2 = *settings,
    });
}

static bool wifi_admin_received(const pet_wifi_admin_command_t *command)
{
    if (!command) return false;
    return post_event((app_event_t){
        .type = APP_EVENT_WIFI_ADMIN,
        .wifi_admin = *command,
    });
}

static bool credential_prepare_received(const pet_credential_prepare_t *command)
{
    return command&&post_event((app_event_t){
        .type=APP_EVENT_CREDENTIAL_PREPARE,.credential_prepare=*command});
}

static bool credential_commit_received(const pet_credential_commit_t *command)
{
    return command&&post_event((app_event_t){
        .type=APP_EVENT_CREDENTIAL_COMMIT,.credential_commit=*command});
}

static bool synced_speech_preference_received(const pet_speech_preference_t *preference)
{
    return preference&&post_event((app_event_t){.type=APP_EVENT_SYNCED_SPEECH_PROFILE,.speech_preference=*preference});
}

static void publish_local_settings(void)
{
    pet_config_t config;
    if (pet_config_load(&config) != ESP_OK) return;
    /* AI identity is synchronized independently from the visual face. A local
     * face swipe must never rewrite the agent route. */
    pet_network_settings_changed(&config, config.ai_pet_id);
}

static void ai_mode_changed(pet_ai_mode_t mode)
{
    post_event((app_event_t){ .type = APP_EVENT_AI_MODE_CHANGED, .ai_mode = mode });
}
static void speech_profile_changed(pet_speech_profile_t profile){post_event((app_event_t){.type=APP_EVENT_SPEECH_PROFILE_CHANGED,.speech_profile=profile});}

static void cartesia_voice_gender_changed(pet_cartesia_voice_gender_t gender)
{
    post_event((app_event_t){
        .type = APP_EVENT_CARTESIA_VOICE_GENDER_CHANGED,
        .cartesia_voice_gender = gender,
    });
}

static void realtime_model_changed(pet_realtime_model_t model)
{
    post_event((app_event_t){ .type = APP_EVENT_REALTIME_MODEL_CHANGED, .realtime_model = model });
}

static void realtime_voice_changed(pet_realtime_voice_t voice)
{
    post_event((app_event_t){ .type = APP_EVENT_REALTIME_VOICE_CHANGED, .realtime_voice = voice });
}

static void gesture_requested(uint8_t gesture)
{
    post_event((app_event_t){ .type = APP_EVENT_GESTURE, .gesture = gesture });
}

static void realtime_boost_changed(pet_realtime_boost_t boost)
{
    post_event((app_event_t){ .type = APP_EVENT_REALTIME_BOOST_CHANGED, .realtime_boost = boost });
}

static void speech_mouth_changed(pet_speech_mouth_mode_t mode)
{
    post_event((app_event_t){
        .type = APP_EVENT_SPEECH_MOUTH_CHANGED,
        .speech_mouth_mode = mode,
    });
}

static void motion_gesture_requested(uint8_t gesture,
                                     pet_motion_trigger_t trigger)
{
    post_event((app_event_t){
        .type = APP_EVENT_MOTION_GESTURE,
        .gesture = gesture,
        .motion_trigger = trigger,
    });
}

static void wifi_join_requested(const char *ssid, const char *password)
{
    app_event_t event = { .type = APP_EVENT_WIFI_JOIN };
    strlcpy(event.ssid, ssid, sizeof(event.ssid));
    strlcpy(event.password, password, sizeof(event.password));
    post_event(event);
}

static void wifi_scan_finished(const pet_wifi_network_t *networks, size_t count, esp_err_t result)
{
    pet_face_set_wifi_networks(networks, count, result);
}

static esp_err_t capture_chunk(uint32_t stream_id, uint32_t sequence, const int16_t *pcm, size_t samples)
{
    return pet_network_send_microphone(stream_id, sequence, pcm, samples);
}

static void capture_done(uint32_t stream_id, uint32_t final_sequence, uint32_t duration_ms, esp_err_t result)
{
    post_event((app_event_t){
        .type = result == ESP_OK ? APP_EVENT_CAPTURE_DONE : APP_EVENT_CAPTURE_ERROR,
        .stream_id = stream_id,
        .final_sequence = final_sequence,
        .duration_ms = duration_ms,
        .result = result,
    });
}

static void playback_done(uint32_t stream_id)
{
    post_event((app_event_t){ .type = APP_EVENT_PLAYBACK_DONE, .stream_id = stream_id });
}

static void gateway_connected(bool ready)
{
    if (ready) {
        esp_err_t ota_health = pet_ota_confirm_healthy();
        if (ota_health != ESP_OK) ESP_LOGE(TAG, "could not confirm OTA health: %s", esp_err_to_name(ota_health));
    }
    post_event((app_event_t){ .type = ready ? APP_EVENT_CONNECTED : APP_EVENT_DISCONNECTED });
}

static void ota_status(const char *release_id, const char *status,
                       unsigned progress, const char *error_code)
{
    pet_network_ota_status(release_id, status, progress, error_code);
}

static esp_err_t gateway_ota_update(const pet_ota_request_t *request)
{
    if (pet_audio_is_capturing() || pet_audio_is_playing() ||
        pet_face_get_state() != PET_FACE_IDLE) return ESP_ERR_INVALID_STATE;
    return pet_ota_start(request);
}

static void gateway_face_profile(const pet_face_profile_t *profile)
{
    if (!profile) return;
    post_event((app_event_t){
        .type = APP_EVENT_FACE_PROFILE_RESTORED,
        .face_profile = *profile,
    });
}

static void gateway_state(const char *state)
{
    /* A gateway can briefly report idle after input submission while the
     * response is still being prepared. Keep the local waiting animation
     * latched until audio actually starts, playback completes, or an explicit
     * error/disconnect path terminates the turn. */
    if (s_gateway_error_pending && strcmp(state, "error")) return;
    if (!strcmp(state, "idle") && !pet_audio_is_playing() && !pet_audio_is_capturing() &&
        pet_face_get_state() != PET_FACE_THINKING) pet_face_set_state(PET_FACE_IDLE);
    else if (!strcmp(state, "thinking")) pet_face_set_state(PET_FACE_THINKING);
    else if (!strcmp(state, "speaking")) pet_face_set_state(PET_FACE_SPEAKING);
    else if (!strcmp(state, "error")) pet_face_set_state(PET_FACE_ERROR);
}

static void gateway_expression(const char *expression)
{
    pet_expression_t value = PET_EXPRESSION_IDLE;
    (void)pet_expression_from_wire(expression, &value);
    pet_face_set_expression(value);
}

static void gateway_gesture(uint8_t gesture)
{
    /* Model-selected reply gestures are deliberately ignored. Playback now
     * returns directly to a calm idle face without an extra flourish. */
    ESP_LOGD(TAG, "reply gesture %u ignored", (unsigned)gesture);
}

static esp_err_t gateway_audio_start(uint32_t stream_id, uint32_t sample_rate)
{
    /* Do not bracket streamed speech with local receive/success cues. Each cue
     * closes and reopens the shared speaker codec, which can pop and also
     * blocks WebSocket audio delivery while its PCM is written. The listening
     * and submit cues already provide interaction feedback before the reply. */
    s_output_stream = stream_id;
    s_output_sequence = 0;
    pet_diagnostics_note_streams(s_active_input_stream, s_output_stream);
    pet_face_set_state(PET_FACE_SPEAKING);
    pet_realtime_boost_t boost = s_ai_mode == PET_AI_MODE_OPENAI_REALTIME ?
        s_realtime_boost : PET_REALTIME_BOOST_OFF;
    return pet_audio_playback_start(stream_id, sample_rate, boost, s_speech_mouth_mode);
}

static esp_err_t gateway_audio_chunk(uint32_t stream_id, uint32_t sequence, const uint8_t *pcm, size_t length)
{
    if (stream_id != s_output_stream || sequence != s_output_sequence) {
        ESP_LOGE(TAG, "speaker sequence mismatch expected=%lu got=%lu", (unsigned long)s_output_sequence, (unsigned long)sequence);
        pet_audio_playback_cancel();
        pet_face_set_state(PET_FACE_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    s_output_sequence++;
    return pet_audio_playback_enqueue(stream_id, sequence, pcm, length);
}

static void gateway_audio_end(uint32_t stream_id, uint32_t final_sequence)
{
    if (stream_id == s_output_stream && (!s_output_sequence || final_sequence == s_output_sequence - 1)) pet_audio_playback_finish(stream_id);
    else {
        pet_audio_playback_cancel();
        pet_face_set_state(PET_FACE_ERROR);
    }
}

static void gateway_error(const char *code, const char *message, bool recoverable)
{
    ESP_LOGW(TAG, "gateway error %s: %s", code, message);
    /* WebSocket callbacks must stay non-blocking. Audio teardown and the
     * visible recovery pose run on the serialized application task. */
    s_gateway_error_pending = true;
    if (!post_event((app_event_t){
            .type = APP_EVENT_GATEWAY_ERROR,
            .recoverable = recoverable,
            .no_voice_detected = !strcmp(code, "NO_SPEECH") ||
                !strcmp(code, "STT_EMPTY") ||
                !strcmp(code, "CARTESIA_STT_EMPTY"),
        })) {
        s_gateway_error_pending = false;
        pet_face_set_state(PET_FACE_ERROR);
    }
}

static void listen_cue_done(uint32_t token, pet_sfx_outcome_t outcome)
{
    post_event((app_event_t){
        .type = APP_EVENT_LISTEN_CUE_DONE,
        .stream_id = token,
        .result = outcome == PET_SFX_CANCELLED ? ESP_ERR_NOT_FINISHED : ESP_OK,
    });
}

/* The listening cue plays before the microphone opens. The application task
 * never waits for it: a second tap during the cue cancels it instead. */
static esp_err_t start_listening(void)
{
    if (!pet_network_is_ready()) return ESP_ERR_INVALID_STATE;
    pet_audio_playback_cancel();
    pet_face_set_expression(PET_EXPRESSION_CURIOUS);
    pet_face_set_state(PET_FACE_LISTENING);
    uint32_t token = (esp_random() & 0x7fffffffu) | 1u;
    ESP_RETURN_ON_ERROR(pet_sfx_play_listen(token), TAG, "queue listening cue");
    s_listen_token = token;
    return ESP_OK;
}

static void cancel_pending_listen(void)
{
    if (!s_listen_token) return;
    s_listen_token = 0;
    pet_sfx_cancel();
}

static esp_err_t open_microphone(void)
{
    if (!pet_network_is_ready()) return ESP_ERR_INVALID_STATE;
    s_active_input_stream = esp_random();
    pet_diagnostics_note_streams(s_active_input_stream, s_output_stream);
    char ai_pet_id[PET_AI_PET_ID_MAX] = "pablo";
    ESP_RETURN_ON_ERROR(pet_network_get_ai_pet_id(ai_pet_id,
                                                  sizeof(ai_pet_id)), TAG,
                        "read synchronized AI identity");
    ESP_RETURN_ON_ERROR(pet_network_input_start(s_active_input_stream,
                                                ai_pet_id), TAG,
                        "announce audio start");
    ESP_RETURN_ON_ERROR(pet_audio_capture_start(s_active_input_stream), TAG, "start microphone");
    return ESP_OK;
}

static bool conversation_is_active(void)
{
    pet_face_state_t state = pet_face_get_state();
    return pet_audio_is_capturing() || pet_audio_is_playing() ||
        state == PET_FACE_LISTENING || state == PET_FACE_THINKING ||
        state == PET_FACE_SPEAKING;
}

static void rollback_synced_settings(
    const pet_config_t *previous_config,
    const char *previous_face_id,
    const char *previous_ai_pet_id,
    uint16_t previous_audio_timeout,
    uint8_t previous_volume,
    const pet_face_profiles_t *previous_profiles,
    const pet_face_profile_t *previous_profile,
    pet_face_gender_t previous_gender,
    bool previous_profile_stored)
{
    if (!previous_config || !previous_face_id || !previous_ai_pet_id ||
        !previous_profiles || !previous_profile) return;
    esp_err_t rollback_err = pet_face_set_id(previous_face_id);
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "synced face rollback failed: %s",
                 esp_err_to_name(rollback_err));
    }
    pet_audio_set_capture_timeout(previous_audio_timeout);
    pet_face_set_recording_timeout(previous_config->recording_timeout_seconds);
    s_face_profiles = *previous_profiles;
    s_active_face_profile = *previous_profile;
    s_active_face_gender = previous_gender;
    s_active_face_profile_stored = previous_profile_stored;
    rollback_err = pet_config_store_face_profiles(&s_face_profiles);
    if (rollback_err == ESP_OK) rollback_err = apply_active_profile();
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "synced profile rollback failed: %s",
                 esp_err_to_name(rollback_err));
    }
    rollback_err = pet_config_store_synced_settings(
        previous_config->settings_version, previous_config->volume,
        previous_config->recording_timeout_seconds,
        previous_config->face_id, previous_ai_pet_id,
        previous_config->voice);
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "synced NVS rollback failed: %s",
                 esp_err_to_name(rollback_err));
    }
    pet_audio_set_volume(previous_volume);
    pet_network_set_ai_pet_id(previous_ai_pet_id);
    refresh_network_face_context();
}

static esp_err_t apply_synced_settings_transaction(
    const pet_synced_settings_t *synced)
{
    if (!synced) return ESP_ERR_INVALID_ARG;
    pet_config_t previous_config;
    ESP_RETURN_ON_ERROR(pet_config_load(&previous_config), TAG,
                        "snapshot settings before sync");
    char previous_face_id[PET_FACE_ID_MAX] = {0};
    ESP_RETURN_ON_ERROR(pet_face_get_id(previous_face_id,
                                        sizeof(previous_face_id)), TAG,
                        "snapshot active face before sync");
    char previous_ai_pet_id[81] = {0};
    ESP_RETURN_ON_ERROR(pet_network_get_ai_pet_id(
                            previous_ai_pet_id,
                            sizeof(previous_ai_pet_id)), TAG,
                        "snapshot AI pet id before sync");
    const uint16_t previous_audio_timeout = pet_audio_get_capture_timeout();
    const uint8_t previous_volume = pet_audio_get_volume();
    const pet_face_profiles_t previous_profiles = s_face_profiles;
    const pet_face_profile_t previous_profile = s_active_face_profile;
    const pet_face_gender_t previous_gender = s_active_face_gender;
    const bool previous_profile_stored = s_active_face_profile_stored;

    esp_err_t err = select_active_profile(synced->face_id);
    if (err == ESP_OK) {
        s_active_face_profile.voice = synced->legacy_voice;
        pet_face_profile_constrain(&s_active_face_profile,
                                   s_active_face_gender);
        err = pet_face_set_id(synced->face_id);
    }
    if (err == ESP_OK) {
        err = pet_audio_set_capture_timeout(
            synced->recording_timeout_seconds);
    }
    if (err == ESP_OK) {
        err = pet_face_set_recording_timeout(
            synced->recording_timeout_seconds);
    }
    if (err == ESP_OK) err = apply_active_profile();
    if (err == ESP_OK) err = persist_active_profile();
    if (err == ESP_OK) refresh_network_face_context();
    /* This is the final fallible operation. Once the NVS transaction is
     * committed, only infallible runtime mirrors remain before ack. */
    if (err == ESP_OK) {
        err = pet_config_store_synced_settings(
            synced->version, synced->volume,
            synced->recording_timeout_seconds, synced->face_id,
            synced->ai_pet_id, synced->legacy_voice);
    }
    if (err == ESP_OK) {
        pet_audio_set_volume(synced->volume);
        return ESP_OK;
    }

    rollback_synced_settings(
        &previous_config, previous_face_id, previous_ai_pet_id,
        previous_audio_timeout, previous_volume, &previous_profiles,
        &previous_profile, previous_gender, previous_profile_stored);
    return err;
}

static esp_err_t apply_synced_config_v2_transaction(
    const pet_synced_config_v2_t *synced)
{
    if (!synced) return ESP_ERR_INVALID_ARG;
    pet_config_t previous;
    ESP_RETURN_ON_ERROR(pet_config_load(&previous), TAG,
                        "snapshot config-v2 settings");
    pet_synced_settings_t legacy = {
        .version = synced->version,
        .volume = synced->volume,
        .recording_timeout_seconds = synced->recording_timeout_seconds,
        .legacy_voice = previous.voice,
    };
    strlcpy(legacy.face_id,synced->face_id,sizeof(legacy.face_id));
    strlcpy(legacy.ai_pet_id,synced->ai_pet_id,sizeof(legacy.ai_pet_id));
    esp_err_t err = apply_synced_settings_transaction(&legacy);
    if (err == ESP_OK) err = pet_face_set_brightness(synced->brightness);
    if (err == ESP_OK) err = pet_face_set_shake_sensitivity(
        synced->shake_sensitivity);
    if (err == ESP_OK) {
        pet_motion_set_sensitivity(synced->shake_sensitivity);
        err = pet_face_set_animation_profile(synced->animation_profile);
    }
    if (err == ESP_OK) err = pet_face_set_speech_profile(
        synced->speech_profile,true,true,false);
    if (err == ESP_OK) err = pet_config_store_synced_settings_v2(
        synced->version,synced->fingerprint,synced->volume,
        synced->brightness,synced->shake_sensitivity,
        synced->recording_timeout_seconds,synced->animation_profile,
        synced->face_id,synced->ai_pet_id,synced->speech_profile);
    if (err == ESP_OK) err = pet_config_store_speech_mouth_offset(
        synced->has_speech_mouth_offset, synced->speech_mouth_offset_ms);
    if (err == ESP_OK) {
        pet_audio_set_speech_mouth_offset(synced->speech_mouth_offset_ms);
        return ESP_OK;
    }
    pet_synced_settings_t rollback = {
        .version = previous.settings_version,
        .volume = previous.volume,
        .recording_timeout_seconds = previous.recording_timeout_seconds,
        .legacy_voice = previous.voice,
    };
    strlcpy(rollback.face_id, previous.face_id, sizeof(rollback.face_id));
    strlcpy(rollback.ai_pet_id, previous.ai_pet_id, sizeof(rollback.ai_pet_id));
    esp_err_t rollback_err = apply_synced_settings_transaction(&rollback);
    if (rollback_err != ESP_OK)
        ESP_LOGE(TAG, "config-v2 legacy rollback failed: %s",
                 esp_err_to_name(rollback_err));
    pet_face_set_brightness(previous.brightness);
    pet_face_set_shake_sensitivity(previous.shake_sensitivity);
    pet_motion_set_sensitivity(previous.shake_sensitivity);
    pet_face_set_animation_profile(previous.animation_profile);
    pet_face_set_speech_profile(previous.speech_profile,true,true,false);
    if (strlen(previous.config_fingerprint)==64)
        pet_config_store_synced_settings_v2(previous.settings_version,
            previous.config_fingerprint,previous.volume,previous.brightness,
            previous.shake_sensitivity,previous.recording_timeout_seconds,
            previous.animation_profile,previous.face_id,previous.ai_pet_id,
            previous.speech_profile);
    pet_config_store_speech_mouth_offset(previous.has_speech_mouth_offset,
                                         previous.speech_mouth_offset_ms);
    return err;
}

static void app_event_task(void *arg)
{
    (void)arg;
    app_event_t event;
    for (;;) {
        if (xQueueReceive(s_app_events, &event, portMAX_DELAY) != pdTRUE) continue;
        pet_diagnostics_note_app_event(event.type, app_event_name(event.type),
                                       uxQueueMessagesWaiting(s_app_events));
        switch (event.type) {
            case APP_EVENT_CONNECTED:
                s_gateway_error_pending = false;
                pet_face_set_expression(PET_EXPRESSION_IDLE);
                pet_face_set_state(PET_FACE_IDLE);
                pet_sfx_play(PET_SFX_CONNECT);
                break;
            case APP_EVENT_DISCONNECTED:
                cancel_pending_listen();
                s_gateway_error_pending = false;
                pet_audio_capture_stop();
                pet_audio_playback_cancel();
                pet_face_set_state(PET_FACE_OFFLINE);
                break;
            case APP_EVENT_TAP:
                if (pet_ota_busy()) {
                    ESP_LOGI(TAG, "ignoring tap during OTA update");
                    break;
                }
                ESP_LOGI(TAG, "face tap state=%d capturing=%d playing=%d cue=%d",
                         pet_face_get_state(), pet_audio_is_capturing(), pet_audio_is_playing(),
                         s_listen_token != 0);
                switch (pet_tap_action(
                    pet_audio_is_capturing(),
                    s_listen_token != 0,
                    pet_audio_is_playing(),
                    pet_face_get_state() == PET_FACE_THINKING,
                    pet_face_get_state() == PET_FACE_SPEAKING)) {
                    case PET_TAP_STOP_CAPTURE:
                        pet_audio_capture_stop();
                        pet_face_set_state(PET_FACE_THINKING);
                        break;
                    case PET_TAP_IGNORE_WAITING:
                        ESP_LOGI(TAG, "ignoring tap while submitted turn is thinking");
                        break;
                    case PET_TAP_STOP_ANSWER:
                        pet_network_cancel(s_active_input_stream);
                        pet_audio_playback_cancel();
                        pet_face_set_audio_level(0);
                        pet_face_set_expression(PET_EXPRESSION_IDLE);
                        pet_face_set_state(PET_FACE_IDLE);
                        ESP_LOGI(TAG, "answer interrupted by tap");
                        break;
                    case PET_TAP_CANCEL_LISTENING:
                        cancel_pending_listen();
                        pet_face_set_expression(PET_EXPRESSION_IDLE);
                        pet_face_set_state(PET_FACE_IDLE);
                        ESP_LOGI(TAG, "listening cancelled before the microphone opened");
                        break;
                    case PET_TAP_START_LISTENING:
                        if (start_listening() != ESP_OK) {
                            pet_sfx_play(PET_SFX_RETRY);
                            pet_face_set_state(PET_FACE_OFFLINE);
                        }
                        break;
                }
                break;
            case APP_EVENT_LISTEN_CUE_DONE:
                /* Stale when a tap, settings, errors or a disconnect ended
                 * this listening attempt first. */
                if (!s_listen_token || event.stream_id != s_listen_token) break;
                s_listen_token = 0;
                if (event.result != ESP_OK || pet_face_get_state() != PET_FACE_LISTENING) break;
                if (open_microphone() != ESP_OK) {
                    pet_sfx_play(PET_SFX_RETRY);
                    pet_face_set_state(PET_FACE_OFFLINE);
                }
                break;
            case APP_EVENT_CAPTURE_DONE:
                ESP_LOGI(TAG, "capture complete stream=%lu sequence=%lu duration=%lums",
                         (unsigned long)event.stream_id, (unsigned long)event.final_sequence,
                         (unsigned long)event.duration_ms);
                if (s_cancel_capture) {
                    s_cancel_capture = false;
                    pet_network_cancel(event.stream_id);
                    break;
                }
                pet_sfx_play(PET_SFX_SUBMIT);
                if (event.stream_id == s_active_input_stream && pet_network_input_end(event.stream_id, event.final_sequence, event.duration_ms) == ESP_OK) pet_face_set_state(PET_FACE_THINKING);
                else pet_face_set_state(PET_FACE_OFFLINE);
                break;
            case APP_EVENT_CAPTURE_ERROR:
                ESP_LOGW(TAG, "capture failed stream=%lu: %s", (unsigned long)event.stream_id,
                         esp_err_to_name(event.result));
                pet_network_cancel(event.stream_id);
                pet_sfx_play(PET_SFX_RETRY);
                pet_face_set_expression(PET_EXPRESSION_CONCERNED);
                pet_face_set_state(PET_FACE_IDLE);
                break;
            case APP_EVENT_PLAYBACK_DONE:
                if (event.stream_id == s_output_stream) {
                    pet_face_set_audio_level(0);
                    pet_face_set_state(PET_FACE_IDLE);
                    ESP_LOGI(TAG, "playback complete stream=%lu", (unsigned long)event.stream_id);
                }
                break;
            case APP_EVENT_GATEWAY_ERROR:
                cancel_pending_listen();
                pet_audio_capture_stop();
                pet_audio_playback_cancel();
                pet_face_set_expression(PET_EXPRESSION_CONCERNED);
                pet_face_set_state(PET_FACE_ERROR);
                pet_sfx_play(event.no_voice_detected ?
                    PET_SFX_NO_VOICE : PET_SFX_ERROR);
                if (event.recoverable) {
                    /* Keep a readable error reaction on screen even though the
                     * gateway immediately follows session.error with idle. */
                    vTaskDelay(pdMS_TO_TICKS(500));
                    pet_face_set_expression(PET_EXPRESSION_IDLE);
                    pet_face_set_state(PET_FACE_IDLE);
                }
                s_gateway_error_pending = false;
                break;
            case APP_EVENT_SETTINGS_OPENED:
                pet_battery_set_enabled(true);
                if (s_listen_token) {
                    cancel_pending_listen();
                    pet_face_set_expression(PET_EXPRESSION_IDLE);
                    pet_face_set_state(PET_FACE_IDLE);
                }
                if (pet_audio_is_capturing()) {
                    s_cancel_capture = true;
                    pet_audio_capture_stop();
                }
                if (pet_audio_is_playing() || pet_face_get_state() == PET_FACE_THINKING) {
                    pet_network_cancel(s_active_input_stream);
                    pet_audio_playback_cancel();
                }
                pet_sfx_play(PET_SFX_SETTINGS_OPEN);
                break;
            case APP_EVENT_SETTINGS_CLOSED:
                pet_battery_set_enabled(false);
                if (event.settings_close_user_initiated) {
                    pet_sfx_play(PET_SFX_SETTINGS_CLOSE);
                }
                break;
            case APP_EVENT_WIFI_SCAN: {
                esp_err_t err = pet_network_scan(wifi_scan_finished);
                if (err != ESP_OK) {
                    pet_face_set_wifi_status("scan could not start; tap scan to retry");
                }
                break;
            }
            case APP_EVENT_VOLUME_CHANGED:
                pet_audio_set_volume(event.volume);
                if (pet_config_store_volume(event.volume) == ESP_OK) {
                    pet_sfx_play(PET_SFX_GESTURE_POP);
                    publish_local_settings();
                }
                else pet_face_set_wifi_status("could not save volume");
                break;
            case APP_EVENT_BRIGHTNESS_CHANGED:
                if (pet_config_store_brightness(event.brightness) == ESP_OK &&
                    pet_face_set_brightness(event.brightness) == ESP_OK) {
                    pet_sfx_play(PET_SFX_GESTURE_POP);
                    publish_local_settings();
                } else {
                    pet_face_brightness_save_failed();
                }
                break;
            case APP_EVENT_SHAKE_SENSITIVITY_CHANGED:
                if (pet_config_store_shake_sensitivity(
                        event.shake_sensitivity) == ESP_OK &&
                    pet_face_set_shake_sensitivity(
                        event.shake_sensitivity) == ESP_OK) {
                    pet_motion_set_sensitivity(event.shake_sensitivity);
                    pet_sfx_play(PET_SFX_GESTURE_POP);
                    publish_local_settings();
                } else {
                    pet_face_shake_sensitivity_save_failed();
                }
                break;
            case APP_EVENT_FACE_ID_CHANGED: {
                pet_config_t controlled_config;
                if (pet_config_load(&controlled_config) == ESP_OK &&
                    strlen(controlled_config.config_fingerprint) == 64) {
                    select_active_profile(controlled_config.face_id);
                    pet_face_set_id(controlled_config.face_id);
                    apply_active_profile();
                    pet_face_set_wifi_status("Choose AI Pets in web admin");
                    break;
                }
                char previous[PET_FACE_ID_MAX] = {0};
                pet_face_get_id(previous, sizeof(previous));
                pet_face_profile_t previous_profile = s_active_face_profile;
                pet_face_gender_t previous_gender = s_active_face_gender;
                bool previous_stored = s_active_face_profile_stored;
                if (select_active_profile(event.face_id) != ESP_OK ||
                    pet_config_store_face_id(event.face_id) != ESP_OK) {
                    s_active_face_profile = previous_profile;
                    s_active_face_gender = previous_gender;
                    s_active_face_profile_stored = previous_stored;
                    pet_face_id_save_failed();
                    break;
                }
                if (conversation_is_active()) {
                    cancel_pending_listen();
                    s_cancel_capture = true;
                    pet_audio_capture_stop();
                    pet_audio_playback_cancel();
                    if (pet_network_is_ready()) {
                        pet_network_cancel(s_active_input_stream);
                    }
                }
                if (pet_face_set_id(event.face_id) == ESP_OK &&
                    /* A face's voice/AI profile can change the hello payload.
                     * Stage it silently, publish the face transaction first,
                     * then announce the complete identity. Otherwise the
                     * gateway can answer the premature hello with its older
                     * face and visibly bounce Luna back to Angel. */
                    apply_active_profile_internal(false) == ESP_OK) {
                    if (event.face_change_source == PET_FACE_CHANGE_SWIPE) {
                        pet_sfx_play(pet_sfx_for_face_swipe(s_face_swipe_sfx_index));
                        s_face_swipe_sfx_index = (uint8_t)((s_face_swipe_sfx_index + 1u) % 5u);
                    } else {
                        pet_sfx_play(PET_SFX_GESTURE_POP);
                    }
                    publish_local_settings();
                    pet_network_announce_profile_preferences();
                } else {
                    pet_config_store_face_id(previous);
                    pet_face_set_id(previous);
                    s_active_face_profile = previous_profile;
                    s_active_face_gender = previous_gender;
                    s_active_face_profile_stored = previous_stored;
                    apply_active_profile();
                    pet_face_id_save_failed();
                }
                break;
            }
            case APP_EVENT_RECORDING_TIMEOUT_CHANGED:
                if (pet_config_store_recording_timeout(event.recording_timeout_seconds) == ESP_OK &&
                    pet_audio_set_capture_timeout(event.recording_timeout_seconds) == ESP_OK &&
                    pet_face_set_recording_timeout(event.recording_timeout_seconds) == ESP_OK) {
                    pet_sfx_play(PET_SFX_GESTURE_POP);
                    publish_local_settings();
                } else {
                    pet_face_recording_timeout_save_failed();
                }
                break;
            case APP_EVENT_ANIMATION_PROFILE_CHANGED: {
                pet_animation_profile_t previous = pet_face_get_animation_profile();
                if (pet_config_store_animation_profile(event.animation_profile) == ESP_OK) {
                    if (pet_face_set_animation_profile(event.animation_profile) == ESP_OK) {
                        pet_sfx_play(PET_SFX_GESTURE_POP);
                        publish_local_settings();
                    } else {
                        pet_config_store_animation_profile(previous);
                        pet_face_animation_profile_save_failed();
                    }
                } else {
                    pet_face_animation_profile_save_failed();
                }
                break;
            }
            case APP_EVENT_VOICE_CHANGED:
            {
                pet_face_profile_t previous = s_active_face_profile;
                s_active_face_profile.voice = event.voice;
                pet_face_profile_constrain(&s_active_face_profile,
                                           s_active_face_gender);
                if (commit_active_profile(&previous) == ESP_OK) {
                    pet_sfx_play(PET_SFX_GESTURE_POP);
                    publish_local_settings();
                } else {
                    s_active_face_profile = previous;
                    pet_face_voice_save_failed();
                }
                break;
            }
            case APP_EVENT_AI_MODE_CHANGED:
            {
                pet_face_profile_t previous = s_active_face_profile;
                s_active_face_profile.ai_mode = event.ai_mode;
                if (commit_active_profile(&previous) == ESP_OK) pet_sfx_play(PET_SFX_GESTURE_POP);
                else {
                    s_active_face_profile = previous;
                    pet_face_ai_setting_save_failed();
                }
                break;
            }
            case APP_EVENT_SPEECH_PROFILE_CHANGED:
                if(pet_network_speech_profile_changed(event.speech_profile)!=ESP_OK)pet_face_speech_profile_save_failed();
                break;
            case APP_EVENT_CARTESIA_VOICE_GENDER_CHANGED:
            {
                pet_face_profile_t previous = s_active_face_profile;
                s_active_face_profile.cartesia_voice_gender =
                    event.cartesia_voice_gender;
                pet_face_profile_constrain(&s_active_face_profile,
                                           s_active_face_gender);
                if (commit_active_profile(&previous) == ESP_OK) pet_sfx_play(PET_SFX_GESTURE_POP);
                else {
                    s_active_face_profile = previous;
                    pet_face_ai_setting_save_failed();
                }
                break;
            }
            case APP_EVENT_REALTIME_MODEL_CHANGED:
            {
                pet_face_profile_t previous = s_active_face_profile;
                s_active_face_profile.realtime_model = event.realtime_model;
                if (commit_active_profile(&previous) == ESP_OK) pet_sfx_play(PET_SFX_GESTURE_POP);
                else {
                    s_active_face_profile = previous;
                    pet_face_ai_setting_save_failed();
                }
                break;
            }
            case APP_EVENT_REALTIME_VOICE_CHANGED:
            {
                pet_face_profile_t previous = s_active_face_profile;
                s_active_face_profile.realtime_voice = event.realtime_voice;
                pet_face_profile_constrain(&s_active_face_profile,
                                           s_active_face_gender);
                if (commit_active_profile(&previous) == ESP_OK) pet_sfx_play(PET_SFX_GESTURE_POP);
                else {
                    s_active_face_profile = previous;
                    pet_face_ai_setting_save_failed();
                }
                break;
            }
            case APP_EVENT_GESTURE: {
                uint8_t active = pet_face_trigger_gesture(event.gesture);
                if (active != FC_GESTURE_NONE) {
                    pet_sfx_play(PET_SFX_GESTURE_POP);
                }
                break;
            }
            case APP_EVENT_MOTION_GESTURE: {
                if (!pet_face_can_trigger_ambient_gesture()) break;
                uint8_t active = pet_face_trigger_gesture(event.gesture);
                /* Tilt is silent; a shake that reached the pet gets its cue. */
                if (active != FC_GESTURE_NONE &&
                    event.motion_trigger == PET_MOTION_TRIGGER_SHAKE) {
                    pet_sfx_play(PET_SFX_GESTURE_SHAKE);
                }
                break;
            }
            case APP_EVENT_REALTIME_BOOST_CHANGED:
                if (pet_config_store_realtime_boost(event.realtime_boost) == ESP_OK) {
                    s_realtime_boost = event.realtime_boost;
                    if (pet_face_set_realtime_boost(s_realtime_boost) == ESP_OK) {
                        pet_sfx_play(PET_SFX_GESTURE_POP);
                    } else pet_face_ai_setting_save_failed();
                } else pet_face_ai_setting_save_failed();
                break;
            case APP_EVENT_SPEECH_MOUTH_CHANGED:
                if (pet_config_store_speech_mouth_mode(event.speech_mouth_mode) == ESP_OK) {
                    s_speech_mouth_mode = event.speech_mouth_mode;
                    if (pet_face_set_speech_mouth_mode(s_speech_mouth_mode) == ESP_OK) {
                        pet_sfx_play(PET_SFX_GESTURE_POP);
                    } else {
                        pet_face_speech_mouth_save_failed();
                    }
                } else {
                    pet_face_speech_mouth_save_failed();
                }
                break;
            case APP_EVENT_HEALTH_RECOVERY:
                ESP_LOGE(TAG, "recovering unhealthy conversation state reason=%d",
                         event.recovery_reason);
                cancel_pending_listen();
                pet_audio_capture_stop();
                pet_audio_playback_cancel();
                if (pet_network_is_ready()) pet_network_cancel(s_active_input_stream);
                if (event.recovery_reason == PET_DIAGNOSTICS_RECOVERY_DISPLAY_STALLED) {
                    /* A stalled LVGL task cannot repaint a recovery screen. A
                     * software restart preserves configuration, records the
                     * RTC breadcrumb, and creates a fresh gateway session. */
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                }
                pet_face_set_expression(PET_EXPRESSION_CONCERNED);
                pet_face_set_state(pet_network_is_ready() ? PET_FACE_IDLE : PET_FACE_OFFLINE);
                break;
            case APP_EVENT_SESSION_RESET:
                ESP_LOGW(TAG, "SESSION_RESET source=boot-button bootId=%s",
                         pet_diagnostics_boot_id());
                pet_audio_capture_stop();
                pet_audio_playback_cancel();
                if (pet_network_is_ready()) pet_network_cancel(s_active_input_stream);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                break;
            case APP_EVENT_FACE_PROFILE_RESTORED: {
                if (pet_face_profiles_find_const(&s_face_profiles,
                                                 event.face_profile.face_id)) break;
                pet_face_profile_t restored = event.face_profile;
                pet_face_profile_constrain(&restored,
                    gender_for_face_id(restored.face_id));
                pet_face_profiles_t next = s_face_profiles;
                if (!pet_face_profiles_put(&next, &restored) ||
                    pet_config_store_face_profiles(&next) != ESP_OK) {
                    ESP_LOGW(TAG, "could not persist restored profile for %s",
                             restored.face_id);
                    break;
                }
                s_face_profiles = next;
                if (!strcmp(restored.face_id, s_active_face_profile.face_id) &&
                    !s_active_face_profile_stored) {
                    const pet_face_profile_t *saved = pet_face_profiles_find_const(
                        &s_face_profiles, restored.face_id);
                    if (saved) s_active_face_profile = *saved;
                    s_active_face_profile_stored = true;
                    if (apply_active_profile() != ESP_OK) {
                        ESP_LOGW(TAG, "could not apply restored active face profile");
                    }
                }
                break;
            }
            case APP_EVENT_SYNCED_SETTINGS: {
                const pet_synced_settings_t *synced = &event.synced_settings;
                esp_err_t err = apply_synced_settings_transaction(synced);
                if (err == ESP_OK) {
                    esp_err_t ack_err = pet_network_synced_settings_result(
                        synced, true);
                    if (ack_err == ESP_OK) {
                        ESP_LOGI(TAG, "applied backend AI pet settings v%lu",
                                 (unsigned long)synced->version);
                        /* This equal-version report is the device's durable
                         * acknowledgement. The gateway must not label a send
                         * as synchronized before it receives this refresh. */
                        publish_local_settings();
                    } else {
                        ESP_LOGE(TAG, "settings durable but ack failed: %s",
                                 esp_err_to_name(ack_err));
                        publish_local_settings();
                    }
                } else {
                    esp_err_t reject_err = pet_network_synced_settings_result(
                        synced, false);
                    ESP_LOGW(TAG, "rejected backend AI pet settings: %s", esp_err_to_name(err));
                    pet_face_set_wifi_status("could not apply synced settings");
                    /* The network layer refreshes the restored durable values
                     * and replays any newer coalesced payload. Fall back to a
                     * direct refresh only if that bookkeeping itself failed. */
                    if (reject_err != ESP_OK) publish_local_settings();
                }
                break;
            }
            case APP_EVENT_SYNCED_CONFIG_V2: {
                const pet_synced_config_v2_t *synced=&event.synced_config_v2;
                esp_err_t err=apply_synced_config_v2_transaction(synced);
                pet_network_config_v2_result(synced,err==ESP_OK,
                    err==ESP_OK?NULL:"APPLY_FAILED",NULL);
                if(err==ESP_OK)pet_sfx_play(PET_SFX_GESTURE_POP);
                else pet_face_set_wifi_status("could not apply cloud settings");
                break;
            }
            case APP_EVENT_SYNCED_SPEECH_PROFILE:{
                const pet_speech_preference_t *preference=&event.speech_preference;esp_err_t err=pet_config_store_speech_profile(preference->version,preference->profile);
                if(err==ESP_OK)err=pet_face_set_speech_profile(preference->profile,preference->allow_cartesia_batch,preference->allow_cartesia_realtime,preference->allow_fish);
                pet_network_speech_profile_result(preference,err==ESP_OK);if(err==ESP_OK)pet_sfx_play(PET_SFX_GESTURE_POP);else pet_face_set_wifi_status("could not save speech profile");break;
            }
            case APP_EVENT_WIFI_JOIN: {
                char remembered_password[PET_PASSWORD_MAX] = {0};
                const char *password = event.password;
                if (!password[0] &&
                    pet_config_recall_wifi(event.ssid, remembered_password,
                                           sizeof(remembered_password)) == ESP_OK) {
                    password = remembered_password;
                }
                esp_err_t err = pet_config_store_wifi(event.ssid, password);
                memset(remembered_password, 0, sizeof(remembered_password));
                memset(event.password, 0, sizeof(event.password));
                if (err == ESP_OK) {
                    pet_face_set_wifi_status("saved; restarting...");
                    vTaskDelay(pdMS_TO_TICKS(400));
                    esp_restart();
                } else {
                    pet_face_set_wifi_status("could not save Wi-Fi");
                }
                break;
            }
            case APP_EVENT_WIFI_ADMIN: {
                pet_wifi_admin_command_t *command=&event.wifi_admin;
                esp_err_t err=pet_config_store_wifi_operation(
                    command->operation_id,command->ssid);
                char recalled[PET_PASSWORD_MAX]={0};
                if(err==ESP_OK&&(command->action==PET_WIFI_ADMIN_ADD||
                    command->action==PET_WIFI_ADMIN_UPDATE)) {
                    err=pet_config_store_wifi(command->ssid,command->password);
                    if(err==ESP_OK)err=pet_config_set_wifi_priority(
                        command->ssid,command->priority);
                } else if(err==ESP_OK&&command->action==PET_WIFI_ADMIN_CONNECT){
                    err=pet_config_recall_wifi(command->ssid,recalled,sizeof(recalled));
                    if(err==ESP_OK)err=pet_config_store_wifi(command->ssid,recalled);
                } else if(err==ESP_OK&&command->action==PET_WIFI_ADMIN_PRIORITY){
                    err=pet_config_set_wifi_priority(command->ssid,
                                                     command->priority);
                } else if(err==ESP_OK&&command->action==PET_WIFI_ADMIN_FORGET){
                    err=pet_config_forget_wifi(command->ssid);
                    if(err==ESP_OK){pet_network_wifi_admin_result(command->operation_id,
                        command->ssid,"forgotten",NULL,command->priority);pet_config_clear_wifi_operation();}
                }
                memset(recalled,0,sizeof(recalled));
                memset(command->password,0,sizeof(command->password));
                if(err==ESP_OK){pet_face_set_wifi_status("network saved; restarting...");vTaskDelay(pdMS_TO_TICKS(400));esp_restart();}
                else{pet_config_clear_wifi_operation();pet_network_wifi_admin_result(command->operation_id,command->ssid,"failed",esp_err_to_name(err),command->priority);pet_face_set_wifi_status("Wi-Fi command failed");}
                break;
            }
            case APP_EVENT_CREDENTIAL_PREPARE: {
                pet_credential_prepare_t *command=&event.credential_prepare;
                esp_err_t err=pet_config_stage_credential(command->operation_id,
                    command->version,command->credential);
                memset(command->credential,0,sizeof(command->credential));
                if(err==ESP_OK)pet_network_credential_prepared(
                    command->operation_id,command->version);
                else ESP_LOGE(TAG,"could not stage credential: %s",
                              esp_err_to_name(err));
                break;
            }
            case APP_EVENT_CREDENTIAL_COMMIT: {
                pet_credential_commit_t *command=&event.credential_commit;
                esp_err_t err=pet_config_commit_credential(command->operation_id,
                                                            command->version);
                if(err==ESP_OK){
                    pet_network_credential_committed(command->operation_id,
                                                     command->version);
                    vTaskDelay(pdMS_TO_TICKS(250));
                    esp_restart();
                }else ESP_LOGE(TAG,"could not commit credential: %s",
                              esp_err_to_name(err));
                break;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Pet firmware %s booting", esp_app_get_description()->version);
    ESP_ERROR_CHECK(pet_diagnostics_init());
    ESP_ERROR_CHECK(pet_config_init());
#if CONFIG_PET_VNEXT_ENROLLMENT
    if(!pet_vnext_independent_control())ESP_ERROR_CHECK(pet_ota_init(ota_status));
#else
    ESP_ERROR_CHECK(pet_ota_init(ota_status));
#endif
    pet_config_t config;
    ESP_ERROR_CHECK(pet_config_load(&config));
    /* The cloud's last mouth timing, from NVS, until it sends another. */
    pet_audio_set_speech_mouth_offset(config.speech_mouth_offset_ms);
#if CONFIG_PET_VNEXT_ENROLLMENT
    /* The setup shell must not resolve, load or initialize a pet pack. */
    ESP_ERROR_CHECK(pet_vnext_start(&config, CONFIG_PET_VNEXT_CONTROL_ORIGIN));
    return;
#endif
    s_face_profiles = config.face_profiles;
    char initial_face_id[PET_FACE_ID_MAX];
    ESP_ERROR_CHECK(resolve_boot_face(config.face_id, initial_face_id,
                                  sizeof(initial_face_id),
                                  &s_active_face_gender));
    if (config.has_legacy_face_selection ||
        strcmp(config.face_id, initial_face_id)) {
        strlcpy(config.face_id, initial_face_id, sizeof(config.face_id));
        ESP_ERROR_CHECK(pet_config_store_face_id(config.face_id));
    }
    const pet_face_profile_t *saved_profile = pet_face_profiles_find_const(
        &s_face_profiles, initial_face_id);
    if (saved_profile) {
        s_active_face_profile = *saved_profile;
        s_active_face_profile_stored = true;
    } else {
        pet_face_profile_defaults(&s_active_face_profile, initial_face_id,
                                  s_active_face_gender);
        if (config.has_legacy_ai_profile) {
            s_active_face_profile.voice = config.voice;
            s_active_face_profile.ai_mode = config.ai_mode;
            s_active_face_profile.realtime_model = config.realtime_model;
            s_active_face_profile.realtime_voice = config.realtime_voice;
            s_active_face_profile.cartesia_voice_gender =
                config.cartesia_voice_gender;
            pet_face_profile_constrain(&s_active_face_profile,
                                       s_active_face_gender);
            if (persist_active_profile() != ESP_OK) {
                ESP_LOGW(TAG, "could not migrate legacy AI settings to face profile");
            }
        }
    }
    pet_face_profile_constrain(&s_active_face_profile, s_active_face_gender);
    apply_profile_to_config(&config, &s_active_face_profile);
    s_ai_mode = config.ai_mode;
    s_cartesia_voice_gender = config.cartesia_voice_gender;
    s_realtime_model = config.realtime_model;
    s_realtime_voice = config.realtime_voice;
    s_realtime_boost = config.realtime_boost;
    s_speech_mouth_mode = config.speech_mouth_mode;
    s_app_events = xQueueCreate(16, sizeof(app_event_t));
    assert(s_app_events);
    pet_face_callbacks_t face_callbacks = {
        .tapped = face_tapped,
        .settings_opened = settings_opened,
        .settings_closed = settings_closed,
        .wifi_scan_requested = wifi_scan_requested,
        .volume_changed = volume_changed,
        .brightness_changed = brightness_changed,
        .shake_sensitivity_changed = shake_sensitivity_changed,
        .face_changed = face_changed,
        .recording_timeout_changed = recording_timeout_changed,
        .animation_profile_changed = animation_profile_changed,
        .voice_changed = voice_changed,
        .ai_mode_changed = ai_mode_changed,
        .speech_profile_changed=speech_profile_changed,
        .cartesia_voice_gender_changed = cartesia_voice_gender_changed,
        .realtime_model_changed = realtime_model_changed,
        .realtime_voice_changed = realtime_voice_changed,
        .gesture_requested = gesture_requested,
        .realtime_boost_changed = realtime_boost_changed,
        .speech_mouth_changed = speech_mouth_changed,
        .wifi_join_requested = wifi_join_requested,
    };
    ESP_ERROR_CHECK(pet_face_init(&face_callbacks, config.volume,
                                  config.brightness, config.shake_sensitivity,
                                  config.wifi_ssid,
                                  config.face_id, config.voice,
                                  config.recording_timeout_seconds,
                                  config.animation_profile, config.ai_mode,
                                  config.realtime_model, config.speech_profile, config.realtime_voice,
                                  config.realtime_boost, config.cartesia_voice_gender,
                                  s_active_face_gender, config.speech_mouth_mode));
    ESP_ERROR_CHECK(pet_audio_init(capture_chunk, capture_done, playback_done));
    ESP_ERROR_CHECK(pet_sfx_init(listen_cue_done));
    ESP_ERROR_CHECK(pet_audio_set_capture_timeout(config.recording_timeout_seconds));
    pet_audio_set_volume(config.volume);
    /* Profile synchronization and face changes temporarily hold the persisted
     * profile set plus a queued event on this task's stack. Three installed
     * pack faces pushed the former 5 KiB allocation past its guard during the
     * initial gateway restore, so keep explicit headroom for that path. */
    xTaskCreate(app_event_task, "pet_app", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(pet_diagnostics_start(diagnostics_recovery_requested));
    /* Logging and event dispatch during the two-second recovery press use
     * substantially more stack than the idle GPIO polling loop. A captured
     * production coredump showed 1,936 bytes used with only 100 bytes left in
     * the former 2 KiB allocation before the guard was crossed. Keep enough
     * transient headroom so a recovery press cannot reset an active voice
     * turn or the LVGL task. */
    if (xTaskCreate(boot_button_task, "pet_boot_btn", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start BOOT button monitor");
    }
    esp_err_t battery_result = pet_battery_start(battery_updated);
    if (battery_result != ESP_OK) {
        ESP_LOGW(TAG, "battery meter unavailable: %s",
                 esp_err_to_name(battery_result));
    }
    esp_err_t motion_result = pet_motion_start(motion_gesture_requested,
                                               motion_detection_enabled,
                                               config.shake_sensitivity);
    if (motion_result != ESP_OK) {
        ESP_LOGW(TAG, "accelerometer gestures unavailable: %s", esp_err_to_name(motion_result));
    }
    pet_config_start_console(NULL);
    if (xTaskCreate(wake_sound_task, "pet_wake_sfx", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start delayed wake sound");
    }

    pet_network_callbacks_t callbacks = {
        .connected = gateway_connected,
        .state = gateway_state,
        .expression = gateway_expression,
        .gesture = gateway_gesture,
        .audio_start = gateway_audio_start,
        .audio_chunk = gateway_audio_chunk,
        .audio_end = gateway_audio_end,
        .session_error = gateway_error,
        .face_profile = gateway_face_profile,
        .settings_received = synced_settings_received,
        .config_v2_received = synced_config_v2_received,
        .wifi_admin_received = wifi_admin_received,
        .credential_prepare_received=credential_prepare_received,
        .credential_commit_received=credential_commit_received,
        .speech_preference_received=synced_speech_preference_received,
        .ota_update = gateway_ota_update,
    };
    pet_face_set_state(PET_FACE_CONNECTING);
    pet_network_set_ai_pet_id(config.ai_pet_id);
    refresh_network_face_context();
    ESP_ERROR_CHECK(pet_network_start(&config, &callbacks));
    if (!pet_config_ready(&config)) {
        pet_face_set_state(PET_FACE_PROVISIONING);
        pet_face_set_wifi_status("Choose Wi-Fi in settings; cloud setup is not complete");
    }
}
