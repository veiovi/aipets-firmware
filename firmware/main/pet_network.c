#include "pet_network.h"
#include "pet_board.h"
#include "face_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_config_v2_keys.h"
#include "pet_diagnostics.h"
#include "pet_face_pack.h"
#include "pet_protocol.h"
#include "pet_session_wire.h"
#include "pet_ima_adpcm.h"

#define WIFI_CONNECTED_BIT BIT0
/* Four seconds of 20 ms microphone chunks, in PSRAM (about 130 KB). The
 * gateway fails a turn on any sequence gap, and a full queue ends the listen,
 * so a Wi-Fi hiccup of a few seconds must not drop a chunk. */
#define AUDIO_TX_QUEUE_DEPTH 200
/* esp_websocket_client aborts the session when a frame write times out
 * midway, and fails a frame that cannot take its client lock in time. At
 * 250 ms both happened routinely while voice was streaming. */
#define WS_SEND_TIMEOUT pdMS_TO_TICKS(2000)
#define AUDIO_TX_FRAME_MAX (PET_AUDIO_HEADER_BYTES + PET_PCM_CHUNK_BYTES)
#define RX_FRAME_MAX 8192
#define INSTALLED_FACE_MAX 5
#define WIFI_SCAN_TASK_STACK_BYTES 4096
#define WIFI_SCAN_START_RETRIES 10
#define WIFI_SCAN_RETRY_DELAY_MS 50
#define WIFI_RETRIES_PER_PROFILE 4

typedef struct {
    size_t length;
    unsigned generation;
    bool text;
    uint8_t data[AUDIO_TX_FRAME_MAX];
} audio_tx_item_t;

static const char *TAG = "pet_network";
static EventGroupHandle_t s_events;
static QueueHandle_t s_audio_tx;
static SemaphoreHandle_t s_send_lock;
/* Latest telemetry/config/speech acknowledgement; bounded and idempotent.
 * Control worker drains these over HTTPS, never from the socket callback. */
static char *s_management_reports[3];
static esp_websocket_client_handle_t s_websocket;
static pet_network_callbacks_t s_callbacks;
typedef void (*connected_callback_t)(bool);
static _Atomic(connected_callback_t) s_wifi_connected_callback;
static atomic_uint s_socket_generation;
static bool s_gesture_sequence_valid;
static uint32_t s_last_gesture_sequence;
/* The speech stream whose frames are IMA ADPCM, and one decoded frame (PSRAM:
 * malloc prefers it here), at most the speaker's 2 KB frame. */
#define SPEECH_ADPCM_MAX_SAMPLES 1024
static bool s_speech_adpcm;
static uint32_t s_speech_stream;
static int16_t *s_speech_pcm;
static pet_config_t s_config;
static atomic_bool s_websocket_ready;
static bool s_legacy_gateway_enabled;
static bool s_bound_session;
static bool s_bound_hello_sent;
static char s_bound_device_id[37];
static pet_control_context_t s_bound_context;
static bool s_tx_started,s_telemetry_started;
static volatile uint32_t s_dropped_chunks;
static uint8_t *s_rx_frame;
static size_t s_rx_expected;
static uint8_t s_rx_opcode;
static pet_network_scan_callback_t s_scan_callback;
static volatile bool s_scan_running;
static volatile bool s_scan_suspended_reconnect;
static TaskHandle_t s_scan_task;
static char s_active_face_id[PET_FACE_ID_MAX];
static char s_installed_face_ids[INSTALLED_FACE_MAX][PET_FACE_ID_MAX];
static size_t s_installed_face_count;
static pet_face_profiles_t s_face_profiles;
static char s_ai_pet_id[81] = "pablo";
static bool s_synced_settings_pending;
static pet_synced_settings_t s_pending_synced_settings;
static bool s_synced_settings_queued;
static pet_synced_settings_t s_queued_synced_settings;
static bool s_settings_refresh_after_pending;
static bool s_speech_preference_pending;
static pet_speech_preference_t s_pending_speech_preference;
static bool s_config_v2_pending;
static pet_synced_config_v2_t s_pending_config_v2;
static uint8_t s_wifi_retry_count;
static bool s_wifi_using_fallback;
static pet_battery_snapshot_t s_battery_snapshot;
/* session-rebind-v1: the gateway's hello listed it, a device.binding.changed is
 * awaiting its answer, and the context it asked for (PSRAM, guarded by
 * s_rebind_lock). */
static bool s_gateway_rebind;
/* story-v1: the gateway's hello listed it, so a long press may ask for a story. */
static bool s_gateway_story;
static atomic_bool s_rebind_pending;
static pet_control_context_t *s_rebind_context;
static SemaphoreHandle_t s_rebind_lock;

void pet_network_set_battery_snapshot(const pet_battery_snapshot_t *snapshot)
{
    if (snapshot) s_battery_snapshot = *snapshot;
}

static const char *battery_state_name(pet_battery_state_t state)
{
    switch (state) {
        case PET_BATTERY_DISCHARGING: return "discharging";
        case PET_BATTERY_CHARGING: return "charging";
        case PET_BATTERY_FULL: return "full";
        case PET_BATTERY_IDLE: return "idle";
    }
    return "idle";
}

static esp_err_t connect_wifi_profile(bool fallback)
{
    const char *ssid = fallback ? s_config.wifi_fallback_ssid : s_config.wifi_ssid;
    const char *password = fallback ? s_config.wifi_fallback_password : s_config.wifi_password;
    if (!ssid[0]) return ESP_ERR_INVALID_STATE;
    wifi_config_t wifi = {0};
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (ssid_length > sizeof(wifi.sta.ssid) || password_length > sizeof(wifi.sta.password))
        return ESP_ERR_INVALID_ARG;
    memcpy(wifi.sta.ssid, ssid, ssid_length);
    memcpy(wifi.sta.password, password, password_length);
    wifi.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi), TAG,
                        "set Wi-Fi profile");
    s_wifi_using_fallback = fallback;
    ESP_LOGI(TAG, "connecting with %s Wi-Fi profile: %s",
             fallback ? "fallback" : "primary", ssid);
    return esp_wifi_connect();
}

static bool parse_speech_profile(const char *name, pet_speech_profile_t *profile)
{
    if (!name || !profile) return false;
    if (!strcmp(name, "cartesia-batch") || !strcmp(name, "batch-whisper")) *profile = PET_SPEECH_PROFILE_CARTESIA_BATCH;
    else if (!strcmp(name, "cartesia-realtime") || !strcmp(name, "realtime-ink2")) *profile = PET_SPEECH_PROFILE_CARTESIA_REALTIME;
    else if (!strcmp(name, "fish-direct")) *profile = PET_SPEECH_PROFILE_FISH_DIRECT;
    else return false;
    return true;
}

static bool synced_settings_equal(const pet_synced_settings_t *left,
                                  const pet_synced_settings_t *right)
{
    return left && right && left->version == right->version &&
        left->volume == right->volume &&
        left->recording_timeout_seconds == right->recording_timeout_seconds &&
        left->legacy_voice == right->legacy_voice &&
        !strcmp(left->face_id, right->face_id) &&
        !strcmp(left->ai_pet_id, right->ai_pet_id);
}

static bool config_v2_equal(const pet_synced_config_v2_t *left,
                            const pet_synced_config_v2_t *right)
{
    return left && right && left->version == right->version &&
        left->volume == right->volume && left->brightness == right->brightness &&
        left->shake_sensitivity == right->shake_sensitivity &&
        left->recording_timeout_seconds == right->recording_timeout_seconds &&
        left->animation_profile == right->animation_profile &&
        left->speech_profile == right->speech_profile &&
        left->has_speech_mouth_offset == right->has_speech_mouth_offset &&
        left->speech_mouth_offset_ms == right->speech_mouth_offset_ms &&
        !strcmp(left->fingerprint, right->fingerprint) &&
        !strcmp(left->face_id, right->face_id) &&
        !strcmp(left->ai_pet_id, right->ai_pet_id);
}

static cJSON *add_settings_json(cJSON *parent, const char *name, const pet_config_t *config,
                                const char *ai_pet_id)
{
    cJSON *settings = cJSON_AddObjectToObject(parent, name);
    if (!settings) return NULL;
    cJSON_AddNumberToObject(settings, "volume", config->volume);
    cJSON_AddNumberToObject(settings, "recordingTimeoutSeconds", config->recording_timeout_seconds);
    cJSON_AddStringToObject(settings, "aiPetId",
                            ai_pet_id && ai_pet_id[0] ? ai_pet_id : "pablo");
    cJSON_AddStringToObject(settings, "faceId", config->face_id);
    cJSON_AddStringToObject(settings, "legacyVoice", pet_voice_name(config->voice));
    return settings;
}

static cJSON *add_config_v2_json(cJSON *parent, const char *name,
                                 const pet_config_t *config,
                                 const char *ai_pet_id)
{
    cJSON *settings = cJSON_AddObjectToObject(parent, name);
    if (!settings) return NULL;
    cJSON_AddNumberToObject(settings, "volume", config->volume);
    cJSON_AddNumberToObject(settings, "brightness", config->brightness);
    cJSON_AddNumberToObject(settings, "shakeSensitivity", config->shake_sensitivity);
    cJSON_AddNumberToObject(settings, "recordingTimeoutSeconds",
                            config->recording_timeout_seconds);
    const char *animation_profile =
        pet_animation_profile_wire_value(config->animation_profile);
    if (!animation_profile) return NULL;
    cJSON_AddStringToObject(settings, "animationProfile", animation_profile);
    cJSON_AddStringToObject(settings, "aiPetId",
                            ai_pet_id && ai_pet_id[0] ? ai_pet_id : "luna");
    cJSON_AddStringToObject(settings, "faceId", config->face_id);
    cJSON_AddStringToObject(settings, "speechProfile",
                            pet_speech_profile_name(config->speech_profile));
    /* Only after the cloud sent it: a cloud that does not know the field
     * refuses a document with an extra key. */
    if (config->has_speech_mouth_offset)
        cJSON_AddNumberToObject(settings, "speechMouthOffsetMs", config->speech_mouth_offset_ms);
    return settings;
}

/* Config v2 settings are strict, as the cloud's schema is. */
static bool config_v2_keys_known(const cJSON *settings)
{
    if (!cJSON_IsObject(settings)) return false;
    for (const cJSON *item = settings->child; item; item = item->next) {
        if (!pet_config_v2_setting_known(item->string)) return false;
    }
    return true;
}

static bool parse_face_profile(const cJSON *item, pet_face_profile_t *profile)
{
    if (!cJSON_IsObject(item) || !profile) return false;
    const cJSON *face_id = cJSON_GetObjectItemCaseSensitive(item, "faceId");
    const cJSON *ai_mode = cJSON_GetObjectItemCaseSensitive(item, "aiMode");
    const cJSON *voice = cJSON_GetObjectItemCaseSensitive(item, "voice");
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(item, "realtimeModel");
    const cJSON *rt_voice = cJSON_GetObjectItemCaseSensitive(item, "realtimeVoice");
    const cJSON *cartesia = cJSON_GetObjectItemCaseSensitive(item, "cartesiaVoiceGender");
    if (!cJSON_IsString(face_id) || !pet_face_id_valid(face_id->valuestring) ||
        !cJSON_IsString(ai_mode) || !cJSON_IsString(voice) || !cJSON_IsString(model) ||
        !cJSON_IsString(rt_voice) || !cJSON_IsString(cartesia)) return false;
    memset(profile, 0, sizeof(*profile));
    strlcpy(profile->face_id, face_id->valuestring, sizeof(profile->face_id));
    bool found = false;
    for (pet_ai_mode_t value = 0; value < PET_AI_MODE_COUNT; value++) {
        if (!strcmp(ai_mode->valuestring, pet_ai_mode_name(value))) { profile->ai_mode = value; found = true; break; }
    }
    if (!found) return false;
    found = false;
    for (pet_voice_t value = 0; value < PET_VOICE_COUNT; value++) {
        if (!strcmp(voice->valuestring, pet_voice_name(value))) { profile->voice = value; found = true; break; }
    }
    if (!found) return false;
    found = false;
    for (pet_realtime_model_t value = 0; value < PET_REALTIME_MODEL_COUNT; value++) {
        if (!strcmp(model->valuestring, pet_realtime_model_name(value))) { profile->realtime_model = value; found = true; break; }
    }
    if (!found) return false;
    found = false;
    for (pet_realtime_voice_t value = 0; value < PET_REALTIME_VOICE_COUNT; value++) {
        if (!strcmp(rt_voice->valuestring, pet_realtime_voice_name(value))) { profile->realtime_voice = value; found = true; break; }
    }
    if (!found) return false;
    found = false;
    for (pet_cartesia_voice_gender_t value = 0; value < PET_CARTESIA_VOICE_GENDER_COUNT; value++) {
        if (!strcmp(cartesia->valuestring, pet_cartesia_voice_gender_name(value))) { profile->cartesia_voice_gender = value; found = true; break; }
    }
    return found && pet_face_profile_valid(profile);
}

static void add_face_profile_json(cJSON *array, const pet_face_profile_t *profile)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "faceId", profile->face_id);
    cJSON_AddStringToObject(item, "aiMode", pet_ai_mode_name(profile->ai_mode));
    cJSON_AddStringToObject(item, "voice", pet_voice_name(profile->voice));
    cJSON_AddStringToObject(item, "realtimeModel", pet_realtime_model_name(profile->realtime_model));
    cJSON_AddStringToObject(item, "realtimeVoice", pet_realtime_voice_name(profile->realtime_voice));
    cJSON_AddStringToObject(item, "cartesiaVoiceGender", pet_cartesia_voice_gender_name(profile->cartesia_voice_gender));
    cJSON_AddItemToArray(array, item);
}

static void finish_wifi_scan(void)
{
    uint16_t count = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t *records = NULL;
    if (err == ESP_OK && count) {
        records = calloc(count, sizeof(*records));
        if (!records) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && count) err = esp_wifi_scan_get_ap_records(&count, records);

    pet_wifi_network_t networks[PET_WIFI_SCAN_MAX] = {0};
    size_t network_count = 0;
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count && network_count < PET_WIFI_SCAN_MAX; ++i) {
            if (!records[i].ssid[0]) continue;
            bool duplicate = false;
            for (size_t j = 0; j < network_count; ++j) {
                if (!strcmp(networks[j].ssid, (const char *)records[i].ssid)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            strlcpy(networks[network_count].ssid, (const char *)records[i].ssid,
                    sizeof(networks[network_count].ssid));
            networks[network_count].rssi = records[i].rssi;
            networks[network_count].secured = records[i].authmode != WIFI_AUTH_OPEN;
            networks[network_count].saved =
                pet_config_wifi_saved(networks[network_count].ssid);
            network_count++;
        }
    }
    free(records);
    s_scan_running = false;
    pet_network_scan_callback_t callback = s_scan_callback;
    s_scan_callback = NULL;
    if (s_scan_suspended_reconnect) {
        s_scan_suspended_reconnect = false;
        esp_err_t reconnect_err = esp_wifi_connect();
        if (reconnect_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi reconnect after scan failed: %s",
                     esp_err_to_name(reconnect_err));
        }
    }
    if (callback) callback(networks, network_count, err);
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        finish_wifi_scan();
    }
}

static esp_err_t send_text_unchecked(const char *text)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    int written = s_websocket?esp_websocket_client_send_text(s_websocket, text, strlen(text), WS_SEND_TIMEOUT):-1;
    xSemaphoreGive(s_send_lock);
    return written == (int)strlen(text) ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_text(const char *text)
{
    if (s_bound_session)
    {
        cJSON *root = cJSON_Parse(text);
        const cJSON *type = root ? cJSON_GetObjectItemCaseSensitive(root, "type") : NULL;
        if (!cJSON_IsString(type))
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (!pet_session_wire_brain_message(type->valuestring, false))
        {
            bool accepted = pet_session_wire_management_message(type->valuestring, false) && strlen(text) <= 4096;
            unsigned slot = !strcmp(type->valuestring, "device.telemetry") ? 0 :
                !strcmp(type->valuestring, "device.speech.profile.confirmed") ? 2 : 1;
            char *copy = accepted ? strdup(text) : NULL;
            if (copy)
            {
                xSemaphoreTake(s_send_lock, portMAX_DELAY);
                free(s_management_reports[slot]);
                s_management_reports[slot] = copy;
                xSemaphoreGive(s_send_lock);
            }
            cJSON_Delete(root);
            return copy ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
        cJSON_Delete(root);
    }
    if (!s_websocket_ready) return ESP_ERR_INVALID_STATE;
    return send_text_unchecked(text);
}

static void send_hello(void)
{
    if(s_bound_session) {
        if(s_bound_hello_sent)return;
        cJSON *root=cJSON_CreateObject();if(!root)return;
        cJSON_AddNumberToObject(root,"v",PET_PROTOCOL_VERSION);
        cJSON_AddStringToObject(root,"type","device.hello");
        cJSON_AddStringToObject(root,"deviceId",s_bound_device_id);
        cJSON_AddStringToObject(root,"bootId",pet_diagnostics_boot_id());
        cJSON_AddStringToObject(root,"firmware",esp_app_get_description()->version);
        cJSON_AddStringToObject(root,"hardware",pet_board_current()->id);
        cJSON_AddStringToObject(root,"token",s_config.pairing_token);
        cJSON_AddStringToObject(root,"activeFaceId",s_bound_context.config.face_id);
        cJSON *installed=cJSON_AddArrayToObject(root,"installedFaceIds");
        cJSON_AddItemToArray(installed,cJSON_CreateString(s_bound_context.config.face_id));
        cJSON *binding=cJSON_AddObjectToObject(root,"binding");
        cJSON_AddStringToObject(binding,"revision",s_bound_context.binding.revision);
        cJSON_AddStringToObject(binding,"relationshipId",s_bound_context.binding.relationship_id);
        cJSON_AddStringToObject(binding,"buildId",s_bound_context.binding.build_id);
        cJSON_AddStringToObject(binding,"sha256",s_bound_context.binding.sha256);
        cJSON_AddStringToObject(binding,"configVersion",s_bound_context.config.version);
        cJSON *caps=cJSON_AddArrayToObject(root,"capabilities");
        const char *names[]={"audio-in","audio-out","touch","gestures",
            "session-binding-v1",PET_BRAIN_CAPABILITY,"text-v1",PET_IMA_ADPCM_CAPABILITY,
#if CONFIG_PET_POCKET_TERMINAL
            PET_SESSION_REBIND_CAPABILITY,PET_STORY_CAPABILITY,
#endif
        };
        for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)cJSON_AddItemToArray(caps,cJSON_CreateString(names[i]));
        char *json=cJSON_PrintUnformatted(root);
        if(json){s_bound_hello_sent=send_text_unchecked(json)==ESP_OK;memset(json,0,strlen(json));free(json);}
        cJSON *token=cJSON_GetObjectItemCaseSensitive(root,"token");
        if(cJSON_IsString(token))memset(token->valuestring,0,strlen(token->valuestring));
        cJSON_Delete(root);return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[24];
    snprintf(device_id, sizeof(device_id), "pet-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", PET_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "device.hello");
    cJSON_AddStringToObject(root, "deviceId", device_id);
    cJSON_AddStringToObject(root, "bootId", pet_diagnostics_boot_id());
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "hardware", pet_board_current()->id);
    cJSON_AddStringToObject(root, "token", s_config.pairing_token);
    cJSON_AddStringToObject(root, "voice", pet_voice_name(s_config.voice));
    cJSON_AddStringToObject(root, "aiMode", pet_ai_mode_name(s_config.ai_mode));
    cJSON_AddStringToObject(root, "realtimeModel", pet_realtime_model_name(s_config.realtime_model));
    cJSON_AddStringToObject(root, "realtimeVoice", pet_realtime_voice_name(s_config.realtime_voice));
    cJSON_AddStringToObject(root, "cartesiaVoiceGender",
                           pet_cartesia_voice_gender_name(s_config.cartesia_voice_gender));
    cJSON_AddNumberToObject(root, "settingsVersion", s_config.settings_version);
    cJSON_AddNumberToObject(root, "configSchemaVersion", 2);
    cJSON_AddNumberToObject(root, "appliedConfigVersion",
                            strlen(s_config.config_fingerprint) == 64 ? s_config.settings_version : 0);
    if (strlen(s_config.config_fingerprint) == 64)
        cJSON_AddStringToObject(root, "configFingerprint",
                               s_config.config_fingerprint);
    add_config_v2_json(root, "reportedConfig", &s_config, s_ai_pet_id);
    cJSON_AddNumberToObject(root, "speechProfileVersion", s_config.speech_profile_version);
    cJSON_AddStringToObject(root, "speechProfile", pet_speech_profile_name(s_config.speech_profile));
    add_settings_json(root, "settings", &s_config, s_ai_pet_id);
    if (s_active_face_id[0]) cJSON_AddStringToObject(root, "activeFaceId", s_active_face_id);
    if (s_installed_face_count) {
        pet_face_catalog_item_t catalog[PET_FACE_CATALOG_CAPACITY]={0};
        size_t catalog_count=0;
        pet_face_pack_list(catalog,PET_FACE_CATALOG_CAPACITY,&catalog_count);
        cJSON *installed = cJSON_AddArrayToObject(root, "installedFaceIds");
        for (size_t i = 0; i < s_installed_face_count; i++)
            cJSON_AddItemToArray(installed, cJSON_CreateString(s_installed_face_ids[i]));
        cJSON *inventory = cJSON_AddArrayToObject(root, "inventory");
        for (size_t i = 0; i < s_installed_face_count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "faceId", s_installed_face_ids[i]);
            cJSON_AddStringToObject(item, "aiPetId", s_installed_face_ids[i]);
            for(size_t catalog_index=0;catalog_index<catalog_count;
                ++catalog_index){
                if(strcmp(catalog[catalog_index].id,s_installed_face_ids[i]))
                    continue;
                if(catalog[catalog_index].version[0])
                    cJSON_AddStringToObject(item,"packVersion",
                                            catalog[catalog_index].version);
                if(strlen(catalog[catalog_index].sha256)==64)
                    cJSON_AddStringToObject(item,"sha256",
                                            catalog[catalog_index].sha256);
                cJSON_AddNumberToObject(item,"sizeBytes",
                                        catalog[catalog_index].pack_bytes);
                cJSON_AddNumberToObject(item,"formatVersion",
                                        catalog[catalog_index].format_version);
                break;
            }
            cJSON_AddStringToObject(item, "source", "embedded");
            cJSON_AddBoolToObject(item, "compatible", true);
            cJSON_AddBoolToObject(item, "active",
                                  !strcmp(s_installed_face_ids[i],
                                          s_active_face_id));
            cJSON_AddItemToArray(inventory, item);
        }
    }
    cJSON *capacity=cJSON_AddObjectToObject(root,"assetCapacity");
    cJSON_AddNumberToObject(capacity,"totalBytes",0);
    cJSON_AddNumberToObject(capacity,"availableBytes",0);
    cJSON_AddNumberToObject(capacity,"maxPackBytes",0);
    cJSON *wifi_networks = cJSON_AddArrayToObject(root, "wifiNetworks");
    pet_wifi_network_metadata_t saved_networks[PET_WIFI_PROFILE_MAX] = {0};
    size_t saved_network_count = 0;
    const char *active_ssid = s_wifi_using_fallback ?
        s_config.wifi_fallback_ssid : s_config.wifi_ssid;
    if (pet_config_list_wifi_networks(saved_networks,
            PET_WIFI_PROFILE_MAX,&saved_network_count)==ESP_OK &&
        saved_network_count) {
        for (size_t index=0; index<saved_network_count; ++index) {
            cJSON *network = cJSON_CreateObject();
            cJSON_AddStringToObject(network,"ssid",saved_networks[index].ssid);
            cJSON_AddNumberToObject(network,"priority",
                                    saved_networks[index].priority);
            cJSON_AddBoolToObject(network,"saved",true);
            cJSON_AddBoolToObject(network,"active",
                                  !strcmp(saved_networks[index].ssid,
                                          active_ssid));
            cJSON_AddItemToArray(wifi_networks,network);
        }
    } else if (s_config.wifi_ssid[0]) {
        cJSON *network=cJSON_CreateObject();
        cJSON_AddStringToObject(network,"ssid",s_config.wifi_ssid);
        cJSON_AddNumberToObject(network,"priority",100);
        cJSON_AddBoolToObject(network,"saved",true);
        cJSON_AddBoolToObject(network,"active",!s_wifi_using_fallback);
        cJSON_AddItemToArray(wifi_networks,network);
    }
    if (s_face_profiles.count) {
        cJSON *profiles = cJSON_AddArrayToObject(root, "faceProfiles");
        for (size_t i = 0; i < s_face_profiles.count; i++)
            add_face_profile_json(profiles, &s_face_profiles.entries[i]);
    }
    if (s_config.credential_version > 0) cJSON_AddNumberToObject(root, "credentialVersion", s_config.credential_version);
    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("audio-in"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("audio-out"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("touch"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("cartesia-gender-select"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("voice-select"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("realtime-select"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("gestures"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("face-profile-sync"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("settings-sync"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("speech-profile-v1"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ota-v1"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("config-v2"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("pet-inventory-v1"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("wifi-admin-v1"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("credential-rotation-v1"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("crash-ack-v1"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("speechMouthOffset"));
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        send_text_unchecked(json);
        free(json);
    }
    cJSON_Delete(root);
}

esp_err_t pet_network_set_voice(pet_voice_t voice)
{
    if (voice < PET_VOICE_PUCK || voice >= PET_VOICE_COUNT) return ESP_ERR_INVALID_ARG;
    bool changed = s_config.voice != voice;
    s_config.voice = voice;
    if (changed && s_websocket_ready) send_hello();
    return ESP_OK;
}

void pet_network_set_ai_pet_id(const char *ai_pet_id)
{
    if (pet_config_ai_pet_id_valid(ai_pet_id))
        strlcpy(s_ai_pet_id, ai_pet_id, sizeof(s_ai_pet_id));
}

esp_err_t pet_network_get_ai_pet_id(char *ai_pet_id, size_t capacity)
{
    if (!ai_pet_id || !capacity) return ESP_ERR_INVALID_ARG;
    strlcpy(ai_pet_id, s_ai_pet_id, capacity);
    return ESP_OK;
}

esp_err_t pet_network_settings_changed(const pet_config_t *config, const char *ai_pet_id)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    pet_network_set_ai_pet_id(ai_pet_id);
    s_config.volume = config->volume;
    s_config.recording_timeout_seconds = config->recording_timeout_seconds;
    strlcpy(s_config.face_id, config->face_id, sizeof(s_config.face_id));
    strlcpy(s_config.ai_pet_id, s_ai_pet_id, sizeof(s_config.ai_pet_id));
    s_config.voice = config->voice;
    s_config.brightness = config->brightness;
    s_config.shake_sensitivity = config->shake_sensitivity;
    s_config.animation_profile = config->animation_profile;
    s_config.speech_profile = config->speech_profile;
    s_config.has_speech_mouth_offset = config->has_speech_mouth_offset;
    s_config.speech_mouth_offset_ms = config->speech_mouth_offset_ms;
    s_config.settings_version = config->settings_version;
    if (!s_websocket_ready) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", PET_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "device.config.proposed");
    cJSON_AddNumberToObject(root, "baseVersion", config->settings_version);
    cJSON_AddStringToObject(root, "bootId", pet_diagnostics_boot_id());
    add_config_v2_json(root, "settings", config, s_ai_pet_id);
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = json ? send_text(json) : ESP_ERR_NO_MEM;
    free(json);
    cJSON_Delete(root);
    return err;
}

static esp_err_t refresh_durable_settings(void)
{
    pet_config_t current = s_config;
    return pet_network_settings_changed(&current, s_ai_pet_id);
}

static bool dispatch_synced_settings(const pet_synced_settings_t *settings)
{
    if (!settings || !s_callbacks.settings_received) return false;
    s_pending_synced_settings = *settings;
    s_synced_settings_pending = true;
    if (s_callbacks.settings_received(settings)) return true;
    if (s_synced_settings_pending &&
        synced_settings_equal(settings, &s_pending_synced_settings)) {
        s_synced_settings_pending = false;
        memset(&s_pending_synced_settings, 0,
               sizeof(s_pending_synced_settings));
    }
    return false;
}

esp_err_t pet_network_synced_settings_result(
    const pet_synced_settings_t *settings, bool applied)
{
    if (!settings) return ESP_ERR_INVALID_ARG;
    if (!s_synced_settings_pending ||
        !synced_settings_equal(settings, &s_pending_synced_settings)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (applied) {
        s_config.settings_version = settings->version;
        s_config.volume = settings->volume;
        s_config.recording_timeout_seconds =
            settings->recording_timeout_seconds;
        strlcpy(s_config.face_id, settings->face_id,
                sizeof(s_config.face_id));
        s_config.voice = settings->legacy_voice;
        strlcpy(s_ai_pet_id, settings->ai_pet_id, sizeof(s_ai_pet_id));
        strlcpy(s_config.ai_pet_id, settings->ai_pet_id,
                sizeof(s_config.ai_pet_id));
    }
    s_synced_settings_pending = false;
    memset(&s_pending_synced_settings, 0,
           sizeof(s_pending_synced_settings));

    if (s_synced_settings_queued) {
        pet_synced_settings_t queued = s_queued_synced_settings;
        s_synced_settings_queued = false;
        memset(&s_queued_synced_settings, 0,
               sizeof(s_queued_synced_settings));
        s_settings_refresh_after_pending = false;
        if (!dispatch_synced_settings(&queued)) {
            refresh_durable_settings();
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (!applied || s_settings_refresh_after_pending) {
        s_settings_refresh_after_pending = false;
        return refresh_durable_settings();
    }
    return ESP_OK;
}

esp_err_t pet_network_config_v2_result(const pet_synced_config_v2_t *settings,
                                       bool applied, const char *error_code,
                                       const char *field)
{
    if (!settings) return ESP_ERR_INVALID_ARG;
    if (s_config_v2_pending &&
        !config_v2_equal(settings, &s_pending_config_v2))
        return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", PET_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type",
                            applied ? "device.config.applied" :
                                      "device.config.rejected");
    cJSON_AddNumberToObject(root, "version", settings->version);
    if (applied) {
        cJSON_AddStringToObject(root, "fingerprint", settings->fingerprint);
        cJSON_AddStringToObject(root, "bootId", pet_diagnostics_boot_id());
        pet_config_t report = s_config;
        report.volume = settings->volume;
        report.brightness = settings->brightness;
        report.shake_sensitivity = settings->shake_sensitivity;
        report.recording_timeout_seconds = settings->recording_timeout_seconds;
        report.animation_profile = settings->animation_profile;
        report.speech_profile = settings->speech_profile;
        report.has_speech_mouth_offset = settings->has_speech_mouth_offset;
        report.speech_mouth_offset_ms = settings->speech_mouth_offset_ms;
        strlcpy(report.face_id, settings->face_id, sizeof(report.face_id));
        strlcpy(report.ai_pet_id, settings->ai_pet_id,
                sizeof(report.ai_pet_id));
        add_config_v2_json(root, "settings", &report, settings->ai_pet_id);
        s_config.volume = report.volume;
        s_config.brightness = report.brightness;
        s_config.shake_sensitivity = report.shake_sensitivity;
        s_config.recording_timeout_seconds = report.recording_timeout_seconds;
        s_config.animation_profile = report.animation_profile;
        s_config.speech_profile = report.speech_profile;
        s_config.has_speech_mouth_offset = report.has_speech_mouth_offset;
        s_config.speech_mouth_offset_ms = report.speech_mouth_offset_ms;
        strlcpy(s_config.face_id, report.face_id, sizeof(s_config.face_id));
        strlcpy(s_config.ai_pet_id, report.ai_pet_id, sizeof(s_config.ai_pet_id));
        s_config.settings_version = settings->version;
        s_config.config_schema_version = 2;
        strlcpy(s_config.config_fingerprint, settings->fingerprint,
                sizeof(s_config.config_fingerprint));
        strlcpy(s_ai_pet_id, settings->ai_pet_id, sizeof(s_ai_pet_id));
    } else {
        cJSON_AddStringToObject(root, "code",
                                error_code && error_code[0] ? error_code :
                                                              "APPLY_FAILED");
        if (field && field[0]) cJSON_AddStringToObject(root, "field", field);
    }
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = json ? send_text(json) : ESP_ERR_NO_MEM;
    free(json);
    cJSON_Delete(root);
    s_config_v2_pending = false;
    memset(&s_pending_config_v2, 0, sizeof(s_pending_config_v2));
    return err;
}

esp_err_t pet_network_wifi_admin_result(const char *operation_id,
                                        const char *ssid,
                                        const char *status,
                                        const char *error_code,
                                        uint8_t priority)
{
    if (!operation_id || strlen(operation_id) != 36 || !ssid || !ssid[0] ||
        !status || (strcmp(status,"connected") && strcmp(status,"failed") &&
                    strcmp(status,"rolled_back") && strcmp(status,"forgotten")))
        return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root,"v",PET_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root,"type","device.wifi.result");
    cJSON_AddStringToObject(root,"operationId",operation_id);
    cJSON_AddStringToObject(root,"ssid",ssid);
    cJSON_AddStringToObject(root,"status",status);
    cJSON_AddNumberToObject(root,"priority",priority);
    if (error_code && error_code[0])
        cJSON_AddStringToObject(root,"code",error_code);
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = json ? send_text(json) : ESP_ERR_NO_MEM;
    free(json);cJSON_Delete(root);return err;
}

static esp_err_t send_credential_result(const char *type,
                                        const char *operation_id,
                                        uint32_t version)
{
    if (!operation_id || strlen(operation_id) != 36 || !version)
        return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root,"v",PET_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root,"type",type);
    cJSON_AddStringToObject(root,"operationId",operation_id);
    cJSON_AddNumberToObject(root,"credentialVersion",version);
    char *json=cJSON_PrintUnformatted(root);
    esp_err_t err=json?send_text(json):ESP_ERR_NO_MEM;
    free(json);cJSON_Delete(root);return err;
}

esp_err_t pet_network_credential_prepared(const char *operation_id,
                                          uint32_t version)
{
    return send_credential_result("device.credential.prepared",operation_id,
                                  version);
}

esp_err_t pet_network_credential_committed(const char *operation_id,
                                           uint32_t version)
{
    return send_credential_result("device.credential.committed",operation_id,
                                  version);
}

esp_err_t pet_network_speech_profile_changed(pet_speech_profile_t profile)
{
    if (profile >= PET_SPEECH_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_websocket_ready) return ESP_ERR_INVALID_STATE;
    cJSON *root=cJSON_CreateObject();if(!root)return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root,"v",PET_PROTOCOL_VERSION);cJSON_AddStringToObject(root,"type","device.speech.profile.changed");cJSON_AddStringToObject(root,"aiPetId",s_ai_pet_id);cJSON_AddNumberToObject(root,"baseVersion",s_config.speech_profile_version);cJSON_AddStringToObject(root,"profile",pet_speech_profile_name(profile));
    char *json=cJSON_PrintUnformatted(root);esp_err_t err=json?send_text(json):ESP_ERR_NO_MEM;free(json);cJSON_Delete(root);return err;
}

esp_err_t pet_network_speech_profile_result(const pet_speech_preference_t *preference,bool applied)
{
    if(!preference||!s_speech_preference_pending)return ESP_ERR_INVALID_STATE;
    if(applied){s_config.speech_profile=preference->profile;s_config.speech_profile_version=preference->version;strlcpy(s_ai_pet_id,preference->ai_pet_id,sizeof(s_ai_pet_id));
        char json[192];snprintf(json,sizeof(json),"{\"v\":1,\"type\":\"device.speech.profile.confirmed\",\"aiPetId\":\"%s\",\"version\":%lu}",preference->ai_pet_id,(unsigned long)preference->version);send_text(json);}
    s_speech_preference_pending=false;memset(&s_pending_speech_preference,0,sizeof(s_pending_speech_preference));return ESP_OK;
}

esp_err_t pet_network_set_ai_preferences(pet_ai_mode_t mode, pet_realtime_model_t model,
                                         pet_realtime_voice_t voice,
                                         pet_cartesia_voice_gender_t cartesia_voice_gender)
{
    if (mode >= PET_AI_MODE_COUNT || model >= PET_REALTIME_MODEL_COUNT ||
        voice >= PET_REALTIME_VOICE_COUNT ||
        cartesia_voice_gender >= PET_CARTESIA_VOICE_GENDER_COUNT) return ESP_ERR_INVALID_ARG;
    bool changed = s_config.ai_mode != mode || s_config.realtime_model != model ||
                   s_config.realtime_voice != voice ||
                   s_config.cartesia_voice_gender != cartesia_voice_gender;
    s_config.ai_mode = mode;
    s_config.realtime_model = model;
    s_config.realtime_voice = voice;
    s_config.cartesia_voice_gender = cartesia_voice_gender;
    if (changed && s_websocket_ready) send_hello();
    return ESP_OK;
}

static esp_err_t set_profile_preferences(const pet_face_profile_t *profile,
                                         bool announce)
{
    if (!pet_face_profile_valid(profile)) return ESP_ERR_INVALID_ARG;
    bool changed = s_config.voice != profile->voice ||
                   s_config.ai_mode != profile->ai_mode ||
                   s_config.realtime_model != profile->realtime_model ||
                   s_config.realtime_voice != profile->realtime_voice ||
                   s_config.cartesia_voice_gender != profile->cartesia_voice_gender;
    s_config.voice = profile->voice;
    s_config.ai_mode = profile->ai_mode;
    s_config.realtime_model = profile->realtime_model;
    s_config.realtime_voice = profile->realtime_voice;
    s_config.cartesia_voice_gender = profile->cartesia_voice_gender;
    if (announce && changed && s_websocket_ready) send_hello();
    return ESP_OK;
}

esp_err_t pet_network_set_profile_preferences(const pet_face_profile_t *profile)
{
    return set_profile_preferences(profile, true);
}

esp_err_t pet_network_stage_profile_preferences(const pet_face_profile_t *profile)
{
    return set_profile_preferences(profile, false);
}

void pet_network_announce_profile_preferences(void)
{
    if (s_websocket_ready) send_hello();
}

esp_err_t pet_network_set_face_context(const char *active_face_id,
                                       const char installed_face_ids[][PET_FACE_ID_MAX],
                                       size_t installed_face_count,
                                       const pet_face_profiles_t *profiles)
{
    if (!active_face_id || !pet_face_id_valid(active_face_id) || !profiles ||
        installed_face_count > INSTALLED_FACE_MAX ||
        (installed_face_count && !installed_face_ids)) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < installed_face_count; i++)
        if (!pet_face_id_valid(installed_face_ids[i])) return ESP_ERR_INVALID_ARG;
    strlcpy(s_active_face_id, active_face_id, sizeof(s_active_face_id));
    memset(s_installed_face_ids, 0, sizeof(s_installed_face_ids));
    for (size_t i = 0; i < installed_face_count; i++)
        strlcpy(s_installed_face_ids[i], installed_face_ids[i], sizeof(s_installed_face_ids[i]));
    s_installed_face_count = installed_face_count;
    s_face_profiles = *profiles;
    return ESP_OK;
}

static bool installed_face_id(const char *face_id)
{
    if (!face_id) return false;
    for (size_t index = 0; index < s_installed_face_count; ++index) {
        if (!strcmp(face_id, s_installed_face_ids[index])) return true;
    }
    return false;
}

/* An accepted rebind: the session's identity, face and settings become the new
 * pet's, as pet_network_start_bound sets them for a new socket. */
static void apply_bound_context(const pet_control_context_t *context)
{
    const pet_control_config_t *c = &context->config;
    s_bound_context = *context;
    strlcpy(s_active_face_id, c->face_id, sizeof(s_active_face_id));
    memset(s_installed_face_ids, 0, sizeof(s_installed_face_ids));
    strlcpy(s_installed_face_ids[0], c->face_id, sizeof(s_installed_face_ids[0]));
    s_installed_face_count = 1;
    strlcpy(s_ai_pet_id, c->ai_pet_id, sizeof(s_ai_pet_id));
    s_config.volume = c->volume; s_config.brightness = c->brightness; s_config.shake_sensitivity = c->shake_sensitivity;
    s_config.recording_timeout_seconds = c->recording_timeout;
    s_config.animation_profile = (pet_animation_profile_t)c->animation_profile;
    s_config.speech_profile = (pet_speech_profile_t)c->speech_profile;
    strlcpy(s_config.face_id, c->face_id, sizeof(s_config.face_id));
    strlcpy(s_config.ai_pet_id, c->ai_pet_id, sizeof(s_config.ai_pet_id));
    s_config_v2_pending = false; s_speech_preference_pending = false;
}

static void process_control(const uint8_t *data, size_t length, bool management)
{
    cJSON *root = pet_control_json((const char *)data, length, PET_CONTROL_RESPONSE_MAX);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) goto done;
    if (management && !pet_session_wire_management_message(type->valuestring, true)) goto done;
    if (!management && s_bound_session && !pet_session_wire_brain_message(type->valuestring, true)) goto done;
    if (!strcmp(type->valuestring, "gateway.hello")) {
        if (s_bound_session)
        {
            bool brain = false;
            const cJSON *cap = NULL, *caps = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
            if (cJSON_IsArray(caps)) cJSON_ArrayForEach(cap, caps)
                if (cJSON_IsString(cap) && !strcmp(cap->valuestring, PET_BRAIN_CAPABILITY)) brain = true;
            if (!brain) goto done;
        }
        s_gesture_sequence_valid = false;
        s_gateway_rebind = s_bound_session && pet_session_wire_gateway_offers(root, PET_SESSION_REBIND_CAPABILITY);
        s_gateway_story = s_bound_session && pet_session_wire_gateway_offers(root, PET_STORY_CAPABILITY);
        s_websocket_ready = true;
        ESP_LOGI(TAG, "gateway handshake accepted");
        const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(root, "faceProfiles");
        if (cJSON_IsArray(profiles) && s_callbacks.face_profile) {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, profiles) {
                pet_face_profile_t profile;
                if (parse_face_profile(item, &profile)) s_callbacks.face_profile(&profile);
            }
        }
        if (s_config.pending_wifi_operation_id[0] &&
            s_config.pending_wifi_ssid[0]) {
            if (s_wifi_using_fallback)
                pet_config_restore_wifi_fallback();
            pet_network_wifi_admin_result(
                s_config.pending_wifi_operation_id,
                s_config.pending_wifi_ssid,
                s_wifi_using_fallback ? "rolled_back" : "connected",
                s_wifi_using_fallback ? "REQUESTED_NETWORK_UNAVAILABLE" : NULL,
                s_wifi_using_fallback ? 0 : 100);
            pet_config_clear_wifi_operation();
            s_config.pending_wifi_operation_id[0] = '\0';
            s_config.pending_wifi_ssid[0] = '\0';
        }
        if (s_callbacks.connected) s_callbacks.connected(true);
    } else if (!strcmp(type->valuestring, "gateway.binding.accepted")) {
        /* The open session now talks as the pet the rebind named. */
        bool accepted = false;
        xSemaphoreTake(s_rebind_lock, portMAX_DELAY);
        if (atomic_load(&s_rebind_pending) && pet_session_wire_accepted(root, s_rebind_context)) {
            apply_bound_context(s_rebind_context);
            atomic_store(&s_rebind_pending, false);
            accepted = true;
        }
        xSemaphoreGive(s_rebind_lock);
        if (accepted) {
            s_gesture_sequence_valid = false;
            ESP_LOGI(TAG, "gateway rebind accepted");
            if (s_callbacks.connected) s_callbacks.connected(true);
        } else ESP_LOGW(TAG, "ignored an acceptance of another binding");
    } else if (!strcmp(type->valuestring, "pet.config.desired")) {
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON *fingerprint = cJSON_GetObjectItemCaseSensitive(root, "fingerprint");
        const cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
        const cJSON *volume = settings ? cJSON_GetObjectItemCaseSensitive(settings, "volume") : NULL;
        const cJSON *brightness = settings ? cJSON_GetObjectItemCaseSensitive(settings, "brightness") : NULL;
        const cJSON *shake = settings ? cJSON_GetObjectItemCaseSensitive(settings, "shakeSensitivity") : NULL;
        const cJSON *timeout = settings ? cJSON_GetObjectItemCaseSensitive(settings, "recordingTimeoutSeconds") : NULL;
        const cJSON *animation = settings ? cJSON_GetObjectItemCaseSensitive(settings, "animationProfile") : NULL;
        const cJSON *ai_pet_id = settings ? cJSON_GetObjectItemCaseSensitive(settings, "aiPetId") : NULL;
        const cJSON *face_id = settings ? cJSON_GetObjectItemCaseSensitive(settings, "faceId") : NULL;
        const cJSON *speech = settings ? cJSON_GetObjectItemCaseSensitive(settings, "speechProfile") : NULL;
        const cJSON *mouth_offset = settings ? cJSON_GetObjectItemCaseSensitive(settings, "speechMouthOffsetMs") : NULL;
        pet_animation_profile_t animation_profile;
        pet_speech_profile_t speech_profile;
        int16_t speech_mouth_offset_ms = 0;
        const bool mouth_offset_valid = pet_speech_mouth_offset_parse(
            !mouth_offset ? PET_SPEECH_MOUTH_OFFSET_ABSENT :
            cJSON_IsNumber(mouth_offset) ? PET_SPEECH_MOUTH_OFFSET_NUMBER : PET_SPEECH_MOUTH_OFFSET_OTHER,
            cJSON_IsNumber(mouth_offset) ? mouth_offset->valuedouble : 0.0, &speech_mouth_offset_ms);
        if (cJSON_IsNumber(schema) && schema->valueint == 2 &&
            config_v2_keys_known(settings) && mouth_offset_valid &&
            cJSON_IsNumber(version) && version->valuedouble >= 1 &&
            cJSON_IsString(fingerprint) && strlen(fingerprint->valuestring) == 64 &&
            cJSON_IsNumber(volume) && volume->valuedouble >= 0 && volume->valuedouble <= 100 &&
            cJSON_IsNumber(brightness) && brightness->valuedouble >= PET_BRIGHTNESS_MIN && brightness->valuedouble <= 100 &&
            cJSON_IsNumber(shake) && shake->valuedouble >= 0 && shake->valuedouble <= 100 &&
            cJSON_IsNumber(timeout) && pet_config_recording_timeout_valid((uint16_t)timeout->valuedouble) &&
            cJSON_IsString(animation) && pet_animation_profile_parse_wire(animation->valuestring, &animation_profile) &&
            cJSON_IsString(ai_pet_id) && pet_config_ai_pet_id_valid(ai_pet_id->valuestring) &&
            cJSON_IsString(face_id) && pet_face_id_valid(face_id->valuestring) &&
            (management || installed_face_id(face_id->valuestring)) &&
            cJSON_IsString(speech) && parse_speech_profile(speech->valuestring, &speech_profile) &&
            speech_profile != PET_SPEECH_PROFILE_FISH_DIRECT) {
            pet_synced_config_v2_t next = {
                .version = (uint32_t)version->valuedouble,
                .volume = (uint8_t)volume->valuedouble,
                .brightness = (uint8_t)brightness->valuedouble,
                .shake_sensitivity = (uint8_t)shake->valuedouble,
                .recording_timeout_seconds = (uint16_t)timeout->valuedouble,
                .animation_profile = animation_profile,
                .speech_profile = speech_profile,
                .has_speech_mouth_offset = mouth_offset != NULL,
                .speech_mouth_offset_ms = speech_mouth_offset_ms,
            };
            strlcpy(next.fingerprint, fingerprint->valuestring,
                    sizeof(next.fingerprint));
            strlcpy(next.ai_pet_id, ai_pet_id->valuestring,
                    sizeof(next.ai_pet_id));
            strlcpy(next.face_id, face_id->valuestring, sizeof(next.face_id));
            bool unchanged = next.version == s_config.settings_version &&
                !strcmp(next.fingerprint, s_config.config_fingerprint);
            if (!unchanged && !s_config_v2_pending && s_callbacks.config_v2_received) {
                s_pending_config_v2 = next;
                s_config_v2_pending = s_callbacks.config_v2_received(&next);
            } else if (unchanged) {
                pet_network_config_v2_result(&next, true, NULL, NULL);
            }
        } else if (cJSON_IsNumber(version)) {
            char rejection[192];
            snprintf(rejection, sizeof(rejection),
                     "{\"v\":1,\"type\":\"device.config.rejected\",\"version\":%.0f,\"code\":\"CONFIG_INVALID\"%s}",
                     version->valuedouble, mouth_offset_valid ? "" : ",\"field\":\"speechMouthOffsetMs\"");
            send_text(rejection);
        }
    } else if (!strcmp(type->valuestring, "pet.settings")) {
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
        const cJSON *volume = settings ? cJSON_GetObjectItemCaseSensitive(settings, "volume") : NULL;
        const cJSON *timeout = settings ? cJSON_GetObjectItemCaseSensitive(settings, "recordingTimeoutSeconds") : NULL;
        const cJSON *ai_pet_id = settings ? cJSON_GetObjectItemCaseSensitive(settings, "aiPetId") : NULL;
        const cJSON *face_id = settings ?
            cJSON_GetObjectItemCaseSensitive(settings, "faceId") : NULL;
        const cJSON *legacy_voice = settings ? cJSON_GetObjectItemCaseSensitive(settings, "legacyVoice") : NULL;
        pet_voice_t voice = PET_VOICE_COUNT;
        if (cJSON_IsString(legacy_voice)) {
            for (pet_voice_t candidate = PET_VOICE_PUCK; candidate < PET_VOICE_COUNT; ++candidate) {
                if (!strcmp(legacy_voice->valuestring, pet_voice_name(candidate))) { voice = candidate; break; }
            }
        }
        if (cJSON_IsNumber(version) && version->valuedouble >= 1 && cJSON_IsNumber(volume) && volume->valuedouble >= 0 && volume->valuedouble <= 100 &&
            cJSON_IsNumber(timeout) && pet_config_recording_timeout_valid((uint16_t)timeout->valuedouble) && cJSON_IsString(ai_pet_id) && pet_config_ai_pet_id_valid(ai_pet_id->valuestring) &&
            cJSON_IsString(face_id) && pet_face_id_valid(face_id->valuestring) &&
            voice < PET_VOICE_COUNT) {
            if (!installed_face_id(face_id->valuestring)) {
                /* Reject a catalog mismatch explicitly. Otherwise the gateway
                 * can believe a face is synchronized while this device keeps
                 * rendering the previous resident. */
                if (s_synced_settings_pending) {
                    s_settings_refresh_after_pending = true;
                } else {
                    refresh_durable_settings();
                }
                goto done;
            }
            pet_synced_settings_t synced = { .version = (uint32_t)version->valuedouble, .volume = (uint8_t)volume->valuedouble,
                .recording_timeout_seconds = (uint16_t)timeout->valuedouble,
                .legacy_voice = voice };
            strlcpy(synced.face_id, face_id->valuestring,
                    sizeof(synced.face_id));
            strlcpy(synced.ai_pet_id, ai_pet_id->valuestring, sizeof(synced.ai_pet_id));
            bool unchanged = synced.version == s_config.settings_version &&
                synced.volume == s_config.volume &&
                synced.recording_timeout_seconds == s_config.recording_timeout_seconds &&
                !strcmp(synced.face_id, s_config.face_id) &&
                synced.legacy_voice == s_config.voice &&
                !strcmp(synced.ai_pet_id, s_ai_pet_id);
            if (unchanged || (s_synced_settings_pending &&
                              synced_settings_equal(
                                  &synced, &s_pending_synced_settings))) goto done;
            /* Keep the durable config authoritative while the application
             * applies this transaction. Coalesce rapid publishes to the
             * newest version and replay it immediately after the in-flight
             * transaction is acknowledged or rejected. */
            if (s_synced_settings_pending) {
                if (synced.version > s_pending_synced_settings.version &&
                    (!s_synced_settings_queued ||
                     synced.version >= s_queued_synced_settings.version)) {
                    s_queued_synced_settings = synced;
                    s_synced_settings_queued = true;
                } else if (synced.version == s_pending_synced_settings.version &&
                           !synced_settings_equal(
                               &synced, &s_pending_synced_settings)) {
                    s_settings_refresh_after_pending = true;
                }
                goto done;
            }
            if (!dispatch_synced_settings(&synced)) refresh_durable_settings();
        }
    } else if(!strcmp(type->valuestring,"pet.speech.profile")||!strcmp(type->valuestring,"pet.stt.preference")){
        const bool legacy=!strcmp(type->valuestring,"pet.stt.preference");const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"version");const cJSON *profile_json=cJSON_GetObjectItemCaseSensitive(root,legacy?"mode":"profile");const cJSON *ai_pet_id=cJSON_GetObjectItemCaseSensitive(root,"aiPetId");const cJSON *available=cJSON_GetObjectItemCaseSensitive(root,legacy?"availableModes":"availableProfiles");pet_speech_profile_t profile;bool allow_cartesia_batch=false,allow_cartesia_realtime=false,allow_fish=false;
        const cJSON *item=NULL;if(cJSON_IsArray(available))cJSON_ArrayForEach(item,available){if(cJSON_IsString(item)){pet_speech_profile_t candidate;if(parse_speech_profile(item->valuestring,&candidate)){if(candidate==PET_SPEECH_PROFILE_CARTESIA_BATCH)allow_cartesia_batch=true;else if(candidate==PET_SPEECH_PROFILE_CARTESIA_REALTIME)allow_cartesia_realtime=true;else if(candidate==PET_SPEECH_PROFILE_FISH_DIRECT)allow_fish=true;}}}
        if(cJSON_IsNumber(version)&&version->valuedouble>=1&&cJSON_IsString(profile_json)&&parse_speech_profile(profile_json->valuestring,&profile)&&cJSON_IsString(ai_pet_id)&&pet_config_ai_pet_id_valid(ai_pet_id->valuestring)&&((profile==PET_SPEECH_PROFILE_CARTESIA_BATCH&&allow_cartesia_batch)||(profile==PET_SPEECH_PROFILE_CARTESIA_REALTIME&&allow_cartesia_realtime)||(profile==PET_SPEECH_PROFILE_FISH_DIRECT&&allow_fish))){
            pet_speech_preference_t preference={.version=(uint32_t)version->valuedouble,.profile=profile,.allow_cartesia_batch=allow_cartesia_batch,.allow_cartesia_realtime=allow_cartesia_realtime,.allow_fish=allow_fish};strlcpy(preference.ai_pet_id,ai_pet_id->valuestring,sizeof(preference.ai_pet_id));
            if(!s_speech_preference_pending&&s_callbacks.speech_preference_received){s_pending_speech_preference=preference;s_speech_preference_pending=s_callbacks.speech_preference_received(&preference);}
        }
    } else if (!strcmp(type->valuestring, "pet.ota.update")) {
        const cJSON *release_id = cJSON_GetObjectItemCaseSensitive(root, "releaseId");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
        const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(root, "bytes");
        const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
        pet_ota_request_t request = {0};
        bool valid = cJSON_IsString(release_id) && strlen(release_id->valuestring) == 64 &&
            cJSON_IsString(version) && strlen(version->valuestring) < sizeof(request.version) &&
            cJSON_IsString(url) && strlen(url->valuestring) < sizeof(request.url) &&
            cJSON_IsString(token) && strlen(token->valuestring) < sizeof(request.token) &&
            cJSON_IsNumber(bytes) && bytes->valuedouble >= 1024 && bytes->valuedouble <= 0x620000 &&
            cJSON_IsString(sha256) && strlen(sha256->valuestring) == 64;
        if (valid) {
            strlcpy(request.release_id, release_id->valuestring, sizeof(request.release_id));
            strlcpy(request.version, version->valuestring, sizeof(request.version));
            strlcpy(request.url, url->valuestring, sizeof(request.url));
            strlcpy(request.token, token->valuestring, sizeof(request.token));
            strlcpy(request.sha256, sha256->valuestring, sizeof(request.sha256));
            request.bytes = (size_t)bytes->valuedouble;
            if ((double)request.bytes != bytes->valuedouble || !s_callbacks.ota_update ||
                s_callbacks.ota_update(&request) != ESP_OK)
                pet_network_ota_status(request.release_id, "failed", 0, "OTA_BUSY_OR_REJECTED");
            memset(request.token, 0, sizeof(request.token));
        }
    } else if (!strcmp(type->valuestring, "device.wifi.command")) {
        const cJSON *operation = cJSON_GetObjectItemCaseSensitive(root, "operationId");
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        const cJSON *priority = cJSON_GetObjectItemCaseSensitive(root, "priority");
        pet_wifi_admin_action_t parsed_action = PET_WIFI_ADMIN_ADD;
        bool action_valid = cJSON_IsString(action);
        if (action_valid && !strcmp(action->valuestring, "add")) parsed_action = PET_WIFI_ADMIN_ADD;
        else if (action_valid && !strcmp(action->valuestring, "update")) parsed_action = PET_WIFI_ADMIN_UPDATE;
        else if (action_valid && !strcmp(action->valuestring, "connect")) parsed_action = PET_WIFI_ADMIN_CONNECT;
        else if (action_valid && !strcmp(action->valuestring, "forget")) parsed_action = PET_WIFI_ADMIN_FORGET;
        else if (action_valid && !strcmp(action->valuestring, "priority")) parsed_action = PET_WIFI_ADMIN_PRIORITY;
        else action_valid = false;
        bool password_required = parsed_action == PET_WIFI_ADMIN_ADD ||
                                 parsed_action == PET_WIFI_ADMIN_UPDATE;
        if (cJSON_IsString(operation) && strlen(operation->valuestring) == 36 &&
            action_valid && cJSON_IsString(ssid) && ssid->valuestring[0] &&
            strlen(ssid->valuestring) < PET_SSID_MAX &&
            (!password_required || (cJSON_IsString(password) &&
             strlen(password->valuestring) >= 8 &&
             strlen(password->valuestring) < PET_PASSWORD_MAX)) &&
            s_callbacks.wifi_admin_received) {
            pet_wifi_admin_command_t command = {.action=parsed_action};
            strlcpy(command.operation_id, operation->valuestring,
                    sizeof(command.operation_id));
            strlcpy(command.ssid, ssid->valuestring, sizeof(command.ssid));
            if (cJSON_IsString(password))
                strlcpy(command.password, password->valuestring,
                        sizeof(command.password));
            if (cJSON_IsNumber(priority) && priority->valuedouble >= 0 &&
                priority->valuedouble <= 100)
                command.priority = (uint8_t)priority->valuedouble;
            if (!s_callbacks.wifi_admin_received(&command))
                pet_network_wifi_admin_result(command.operation_id,command.ssid,
                                              "failed","QUEUE_FULL",command.priority);
            memset(command.password, 0, sizeof(command.password));
        }
    } else if (!strcmp(type->valuestring, "pet.credential.prepare")) {
        const cJSON *operation=cJSON_GetObjectItemCaseSensitive(root,"operationId");
        const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"credentialVersion");
        const cJSON *credential=cJSON_GetObjectItemCaseSensitive(root,"credential");
        if(cJSON_IsString(operation)&&strlen(operation->valuestring)==36&&
           cJSON_IsNumber(version)&&version->valuedouble>=1&&
           cJSON_IsString(credential)&&strlen(credential->valuestring)>=32&&
           strlen(credential->valuestring)<PET_TOKEN_MAX&&
           s_callbacks.credential_prepare_received){
            pet_credential_prepare_t command={.version=(uint32_t)version->valuedouble};
            strlcpy(command.operation_id,operation->valuestring,sizeof(command.operation_id));
            strlcpy(command.credential,credential->valuestring,sizeof(command.credential));
            s_callbacks.credential_prepare_received(&command);
            memset(command.credential,0,sizeof(command.credential));
        }
    } else if (!strcmp(type->valuestring, "pet.credential.commit")) {
        const cJSON *operation=cJSON_GetObjectItemCaseSensitive(root,"operationId");
        const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"credentialVersion");
        if(cJSON_IsString(operation)&&strlen(operation->valuestring)==36&&
           cJSON_IsNumber(version)&&version->valuedouble>=1&&
           s_callbacks.credential_commit_received){
            pet_credential_commit_t command={.version=(uint32_t)version->valuedouble};
            strlcpy(command.operation_id,operation->valuestring,sizeof(command.operation_id));
             s_callbacks.credential_commit_received(&command);
        }
    } else if (!strcmp(type->valuestring, "pet.crash.acknowledged")) {
        /* The cloud stored this crash summary. Diagnostics only schedules the
         * erase on an exact match; the telemetry task performs it. */
        const cJSON *elf_sha = cJSON_GetObjectItemCaseSensitive(root, "crashElfSha");
        const cJSON *pc = cJSON_GetObjectItemCaseSensitive(root, "crashPc");
        if (cJSON_IsString(elf_sha) && cJSON_IsNumber(pc) && pc->valuedouble >= 0 &&
            pc->valuedouble <= UINT32_MAX &&
            pc->valuedouble == (double)(uint32_t)pc->valuedouble) {
            pet_diagnostics_acknowledge_crash(elf_sha->valuestring,
                                              (uint32_t)pc->valuedouble);
        } else {
            ESP_LOGW(TAG, "ignored malformed crash acknowledgement");
        }
    } else if (!strcmp(type->valuestring, "pet.state")) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state) && s_callbacks.state) s_callbacks.state(state->valuestring);
    } else if (!strcmp(type->valuestring, "pet.expression")) {
        const cJSON *expression = cJSON_GetObjectItemCaseSensitive(root, "expression");
        if (cJSON_IsString(expression) && s_callbacks.expression) s_callbacks.expression(expression->valuestring);
    } else if (!strcmp(type->valuestring, "pet.gesture")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "gesture");
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(root, "sequence");
        uint8_t gesture = FC_GESTURE_NONE;
        if (cJSON_IsString(name)) {
            if (!strcmp(name->valuestring, "spin_cw")) gesture = FC_GESTURE_SPIN_CW;
            else if (!strcmp(name->valuestring, "spin_ccw")) gesture = FC_GESTURE_SPIN_CCW;
            else if (!strcmp(name->valuestring, "shake")) gesture = FC_GESTURE_SHAKE;
            else if (!strcmp(name->valuestring, "nod")) gesture = FC_GESTURE_NOD;
            else if (!strcmp(name->valuestring, "zoom_in")) gesture = FC_GESTURE_ZOOM_IN;
            else if (!strcmp(name->valuestring, "heartbeat")) gesture = FC_GESTURE_HEARTBEAT;
            else if (!strcmp(name->valuestring, "bounce")) gesture = FC_GESTURE_BOUNCE;
            else if (!strcmp(name->valuestring, "wobble")) gesture = FC_GESTURE_WOBBLE;
            else if (!strcmp(name->valuestring, "pop")) gesture = FC_GESTURE_POP;
        }
        if (gesture != FC_GESTURE_NONE && cJSON_IsNumber(sequence) &&
            sequence->valuedouble >= 0 && sequence->valuedouble <= UINT32_MAX) {
            uint32_t current = (uint32_t)sequence->valuedouble;
            if (sequence->valuedouble == (double)current &&
                (!s_gesture_sequence_valid || current > s_last_gesture_sequence)) {
                s_gesture_sequence_valid = true;
                s_last_gesture_sequence = current;
                if (s_callbacks.gesture) s_callbacks.gesture(gesture);
            }
        }
    } else if (!strcmp(type->valuestring, "output.audio.start")) {
        uint32_t stream, rate;
        bool adpcm = false;
        bool accepted = pet_session_wire_audio_start(root, &stream, &rate, &adpcm);
        if (accepted && adpcm && !s_speech_pcm) s_speech_pcm = malloc(SPEECH_ADPCM_MAX_SAMPLES * sizeof(int16_t));
        if (accepted && (!adpcm || s_speech_pcm) && s_callbacks.audio_start) {
            s_speech_adpcm = adpcm;
            s_speech_stream = stream;
            s_callbacks.audio_start(stream, rate);
        } else if (s_bound_session && s_callbacks.session_error)
            s_callbacks.session_error("AUDIO_FORMAT_UNSUPPORTED", "Unsupported Brain audio", false);
    } else if (!strcmp(type->valuestring, "output.audio.end")) {
        const cJSON *stream = cJSON_GetObjectItemCaseSensitive(root, "streamId");
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(root, "finalSequence");
        if (cJSON_IsNumber(stream) && cJSON_IsNumber(sequence) && s_callbacks.audio_end) s_callbacks.audio_end((uint32_t)stream->valuedouble, (uint32_t)sequence->valuedouble);
    } else if (!strcmp(type->valuestring, "session.error")) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        const cJSON *recoverable = cJSON_GetObjectItemCaseSensitive(root, "recoverable");
        ESP_LOGW(TAG, "session.error code=%s recoverable=%d", cJSON_IsString(code) ? code->valuestring : "?", cJSON_IsTrue(recoverable));
        /* A refused rebind: the gateway closes this socket next. */
        if (cJSON_IsString(code) && !strcmp(code->valuestring, PET_SESSION_REBIND_REFUSED)) atomic_store(&s_rebind_pending, false);
        if (cJSON_IsString(code) && cJSON_IsString(message) && s_callbacks.session_error) s_callbacks.session_error(code->valuestring, message->valuestring, cJSON_IsTrue(recoverable));
    } else if (!strcmp(type->valuestring, "ping")) {
        const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(root, "nonce");
        if (cJSON_IsNumber(nonce)) {
            char pong[96];
            snprintf(pong, sizeof(pong), "{\"v\":1,\"type\":\"pong\",\"nonce\":%.0f}", nonce->valuedouble);
            send_text(pong);
        }
    }
done:
    cJSON_Delete(root);
}

static void process_binary(const uint8_t *data, size_t length)
{
    pet_audio_frame_t frame;
    if (pet_protocol_decode_audio(data, length, &frame) != ESP_OK || frame.kind != PET_AUDIO_KIND_SPEAKER) return;
    if (!s_callbacks.audio_chunk) return;
    if (!s_speech_adpcm || frame.stream_id != s_speech_stream) {
        s_callbacks.audio_chunk(frame.stream_id, frame.sequence, frame.pcm, frame.pcm_length);
        return;
    }
    /* A malformed frame decodes to nothing, which the speaker refuses. */
    size_t samples = pet_ima_adpcm_decode(frame.pcm, frame.pcm_length, s_speech_pcm, SPEECH_ADPCM_MAX_SAMPLES);
    s_callbacks.audio_chunk(frame.stream_id, frame.sequence, (const uint8_t *)s_speech_pcm, samples * sizeof(int16_t));
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *event = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "gateway socket connected");
        s_gateway_rebind = false;
        s_gateway_story = false;
        send_hello();
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_websocket_ready = false;
        s_gateway_rebind = false;
        s_gateway_story = false;
        atomic_store(&s_rebind_pending, false);
        if (s_callbacks.connected) s_callbacks.connected(false);
    } else if (event_id == WEBSOCKET_EVENT_DATA && event->payload_len <= RX_FRAME_MAX) {
        if (event->payload_offset == 0) {
            free(s_rx_frame);
            s_rx_frame = malloc(event->payload_len);
            s_rx_expected = event->payload_len;
            s_rx_opcode = event->op_code;
        }
        if (!s_rx_frame || event->payload_offset + event->data_len > s_rx_expected) return;
        memcpy(s_rx_frame + event->payload_offset, event->data_ptr, event->data_len);
        if (event->payload_offset + event->data_len == s_rx_expected) {
            if (s_rx_opcode == 0x1) process_control(s_rx_frame, s_rx_expected, false);
            else if (s_rx_opcode == 0x2) process_binary(s_rx_frame, s_rx_expected);
            free(s_rx_frame);
            s_rx_frame = NULL;
            s_rx_expected = 0;
        }
    }
}

/* The bound session goes only to the build's backend. */
static bool bound_config_ready(const pet_config_t *config)
{
    char host[PET_GATEWAY_MAX];
    uint16_t port;
    return config && pet_session_wire_origin(CONFIG_PET_VNEXT_CONTROL_ORIGIN, host, sizeof(host), &port) &&
        config->gateway_secure && config->gateway_port == port && !strcmp(config->gateway_host, host) &&
        pet_session_wire_brain_token(config->pairing_token);
}

static void create_websocket(bool bound)
{
    if (s_websocket || (bound?!bound_config_ready(&s_config):!pet_config_ready(&s_config))) return;
    char uri[160];
    snprintf(uri, sizeof(uri), "%s://%s:%u/v1/device", s_config.gateway_secure ? "wss" : "ws", s_config.gateway_host, s_config.gateway_port);
    esp_websocket_client_config_t config = {
        .uri = uri,
        .buffer_size = RX_FRAME_MAX,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 2000,
        /* A socket the network dropped without a word is found by its
         * unanswered pings, 30 s after the first instead of the client's
         * 120 s; the gateway answers every ping. On 1 Oct a dead one looked
         * idle for 80 s while nothing got through. */
        .pingpong_timeout_sec = 30,
        .disable_auto_reconnect = bound,
        /* The client's 4 KiB default overflows when TLS encrypts the hello this
         * task sends from the connected event (esp_aes_dma_start), so every
         * reconnect could reboot the device. */
        .task_stack = 8192,
        .crt_bundle_attach = s_config.gateway_secure ? esp_crt_bundle_attach : NULL,
    };
    s_websocket = esp_websocket_client_init(&config);
    if (!s_websocket) return;
    if(esp_websocket_register_events(s_websocket,WEBSOCKET_EVENT_ANY,websocket_event,NULL)!=ESP_OK||
       esp_websocket_client_start(s_websocket)!=ESP_OK) {
        s_websocket_ready=false;
        xSemaphoreTake(s_send_lock,portMAX_DELAY);
        esp_websocket_client_destroy(s_websocket);s_websocket=NULL;
        atomic_fetch_add(&s_socket_generation,1);
        xSemaphoreGive(s_send_lock);
    }
}

static void start_websocket(void)
{
    /* Only the control worker can admit a bound socket after fresh context. */
    if(s_legacy_gateway_enabled)create_websocket(false);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_config.wifi_ssid[0]) connect_wifi_profile(false);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        s_websocket_ready = false;
        connected_callback_t callback=atomic_load(&s_wifi_connected_callback);
        if(callback)callback(false);
        /* A scan requested while the station is still connecting temporarily
         * suspends reconnects. The scan completion path resumes the current
         * profile without mutating either stored Wi-Fi credential. */
        if (s_scan_suspended_reconnect) return;
        if (!s_config.wifi_ssid[0]) return;
        s_wifi_retry_count++;
        if (s_config.wifi_fallback_ssid[0] &&
            s_wifi_retry_count >= WIFI_RETRIES_PER_PROFILE) {
            s_wifi_retry_count = 0;
            if (connect_wifi_profile(!s_wifi_using_fallback) != ESP_OK) {
                esp_wifi_connect();
            }
        } else {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        /* Wi-Fi events run on ESP-IDF's small sys_evt stack. Collecting scan
         * records and updating the LVGL dropdown there previously overflowed
         * that task. Keep the system callback constant-time and move all scan
         * processing to the dedicated worker. */
        if (s_scan_task) xTaskNotifyGive(s_scan_task);
        else {
            ESP_LOGE(TAG, "Wi-Fi scan worker unavailable");
            s_scan_running = false;
            s_scan_callback = NULL;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        start_websocket();
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    audio_tx_item_t item;
    for (;;) {
        if (xQueueReceive(s_audio_tx, &item, portMAX_DELAY) == pdTRUE && s_websocket_ready) {
            xSemaphoreTake(s_send_lock, portMAX_DELAY);
            int written = !s_websocket||!s_websocket_ready||item.generation!=atomic_load(&s_socket_generation)?-1:
                item.text?esp_websocket_client_send_text(s_websocket, (const char *)item.data, item.length, WS_SEND_TIMEOUT):
                esp_websocket_client_send_bin(s_websocket, (const char *)item.data, item.length, WS_SEND_TIMEOUT);
            xSemaphoreGive(s_send_lock);
            if (written < 0 && item.text) ESP_LOGW(TAG, "stream control message not sent");
            else if (written < 0) s_dropped_chunks++;
        }
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;
    uint32_t nonce = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        /* An acknowledged core dump is erased here, where blocking on flash
         * stalls neither the websocket nor the health checks. The report that
         * follows then shows the cleared summary. */
        pet_diagnostics_clear_acknowledged_crash();
        if (!s_websocket_ready && !s_bound_session) continue;
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        pet_diagnostics_snapshot_t snapshot;
        pet_diagnostics_get_snapshot(&snapshot);
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            ESP_LOGE(TAG, "could not allocate telemetry object");
            continue;
        }
        cJSON_AddNumberToObject(root, "v", PET_PROTOCOL_VERSION);
        cJSON_AddStringToObject(root, "type", "device.telemetry");
        cJSON_AddNumberToObject(root, "uptimeMs", esp_timer_get_time() / 1000);
        cJSON_AddNumberToObject(root, "freeHeap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(root, "freePsram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddNumberToObject(root, "droppedAudioChunks", s_dropped_chunks);
        cJSON_AddNumberToObject(root, "resetReason", esp_reset_reason());
        cJSON *health = cJSON_AddObjectToObject(root, "health");
        cJSON_AddStringToObject(health, "bootId", snapshot.boot_id);
        cJSON_AddStringToObject(health, "state", pet_diagnostics_state_name(snapshot.state));
        cJSON_AddNumberToObject(health, "stateAgeMs", snapshot.state_age_ms);
        cJSON_AddNumberToObject(health, "lastEvent", snapshot.last_event);
        cJSON_AddStringToObject(health, "lastEventName", snapshot.last_event_name);
        cJSON_AddNumberToObject(health, "lastEventAgeMs", snapshot.last_event_age_ms);
        cJSON_AddNumberToObject(health, "inputStreamId", snapshot.input_stream_id);
        cJSON_AddNumberToObject(health, "outputStreamId", snapshot.output_stream_id);
        cJSON_AddBoolToObject(health, "capturing", snapshot.capturing);
        cJSON_AddBoolToObject(health, "playing", snapshot.playing);
        cJSON_AddNumberToObject(health, "appQueueDepth", snapshot.app_queue_depth);
        cJSON_AddNumberToObject(health, "droppedAppEvents", snapshot.dropped_app_events);
        cJSON_AddNumberToObject(health, "minFreeHeap", snapshot.min_free_heap);
        cJSON_AddNumberToObject(health, "minFreeInternalHeap", snapshot.min_free_internal_heap);
        cJSON_AddNumberToObject(health, "minFreePsram", snapshot.min_free_psram);
        cJSON_AddNumberToObject(health, "largestInternalBlock", snapshot.largest_internal_block);
        cJSON_AddNumberToObject(health, "largestPsramBlock", snapshot.largest_psram_block);
        cJSON_AddNumberToObject(health, "appStackFree", snapshot.app_stack_free);
        cJSON_AddNumberToObject(health, "captureStackFree", snapshot.capture_stack_free);
        cJSON_AddNumberToObject(health, "playbackStackFree", snapshot.playback_stack_free);
        cJSON_AddNumberToObject(health, "audioTxStackFree", snapshot.audio_tx_stack_free);
        cJSON_AddNumberToObject(health, "telemetryStackFree", snapshot.telemetry_stack_free);
        cJSON_AddNumberToObject(health, "lvglStackFree", snapshot.lvgl_stack_free);
        cJSON_AddNumberToObject(health, "websocketStackFree", snapshot.websocket_stack_free);
        cJSON_AddNumberToObject(health, "displayHeartbeatAgeMs", snapshot.display_heartbeat_age_ms);
        cJSON_AddBoolToObject(health, "coredumpPresent", snapshot.coredump_present);
        if (s_battery_snapshot.valid) {
            cJSON_AddNumberToObject(health, "batteryPercent",
                                    s_battery_snapshot.percent);
            cJSON_AddStringToObject(health, "batteryState",
                                    battery_state_name(s_battery_snapshot.state));
            if (s_battery_snapshot.eta_valid)
                cJSON_AddNumberToObject(health, "batteryEtaMinutes",
                                        s_battery_snapshot.eta_minutes);
        }
        if (snapshot.coredump_present) {
            cJSON_AddStringToObject(health, "panicReason", snapshot.panic_reason);
            cJSON_AddStringToObject(health, "crashTask", snapshot.crash_task);
            cJSON_AddStringToObject(health, "crashElfSha", snapshot.crash_elf_sha);
            cJSON_AddNumberToObject(health, "crashPc", snapshot.crash_pc);
            cJSON_AddNumberToObject(health, "crashExceptionCause", snapshot.crash_exception_cause);
            cJSON_AddNumberToObject(health, "crashExceptionAddress", snapshot.crash_exception_address);
            cJSON_AddBoolToObject(health, "crashBacktraceCorrupted",
                                  snapshot.crash_backtrace_corrupted);
            cJSON *backtrace = cJSON_AddArrayToObject(health, "crashBacktrace");
            for (uint8_t i = 0; i < snapshot.crash_backtrace_depth; ++i) {
                cJSON_AddItemToArray(backtrace,
                                     cJSON_CreateNumber(snapshot.crash_backtrace[i]));
            }
        }
        char *json = cJSON_PrintUnformatted(root);
        if (json) {
            send_text(json);
            free(json);
        }
        cJSON_Delete(root);
        ESP_LOGI(TAG,
                 "HEALTH bootId=%s state=%s stateAgeMs=%lu capturing=%d playing=%d "
                 "queueDepth=%lu droppedEvents=%lu minHeap=%lu minInternal=%lu "
                 "minPsram=%lu displayHeartbeatAgeMs=%lu coredump=%d",
                 snapshot.boot_id, pet_diagnostics_state_name(snapshot.state),
                 (unsigned long)snapshot.state_age_ms, snapshot.capturing, snapshot.playing,
                 (unsigned long)snapshot.app_queue_depth,
                 (unsigned long)snapshot.dropped_app_events,
                 (unsigned long)snapshot.min_free_heap,
                 (unsigned long)snapshot.min_free_internal_heap,
                 (unsigned long)snapshot.min_free_psram,
                 (unsigned long)snapshot.display_heartbeat_age_ms,
                 snapshot.coredump_present);
        char ping[96];
        snprintf(ping, sizeof(ping), "{\"v\":1,\"type\":\"ping\",\"nonce\":%lu}",
                 (unsigned long)++nonce);
        send_text(ping);
    }
}

static esp_err_t start_network(const pet_config_t *config,
                               const pet_network_callbacks_t *callbacks,
                               bool legacy_gateway)
{
    if (!config || !callbacks) return ESP_ERR_INVALID_ARG;
    if (s_events) return ESP_ERR_INVALID_STATE;
    s_legacy_gateway_enabled = legacy_gateway;
    s_config = *config;
    s_callbacks = *callbacks;
    atomic_store(&s_wifi_connected_callback,callbacks->connected);
    s_synced_settings_pending = false;
    memset(&s_pending_synced_settings, 0, sizeof(s_pending_synced_settings));
    s_synced_settings_queued = false;
    memset(&s_queued_synced_settings, 0, sizeof(s_queued_synced_settings));
    s_settings_refresh_after_pending = false;
    s_speech_preference_pending=false;memset(&s_pending_speech_preference,0,sizeof(s_pending_speech_preference));
    s_wifi_retry_count = 0;
    s_wifi_using_fallback = false;
    strlcpy(s_ai_pet_id, config->ai_pet_id, sizeof(s_ai_pet_id));
    s_events = xEventGroupCreate();
    s_audio_tx = xQueueCreateWithCaps(AUDIO_TX_QUEUE_DEPTH, sizeof(audio_tx_item_t), MALLOC_CAP_SPIRAM);
    s_send_lock = xSemaphoreCreateMutex();
    s_rebind_lock = xSemaphoreCreateMutex();
    s_rebind_context = heap_caps_calloc(1, sizeof(*s_rebind_context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_events || !s_audio_tx || !s_send_lock || !s_rebind_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(wifi_scan_task, "pet_wifi_scan",
                    WIFI_SCAN_TASK_STACK_BYTES, NULL, 4,
                    &s_scan_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* Every connect sets the profile from the pet's own settings
     * (connect_wifi_profile), so the driver keeps it in RAM. In flash, each
     * primary/fallback switch while a network is down wrote NVS every few
     * seconds. */
    esp_err_t storage = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (storage != ESP_OK) ESP_LOGW(TAG, "Wi-Fi profile stays in flash: %s", esp_err_to_name(storage));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem sleep holds each frame for up to a beacon interval, so streamed
     * voice backs up the send buffer until writes time out. */
    esp_err_t power_save = esp_wifi_set_ps(WIFI_PS_NONE);
    if (power_save != ESP_OK) ESP_LOGW(TAG, "Wi-Fi power save stays on: %s", esp_err_to_name(power_save));
    if (legacy_gateway) {
        s_tx_started=xTaskCreatePinnedToCore(tx_task, "pet_audio_tx", 4096, NULL, 6, NULL, 0)==pdPASS;
        s_telemetry_started=xTaskCreate(telemetry_task, "pet_telemetry", 4096, NULL, 2, NULL)==pdPASS;
    }
    return ESP_OK;
}

esp_err_t pet_network_start(const pet_config_t *config, const pet_network_callbacks_t *callbacks)
{
    return start_network(config, callbacks, true);
}

esp_err_t pet_network_start_wifi_only(const pet_config_t *config)
{
    const pet_network_callbacks_t callbacks = {0};
    return start_network(config, &callbacks, false);
}

bool pet_network_wifi_is_ready(void)
{
    return s_events && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t pet_network_start_management(const pet_network_callbacks_t *callbacks)
{
    if (!s_events || s_legacy_gateway_enabled || !callbacks) return ESP_ERR_INVALID_STATE;
    s_bound_session = true;
    s_callbacks.config_v2_received = callbacks->config_v2_received;
    s_callbacks.speech_preference_received = callbacks->speech_preference_received;
    if (!s_telemetry_started)
        s_telemetry_started = xTaskCreate(telemetry_task, "pet_telemetry", 4096, NULL, 2, NULL) == pdPASS;
    return s_telemetry_started ? ESP_OK : ESP_ERR_NO_MEM;
}

void pet_network_freeze_bound(void)
{
    if(!s_bound_session)return;
    s_websocket_ready=false;
    /* Stop waits for the event worker before its callback data can be changed.
     * Destruction is serialized with any queued microphone/text sender. */
    if(s_websocket)esp_websocket_client_stop(s_websocket);
    xSemaphoreTake(s_send_lock,portMAX_DELAY);
    if(s_websocket){esp_websocket_client_destroy(s_websocket);s_websocket=NULL;}
    /* An in-flight gateway.hello can restore readiness while stop drains the
     * event worker. Destroyed sockets never retain admission, even if no final
     * DISCONNECTED event was emitted. */
    s_websocket_ready=false;
    atomic_fetch_add(&s_socket_generation,1);
    xSemaphoreGive(s_send_lock);
    s_bound_hello_sent=false;
    s_gateway_rebind=false;s_gateway_story=false;atomic_store(&s_rebind_pending,false);
}

esp_err_t pet_network_start_bound(const pet_config_t *config,const pet_network_callbacks_t *callbacks,
                                   const char *device_id,const pet_control_context_t *context)
{
    if(!s_events||s_legacy_gateway_enabled||!config||!callbacks||!device_id||strlen(device_id)!=36||
       !context||!context->binding.assigned||strcmp(device_id,context->device_id)||
       !bound_config_ready(config)||!pet_network_wifi_is_ready())return ESP_ERR_INVALID_STATE;
    pet_network_freeze_bound();
    s_bound_session=true;s_bound_hello_sent=false;s_bound_context=*context;
    strcpy(s_bound_device_id,device_id);
    /* Do not race the system Wi-Fi event task by rewriting its credentials. */
    memcpy(&s_config.gateway_host,&config->gateway_host,sizeof(*config)-offsetof(pet_config_t,gateway_host));
    s_callbacks=*callbacks;atomic_store(&s_wifi_connected_callback,callbacks->connected);
    strlcpy(s_active_face_id,context->config.face_id,sizeof(s_active_face_id));
    memset(s_installed_face_ids,0,sizeof(s_installed_face_ids));
    strlcpy(s_installed_face_ids[0],context->config.face_id,sizeof(s_installed_face_ids[0]));
    s_installed_face_count=1;memset(&s_face_profiles,0,sizeof(s_face_profiles));
    s_config_v2_pending=false;s_speech_preference_pending=false;
    strlcpy(s_ai_pet_id,context->config.ai_pet_id,sizeof(s_ai_pet_id));
    if(!s_tx_started)s_tx_started=xTaskCreatePinnedToCore(tx_task,"pet_audio_tx",4096,NULL,6,NULL,0)==pdPASS;
    if(!s_telemetry_started)s_telemetry_started=xTaskCreate(telemetry_task,"pet_telemetry",4096,NULL,2,NULL)==pdPASS;
    if(!s_tx_started||!s_telemetry_started)return ESP_ERR_NO_MEM;
    create_websocket(true);return s_websocket?ESP_OK:ESP_FAIL;
}

void pet_network_stop(void)
{
    s_websocket_ready = false;
    if (s_events) xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
    if (s_websocket) {
        esp_websocket_client_stop(s_websocket);
        esp_websocket_client_destroy(s_websocket);
        s_websocket = NULL;
    }
    esp_wifi_stop();
}

/* Voice readiness: the session is open and not waiting for a rebind's answer. */
bool pet_network_is_ready(void) { return s_websocket_ready && !atomic_load(&s_rebind_pending); }

bool pet_network_bound_to(const pet_control_context_t *context)
{
    const pet_control_binding_t *a = &s_bound_context.binding, *b = context ? &context->binding : NULL;
    return b && s_bound_session && s_websocket && s_websocket_ready && !atomic_load(&s_rebind_pending) &&
        a->assigned && b->assigned && !strcmp(a->revision, b->revision) && !strcmp(a->relationship_id, b->relationship_id) &&
        !strcmp(a->build_id, b->build_id) && !strcmp(a->sha256, b->sha256) &&
        !strcmp(s_bound_context.config.version, context->config.version);
}

bool pet_network_can_rebind(void)
{
    return s_rebind_context && s_bound_session && s_websocket && s_websocket_ready && s_gateway_rebind &&
        !atomic_load(&s_rebind_pending);
}

esp_err_t pet_network_rebind(const pet_control_context_t *context)
{
    if (!context || !context->binding.assigned || strcmp(context->device_id, s_bound_device_id) || !pet_network_can_rebind())
        return ESP_ERR_NOT_SUPPORTED;
    char json[PET_SESSION_WIRE_REBIND_MAX];
    if (!pet_session_wire_brain_rebind(context, s_config.pairing_token, json, sizeof(json)))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_rebind_lock, portMAX_DELAY);
    *s_rebind_context = *context;
    atomic_store(&s_rebind_pending, true);
    xSemaphoreGive(s_rebind_lock);
    if (send_text(json) != ESP_OK) {
        atomic_store(&s_rebind_pending, false);
        ESP_LOGW(TAG, "rebind not sent");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "rebind sent for binding %s", context->binding.revision);
    return ESP_OK;
}

esp_err_t pet_network_scan(pet_network_scan_callback_t callback)
{
    if (!callback || !s_events) return ESP_ERR_INVALID_STATE;
    /* Opening settings starts a scan automatically. A quick tap on the Scan
     * button can arrive before that scan completes; treat it as the same
     * request instead of reporting that Wi-Fi scanning is unavailable. */
    if (s_scan_running) return ESP_OK;
    wifi_scan_config_t config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    s_scan_callback = callback;
    s_scan_running = true;
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err == ESP_ERR_WIFI_STATE) {
        /* ESP-IDF rejects scans while a station connection is in progress.
         * This is exactly when an offline pet needs discovery most, so pause
         * that connection briefly, start the scan, and reconnect afterwards. */
        s_scan_suspended_reconnect = true;
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
            err = disconnect_err;
        } else {
            for (uint8_t retry = 0; retry < WIFI_SCAN_START_RETRIES; ++retry) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_RETRY_DELAY_MS));
                err = esp_wifi_scan_start(&config, false);
                if (err != ESP_ERR_WIFI_STATE) break;
            }
        }
    }
    if (err != ESP_OK) {
        s_scan_running = false;
        s_scan_callback = NULL;
        if (s_scan_suspended_reconnect) {
            s_scan_suspended_reconnect = false;
            esp_wifi_connect();
        }
    }
    return err;
}

void pet_network_set_brain_token(const char *token)
{
    if (!pet_session_wire_brain_token(token) || !s_send_lock) return;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    strlcpy(s_config.pairing_token, token, sizeof(s_config.pairing_token));
    xSemaphoreGive(s_send_lock);
}

bool pet_network_management_request(char *out, size_t capacity)
{
    if (!out || capacity > 0x7fffffff || !s_send_lock) return false;
    cJSON *root = cJSON_CreateObject();
    cJSON *reports = root ? cJSON_AddArrayToObject(root, "reports") : NULL;
    if (!reports)
    {
        cJSON_Delete(root);
        return false;
    }
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    for (unsigned i = 0; i < 3; ++i)
    {
        if (!s_management_reports[i]) continue;
        cJSON *report = cJSON_Parse(s_management_reports[i]);
        if (report) cJSON_AddItemToArray(reports, report);
        free(s_management_reports[i]);
        s_management_reports[i] = NULL;
    }
    xSemaphoreGive(s_send_lock);
    bool ok = cJSON_PrintPreallocated(root, out, (int)capacity, false);
    cJSON_Delete(root);
    return ok;
}

bool pet_network_management_response(const char *json, size_t bytes)
{
    cJSON *root = pet_control_json(json, bytes, 8192);
    const cJSON *messages = root ? cJSON_GetObjectItemCaseSensitive(root, "messages") : NULL;
    bool ok = root && pet_control_version(root) && cJSON_IsArray(messages) && cJSON_GetArraySize(messages) <= 3;
    const cJSON *message = NULL;
    if (ok) cJSON_ArrayForEach(message, messages)
    {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "type");
        if (!cJSON_IsString(type) || !pet_session_wire_management_message(type->valuestring, true))
        {
            ok = false;
            break;
        }
    }
    if (ok) cJSON_ArrayForEach(message, messages)
    {
        char *wire = cJSON_PrintUnformatted(message);
        if (wire) process_control((const uint8_t *)wire, strlen(wire), true);
        free(wire);
    }
    cJSON_Delete(root);
    return ok;
}

/* input.audio.start, or input.story: a turn without the microphone. */
static esp_err_t send_turn_start(const char *type, uint32_t stream_id, const char *ai_pet_id, bool microphone)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", PET_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "streamId", stream_id);
    if (microphone) {
        cJSON *format = cJSON_AddObjectToObject(root, "format");
        cJSON_AddStringToObject(format, "encoding", "pcm_s16le");
        cJSON_AddNumberToObject(format, "sampleRate", 16000);
        cJSON_AddNumberToObject(format, "channels", 1);
        cJSON_AddNumberToObject(root, "startedAtMs", esp_timer_get_time() / 1000);
    }
    if (ai_pet_id && ai_pet_id[0])
        cJSON_AddStringToObject(root, "aiPetId", ai_pet_id);
    cJSON_AddStringToObject(root,"speechProfile",pet_speech_profile_name(s_config.speech_profile));
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t result = json ? send_text(json) : ESP_ERR_NO_MEM;
    free(json);
    cJSON_Delete(root);
    return result;
}

esp_err_t pet_network_input_start(uint32_t stream_id, const char *ai_pet_id)
{
    return send_turn_start("input.audio.start", stream_id, ai_pet_id, true);
}

bool pet_network_can_story(void) { return s_gateway_story && pet_network_is_ready(); }

esp_err_t pet_network_story(uint32_t stream_id, const char *ai_pet_id)
{
    if (!pet_network_can_story()) return ESP_ERR_INVALID_STATE;
    return send_turn_start("input.story", stream_id, ai_pet_id, false);
}

esp_err_t pet_network_send_microphone(uint32_t stream_id, uint32_t sequence,
                                      const int16_t *pcm, size_t samples)
{
    audio_tx_item_t item;
    item.generation=atomic_load(&s_socket_generation);
    /* A binary frame. Left uninitialised, stack garbage sent microphone audio as
     * WebSocket text, which the gateway rejects chunk by chunk (PIPELINE_ERROR). */
    item.text = false;
    item.length = pet_protocol_encode_audio(item.data, sizeof(item.data), PET_AUDIO_KIND_MICROPHONE, stream_id, sequence, pcm, samples);
    if (!item.length) return ESP_ERR_INVALID_ARG;
    if (xQueueSend(s_audio_tx, &item, 0) != pdTRUE) {
        s_dropped_chunks++;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Messages that close a microphone stream travel behind its queued audio.
 * An end that overtook its chunks failed the turn with AUDIO_SEQUENCE. */
static esp_err_t queue_stream_text(const char *json)
{
    if (!s_websocket_ready) return ESP_ERR_INVALID_STATE;
    audio_tx_item_t item = {.generation = atomic_load(&s_socket_generation), .text = true};
    item.length = strlcpy((char *)item.data, json, sizeof(item.data));
    if (item.length >= sizeof(item.data)) return ESP_ERR_INVALID_SIZE;
    if (xQueueSend(s_audio_tx, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "stream control queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t pet_network_input_end(uint32_t stream_id, uint32_t final_sequence, uint32_t duration_ms)
{
    char json[160];
    snprintf(json, sizeof(json), "{\"v\":1,\"type\":\"input.audio.end\",\"streamId\":%lu,\"finalSequence\":%lu,\"durationMs\":%lu}",
             (unsigned long)stream_id, (unsigned long)final_sequence, (unsigned long)duration_ms);
    return queue_stream_text(json);
}

esp_err_t pet_network_cancel(uint32_t stream_id)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"v\":1,\"type\":\"input.cancel\",\"streamId\":%lu}", (unsigned long)stream_id);
    return queue_stream_text(json);
}

uint32_t pet_network_dropped_chunks(void) { return s_dropped_chunks; }

esp_err_t pet_network_ota_status(const char *release_id, const char *status,
                                 unsigned progress, const char *error_code)
{
    if (!release_id || strlen(release_id) != 64 || !status) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", PET_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "device.ota.status");
    cJSON_AddStringToObject(root, "releaseId", release_id);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "progress", progress > 100 ? 100 : progress);
    if (error_code && error_code[0]) cJSON_AddStringToObject(root, "errorCode", error_code);
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = json ? send_text(json) : ESP_ERR_NO_MEM;
    free(json);
    cJSON_Delete(root);
    return err;
}
