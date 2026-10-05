#include "pet_config.h"
#include "pet_enrollment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pet_sfx.h"

static const char *TAG = "pet_config";
static void (*s_changed_callback)(void);
static const uint16_t s_recording_timeouts[PET_RECORDING_TIMEOUT_COUNT] = {
    8, 15, 30, 60, 180, 600,
};

enum {
    FACE_PROFILES_MAGIC = 0x46505231u, /* FPR1 */
    FACE_PROFILES_VERSION = 1,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    pet_face_profiles_t profiles;
} face_profiles_blob_t;

typedef struct {
    char ssid[PET_SSID_MAX];
    char password[PET_PASSWORD_MAX];
} saved_wifi_profile_t;

static void wifi_slot_key(char *target, size_t capacity, const char *prefix,
                          uint8_t index)
{
    snprintf(target, capacity, "%s%u", prefix, (unsigned)index);
}

static esp_err_t read_wifi_pair(nvs_handle_t nvs, const char *ssid_key,
                                const char *password_key, const char *ssid,
                                char *password, size_t capacity)
{
    char candidate_ssid[PET_SSID_MAX] = {0};
    size_t ssid_bytes = sizeof(candidate_ssid);
    esp_err_t err = nvs_get_str(nvs, ssid_key, candidate_ssid, &ssid_bytes);
    if (err != ESP_OK || strcmp(candidate_ssid, ssid)) return ESP_ERR_NVS_NOT_FOUND;
    size_t password_bytes = capacity;
    err = nvs_get_str(nvs, password_key, password, &password_bytes);
    return err == ESP_OK ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t load_saved_wifi(nvs_handle_t nvs,
                                 saved_wifi_profile_t *profiles,
                                 uint8_t *count)
{
    if (!profiles || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    uint8_t stored_count = 0;
    esp_err_t err = nvs_get_u8(nvs, "wifi_count", &stored_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (stored_count > PET_WIFI_PROFILE_MAX) stored_count = PET_WIFI_PROFILE_MAX;
    for (uint8_t i = 0; i < stored_count; ++i) {
        char ssid_key[16];
        char password_key[16];
        wifi_slot_key(ssid_key, sizeof(ssid_key), "wifi_ssid_", i);
        wifi_slot_key(password_key, sizeof(password_key), "wifi_pass_", i);
        size_t ssid_bytes = sizeof(profiles[*count].ssid);
        size_t password_bytes = sizeof(profiles[*count].password);
        if (nvs_get_str(nvs, ssid_key, profiles[*count].ssid, &ssid_bytes) != ESP_OK ||
            !profiles[*count].ssid[0] ||
            nvs_get_str(nvs, password_key, profiles[*count].password,
                        &password_bytes) != ESP_OK) {
            memset(&profiles[*count], 0, sizeof(profiles[*count]));
            continue;
        }
        (*count)++;
    }
    return ESP_OK;
}

static void promote_saved_wifi(saved_wifi_profile_t *profiles, uint8_t *count,
                               const char *ssid, const char *password)
{
    if (!profiles || !count || !ssid || !ssid[0] || !password) return;
    uint8_t existing = *count;
    for (uint8_t i = 0; i < *count; ++i) {
        if (!strcmp(profiles[i].ssid, ssid)) {
            existing = i;
            break;
        }
    }
    uint8_t shift = existing < *count ? existing : *count;
    if (shift >= PET_WIFI_PROFILE_MAX) shift = PET_WIFI_PROFILE_MAX - 1;
    for (uint8_t i = shift; i > 0; --i) profiles[i] = profiles[i - 1];
    strlcpy(profiles[0].ssid, ssid, sizeof(profiles[0].ssid));
    strlcpy(profiles[0].password, password, sizeof(profiles[0].password));
    if (existing == *count && *count < PET_WIFI_PROFILE_MAX) (*count)++;
}

static esp_err_t remember_wifi(nvs_handle_t nvs, const char *const *ssids,
                               const char *const *passwords, size_t pair_count)
{
    saved_wifi_profile_t *profiles = calloc(PET_WIFI_PROFILE_MAX,
                                             sizeof(*profiles));
    if (!profiles) return ESP_ERR_NO_MEM;
    uint8_t count = 0;
    esp_err_t err = load_saved_wifi(nvs, profiles, &count);
    for (size_t i = 0; err == ESP_OK && i < pair_count; ++i) {
        if (ssids[i] && ssids[i][0] && passwords[i]) {
            promote_saved_wifi(profiles, &count, ssids[i], passwords[i]);
        }
    }
    for (uint8_t i = 0; err == ESP_OK && i < count; ++i) {
        char ssid_key[16];
        char password_key[16];
        wifi_slot_key(ssid_key, sizeof(ssid_key), "wifi_ssid_", i);
        wifi_slot_key(password_key, sizeof(password_key), "wifi_pass_", i);
        err = nvs_set_str(nvs, ssid_key, profiles[i].ssid);
        if (err == ESP_OK) err = nvs_set_str(nvs, password_key, profiles[i].password);
    }
    for (uint8_t i = count; err == ESP_OK && i < PET_WIFI_PROFILE_MAX; ++i) {
        char ssid_key[16];
        char password_key[16];
        wifi_slot_key(ssid_key, sizeof(ssid_key), "wifi_ssid_", i);
        wifi_slot_key(password_key, sizeof(password_key), "wifi_pass_", i);
        esp_err_t erase_err = nvs_erase_key(nvs, ssid_key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) err = erase_err;
        erase_err = nvs_erase_key(nvs, password_key);
        if (err == ESP_OK && erase_err != ESP_OK &&
            erase_err != ESP_ERR_NVS_NOT_FOUND) err = erase_err;
    }
    if (err == ESP_OK) err = nvs_set_u8(nvs, "wifi_count", count);
    memset(profiles, 0, PET_WIFI_PROFILE_MAX * sizeof(*profiles));
    free(profiles);
    return err;
}

static bool face_profiles_valid(const pet_face_profiles_t *profiles)
{
    if (!profiles || profiles->count > PET_FACE_PROFILE_MAX) return false;
    for (uint8_t i = 0; i < profiles->count; ++i) {
        if (!pet_face_profile_valid(&profiles->entries[i])) return false;
        for (uint8_t j = 0; j < i; ++j) {
            if (!strcmp(profiles->entries[i].face_id,
                        profiles->entries[j].face_id)) return false;
        }
    }
    return true;
}

bool pet_config_ai_pet_id_valid(const char *id)
{
    if (!id || !id[0] || strlen(id) >= PET_AI_PET_ID_MAX) return false;
    if (!((id[0] >= 'a' && id[0] <= 'z') ||
          (id[0] >= '0' && id[0] <= '9'))) return false;
    for (const unsigned char *cursor = (const unsigned char *)id; *cursor;
         ++cursor) {
        if ((*cursor >= 'a' && *cursor <= 'z') ||
            (*cursor >= '0' && *cursor <= '9') || *cursor == '.' ||
            *cursor == '_' || *cursor == '-') continue;
        return false;
    }
    return true;
}

static esp_err_t read_string(nvs_handle_t nvs, const char *key, char *target, size_t capacity)
{
    size_t length = capacity;
    esp_err_t err = nvs_get_str(nvs, key, target, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        target[0] = 0;
        return ESP_OK;
    }
    return err;
}

esp_err_t pet_config_init(void)
{
    /* Never erase Wi-Fi, enrollment identity or ownership on an init error.
     * A full/version-incompatible NVS needs an explicit recovery. */
    return nvs_flash_init();
}

esp_err_t pet_config_load(pet_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    config->gateway_port = 8787;
    config->volume = 55;
    config->brightness = PET_BRIGHTNESS_DEFAULT;
    config->shake_sensitivity = PET_SHAKE_SENSITIVITY_DEFAULT;
    config->recording_timeout_seconds = PET_RECORDING_TIMEOUT_DEFAULT_SECONDS;
    config->animation_profile = PET_ANIMATION_PROFILE_DEFAULT;
    config->voice = PET_VOICE_PUCK;
    config->ai_mode = PET_AI_MODE_OPENROUTER;
    config->realtime_model = PET_REALTIME_MODEL_2_1_MINI;
    config->realtime_voice = PET_REALTIME_VOICE_CEDAR;
    config->realtime_boost = PET_REALTIME_BOOST_DEFAULT;
    config->cartesia_voice_gender = PET_CARTESIA_VOICE_NEUTRAL;
    config->speech_mouth_mode = PET_SPEECH_MOUTH_DEFAULT;
    config->speech_profile = PET_SPEECH_PROFILE_CARTESIA_BATCH;
    config->config_schema_version = 1;
    strlcpy(config->face_id, "pablo", sizeof(config->face_id));
    strlcpy(config->ai_pet_id, "pablo", sizeof(config->ai_pet_id));
    nvs_handle_t nvs = 0;
    esp_err_t open_err = nvs_open("aipet", NVS_READONLY, &nvs);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(open_err, TAG, "open NVS");
    esp_err_t err = read_string(nvs, "ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
    if (err == ESP_OK) err = read_string(nvs, "password", config->wifi_password, sizeof(config->wifi_password));
    if (err == ESP_OK) err = read_string(nvs, "ssid_alt", config->wifi_fallback_ssid, sizeof(config->wifi_fallback_ssid));
    if (err == ESP_OK) err = read_string(nvs, "password_alt", config->wifi_fallback_password, sizeof(config->wifi_fallback_password));
    if (err == ESP_OK) {
        size_t operation_bytes = sizeof(config->pending_wifi_operation_id);
        if (nvs_get_str(nvs, "wifi_op", config->pending_wifi_operation_id,
                        &operation_bytes) != ESP_OK)
            config->pending_wifi_operation_id[0] = '\0';
        size_t operation_ssid_bytes = sizeof(config->pending_wifi_ssid);
        if (nvs_get_str(nvs, "wifi_op_ssid", config->pending_wifi_ssid,
                        &operation_ssid_bytes) != ESP_OK)
            config->pending_wifi_ssid[0] = '\0';
    }
    if (err == ESP_OK) err = read_string(nvs, "gateway", config->gateway_host, sizeof(config->gateway_host));
    if (err == ESP_OK) err = read_string(nvs, "token", config->pairing_token, sizeof(config->pairing_token));
    if (err == ESP_OK) nvs_get_u32(nvs, "credver", &config->credential_version);
    if (err == ESP_OK) {
        size_t pending_token_bytes = sizeof(config->pending_credential_token);
        if (nvs_get_str(nvs, "pending_token", config->pending_credential_token,
                        &pending_token_bytes) != ESP_OK)
            config->pending_credential_token[0] = '\0';
        nvs_get_u32(nvs, "pending_ver", &config->pending_credential_version);
        size_t pending_operation_bytes = sizeof(config->pending_credential_operation_id);
        if (nvs_get_str(nvs, "pending_op",
                        config->pending_credential_operation_id,
                        &pending_operation_bytes) != ESP_OK)
            config->pending_credential_operation_id[0] = '\0';
    }
    if (err == ESP_OK) nvs_get_u32(nvs, "settings_ver", &config->settings_version);
    if (err == ESP_OK) {
        uint8_t schema = 0;
        if (nvs_get_u8(nvs, "config_schema", &schema) == ESP_OK && schema == 2)
            config->config_schema_version = schema;
        size_t fingerprint_bytes = sizeof(config->config_fingerprint);
        if (nvs_get_str(nvs, "config_fp", config->config_fingerprint,
                        &fingerprint_bytes) != ESP_OK)
            config->config_fingerprint[0] = '\0';
    }
    if (err == ESP_OK) nvs_get_u32(nvs, "stt_pref_ver", &config->speech_profile_version);
    uint16_t port = 0;
    if (err == ESP_OK && nvs_get_u16(nvs, "port", &port) == ESP_OK && port) config->gateway_port = port;
    uint8_t secure = 0;
    if (err == ESP_OK && nvs_get_u8(nvs, "secure", &secure) == ESP_OK) config->gateway_secure = secure != 0;
    uint8_t volume = 0;
    if (err == ESP_OK && nvs_get_u8(nvs, "volume", &volume) == ESP_OK && volume <= 100) config->volume = volume;
    uint8_t brightness = 0;
    if (err == ESP_OK && nvs_get_u8(nvs, "brightness", &brightness) == ESP_OK &&
        brightness >= PET_BRIGHTNESS_MIN && brightness <= 100) {
        config->brightness = brightness;
    }
    uint8_t shake_sensitivity = 0;
    if (err == ESP_OK && nvs_get_u8(nvs, "shake_sens", &shake_sensitivity) == ESP_OK &&
        shake_sensitivity <= 100) {
        config->shake_sensitivity = shake_sensitivity;
    }
    uint16_t recording_timeout = 0;
    if (err == ESP_OK && nvs_get_u16(nvs, "record_max_s", &recording_timeout) == ESP_OK &&
        pet_config_recording_timeout_valid(recording_timeout)) {
        config->recording_timeout_seconds = recording_timeout;
    }
    uint8_t animation_profile = PET_ANIMATION_PROFILE_COUNT;
    if (err == ESP_OK && nvs_get_u8(nvs, "anim_profile", &animation_profile) == ESP_OK &&
        pet_animation_profile_valid((pet_animation_profile_t)animation_profile)) {
        config->animation_profile = (pet_animation_profile_t)animation_profile;
    }
    char face_id[PET_FACE_ID_MAX] = {0};
    size_t face_id_bytes = sizeof(face_id);
    if (err == ESP_OK &&
        nvs_get_str(nvs, "face_id", face_id, &face_id_bytes) == ESP_OK &&
        pet_face_id_valid(face_id)) {
        strlcpy(config->face_id, face_id, sizeof(config->face_id));
    } else {
        /* Older firmware persisted a menu/physical-slot number. The retired
         * packs are incompatible, so migrate any legacy value to the embedded
         * catalog default; normal fallback chooses Pablo if needed. */
        uint8_t ignored_legacy_choice = 0;
        uint8_t ignored_legacy_slot = 0;
        nvs_get_u8(nvs, "face_choice", &ignored_legacy_choice);
        nvs_get_u8(nvs, "face_slot", &ignored_legacy_slot);
        config->has_legacy_face_selection = true;
    }
    char ai_pet_id[PET_AI_PET_ID_MAX] = {0};
    size_t ai_pet_id_bytes = sizeof(ai_pet_id);
    if (err == ESP_OK &&
        nvs_get_str(nvs, "ai_pet_id", ai_pet_id, &ai_pet_id_bytes) == ESP_OK &&
        pet_config_ai_pet_id_valid(ai_pet_id)) {
        strlcpy(config->ai_pet_id, ai_pet_id, sizeof(config->ai_pet_id));
    }
    uint8_t voice = PET_VOICE_PUCK;
    esp_err_t voice_err = err == ESP_OK ? nvs_get_u8(nvs, "voice", &voice) : err;
    if (voice_err == ESP_OK && voice < PET_VOICE_COUNT) {
        config->voice = (pet_voice_t)voice;
        config->has_legacy_ai_profile = true;
    }
    uint8_t ai_mode = PET_AI_MODE_OPENROUTER;
    esp_err_t ai_mode_err = err == ESP_OK ? nvs_get_u8(nvs, "ai_mode", &ai_mode) : err;
    if (ai_mode_err == ESP_OK && ai_mode < PET_AI_MODE_COUNT) {
        config->ai_mode = (pet_ai_mode_t)ai_mode;
        config->has_legacy_ai_profile = true;
    }
    uint8_t realtime_model = PET_REALTIME_MODEL_2_1_MINI;
    esp_err_t realtime_model_err = err == ESP_OK ? nvs_get_u8(nvs, "rt_model", &realtime_model) : err;
    if (realtime_model_err == ESP_OK && realtime_model < PET_REALTIME_MODEL_COUNT) {
        config->realtime_model = (pet_realtime_model_t)realtime_model;
        config->has_legacy_ai_profile = true;
    }
    uint8_t realtime_voice = PET_REALTIME_VOICE_CEDAR;
    esp_err_t realtime_voice_err = err == ESP_OK ? nvs_get_u8(nvs, "rt_voice", &realtime_voice) : err;
    if (realtime_voice_err == ESP_OK && realtime_voice < PET_REALTIME_VOICE_COUNT) {
        config->realtime_voice = (pet_realtime_voice_t)realtime_voice;
        config->has_legacy_ai_profile = true;
    }
    uint8_t realtime_boost = PET_REALTIME_BOOST_DEFAULT;
    if (err == ESP_OK && nvs_get_u8(nvs, "rt_boost", &realtime_boost) == ESP_OK &&
        realtime_boost < PET_REALTIME_BOOST_COUNT) {
        config->realtime_boost = (pet_realtime_boost_t)realtime_boost;
    }
    uint8_t cartesia_voice_gender = PET_CARTESIA_VOICE_NEUTRAL;
    esp_err_t cartesia_gender_err = err == ESP_OK ?
        nvs_get_u8(nvs, "cart_gender", &cartesia_voice_gender) : err;
    if (cartesia_gender_err == ESP_OK &&
        cartesia_voice_gender < PET_CARTESIA_VOICE_GENDER_COUNT) {
        config->cartesia_voice_gender = (pet_cartesia_voice_gender_t)cartesia_voice_gender;
        config->has_legacy_ai_profile = true;
    }
    face_profiles_blob_t blob = {0};
    size_t blob_size = sizeof(blob);
    if (err == ESP_OK && nvs_get_blob(nvs, "face_profiles", &blob, &blob_size) == ESP_OK &&
        blob_size == sizeof(blob) && blob.magic == FACE_PROFILES_MAGIC &&
        blob.version == FACE_PROFILES_VERSION && blob.bytes == sizeof(blob.profiles) &&
        face_profiles_valid(&blob.profiles)) {
        config->face_profiles = blob.profiles;
    }
    uint8_t speech_mouth_mode = PET_SPEECH_MOUTH_MODE_COUNT;
    if (err == ESP_OK && nvs_get_u8(nvs, "mouth_mode", &speech_mouth_mode) == ESP_OK &&
        speech_mouth_mode < PET_SPEECH_MOUTH_MODE_COUNT) {
        config->speech_mouth_mode = (pet_speech_mouth_mode_t)speech_mouth_mode;
    }
    int16_t mouth_offset = 0;
    if (err == ESP_OK && nvs_get_i16(nvs, "mouth_ofs_ms", &mouth_offset) == ESP_OK &&
        pet_speech_mouth_offset_valid(mouth_offset)) {
        config->has_speech_mouth_offset = true;
        config->speech_mouth_offset_ms = mouth_offset;
    }
    uint8_t speech_profile = PET_SPEECH_PROFILE_CARTESIA_BATCH;
    if (err == ESP_OK) {
        esp_err_t profile_err = nvs_get_u8(nvs, "speech_profile", &speech_profile);
        if (profile_err == ESP_ERR_NVS_NOT_FOUND) {
            /* Preserve the Cartesia selection made by firmware released before
             * speech profiles became provider-neutral. */
            profile_err = nvs_get_u8(nvs, "stt_mode", &speech_profile);
        }
        if (profile_err == ESP_OK && speech_profile < PET_SPEECH_PROFILE_COUNT) {
            config->speech_profile = (pet_speech_profile_t)speech_profile;
        }
    }
    nvs_close(nvs);
    return err;
}

bool pet_config_ready(const pet_config_t *config)
{
    return config && config->wifi_ssid[0] && config->gateway_host[0] && strlen(config->pairing_token) >= 16 && (!config->gateway_secure || config->credential_version > 0);
}

static void print_help(void)
{
    printf("\nAI Pet provisioning commands:\n"
           "  set ssid <Wi-Fi name>\n"
           "  set password <Wi-Fi password>\n"
           "  add-wifi <Wi-Fi name> <Wi-Fi password>\n"
           "  set gateway <PC IPv4 or hostname>\n"
           "  set port <1-65535>\n"
           "  set volume <0-100>\n"
           "  set brightness <10-100>\n"
           "  set shake-sensitivity <0-100>\n"
           "  set recording-timeout <8|15|30|60|180|600>\n"
           "  set animation <full|balanced|reduced>\n"
           "  set voice <Puck|Kore|Charon|Fenrir|Aoede|Leda>\n"
           "  set ai-mode <openrouter|openai-realtime|cartesia-agent>\n"
           "  set realtime-model <gpt-realtime-2.1-mini|gpt-realtime-2.1|gpt-realtime-1.5>\n"
           "  set realtime-voice <cedar|marin|alloy|ash|ballad|coral|echo|sage|shimmer|verse>\n"
           "  set realtime-boost <off|3|6|9>\n"
           "  set cartesia-gender <neutral|male|female>\n"
           "  set speech-mouth <0..5|current-sprites|full-range-sprites|synced-sprites|smooth-24-step|classic-shape|expressive-shape>\n"
           "  set secure <on|off>\n"
           "  set token <gateway pairing token>\n"
           "  set credential-version <positive number>\n"
           "  sound deployment-complete\n"
           "  show | clear | restart | help\n> ");
    fflush(stdout);
}

static char *trim(char *value)
{
    while (*value == ' ' || *value == '\t') value++;
    size_t length = strlen(value);
    while (length && (value[length - 1] == '\r' || value[length - 1] == '\n' || value[length - 1] == ' ' || value[length - 1] == '\t')) value[--length] = 0;
    return value;
}

static esp_err_t store_value(const char *key, const char *value)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_recall_wifi(const char *ssid, char *password,
                                 size_t capacity)
{
    if (!ssid || !ssid[0] || !password || !capacity ||
        strlen(ssid) >= PET_SSID_MAX) return ESP_ERR_INVALID_ARG;
    password[0] = 0;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("aipet", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK) return err;
    err = read_wifi_pair(nvs, "ssid", "password", ssid, password, capacity);
    if (err != ESP_OK) {
        err = read_wifi_pair(nvs, "ssid_alt", "password_alt", ssid,
                             password, capacity);
    }
    uint8_t count = 0;
    if (err != ESP_OK && nvs_get_u8(nvs, "wifi_count", &count) == ESP_OK) {
        if (count > PET_WIFI_PROFILE_MAX) count = PET_WIFI_PROFILE_MAX;
        for (uint8_t i = 0; i < count && err != ESP_OK; ++i) {
            char ssid_key[16];
            char password_key[16];
            wifi_slot_key(ssid_key, sizeof(ssid_key), "wifi_ssid_", i);
            wifi_slot_key(password_key, sizeof(password_key), "wifi_pass_", i);
            err = read_wifi_pair(nvs, ssid_key, password_key, ssid,
                                 password, capacity);
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK) password[0] = 0;
    return err;
}

bool pet_config_wifi_saved(const char *ssid)
{
    char password[PET_PASSWORD_MAX] = {0};
    bool saved = pet_config_recall_wifi(ssid, password,
                                        sizeof(password)) == ESP_OK;
    memset(password, 0, sizeof(password));
    return saved;
}

esp_err_t pet_config_store_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) >= PET_SSID_MAX || strlen(password) >= PET_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char resolved_password[PET_PASSWORD_MAX] = {0};
    if (!password[0] &&
        pet_config_recall_wifi(ssid, resolved_password,
                               sizeof(resolved_password)) == ESP_OK) {
        password = resolved_password;
    }
    esp_err_t err = pet_config_store_wifi_explicit(ssid, password);
    pet_enrollment_clear(resolved_password, sizeof(resolved_password));
    return err;
}

esp_err_t pet_config_store_wifi_explicit(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) >= PET_SSID_MAX || strlen(password) >= PET_PASSWORD_MAX)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    char current_ssid[PET_SSID_MAX] = {0};
    char current_password[PET_PASSWORD_MAX] = {0};
    char fallback_ssid[PET_SSID_MAX] = {0};
    char fallback_password[PET_PASSWORD_MAX] = {0};
    size_t current_ssid_bytes = sizeof(current_ssid);
    size_t current_password_bytes = sizeof(current_password);
    size_t fallback_ssid_bytes = sizeof(fallback_ssid);
    size_t fallback_password_bytes = sizeof(fallback_password);
    bool have_current =
        nvs_get_str(nvs, "ssid", current_ssid, &current_ssid_bytes) == ESP_OK &&
        current_ssid[0] &&
        nvs_get_str(nvs, "password", current_password, &current_password_bytes) == ESP_OK;
    bool preserve_current = have_current &&
        (strcmp(current_ssid, ssid) || strcmp(current_password, password));
    bool preserve_fallback =
        nvs_get_str(nvs, "ssid_alt", fallback_ssid, &fallback_ssid_bytes) == ESP_OK &&
        fallback_ssid[0] && strcmp(fallback_ssid, ssid) &&
        nvs_get_str(nvs, "password_alt", fallback_password,
                    &fallback_password_bytes) == ESP_OK;
    esp_err_t err = ESP_OK;
    const char *remember_ssids[] = {
        preserve_fallback ? fallback_ssid : NULL,
        preserve_current ? current_ssid : NULL,
        ssid,
    };
    const char *remember_passwords[] = {
        preserve_fallback ? fallback_password : NULL,
        preserve_current ? current_password : NULL,
        password,
    };
    err = remember_wifi(nvs, remember_ssids, remember_passwords,
                        sizeof(remember_ssids) / sizeof(remember_ssids[0]));
    if (err == ESP_OK && preserve_current) {
        err = nvs_set_str(nvs, "ssid_alt", current_ssid);
    }
    if (err == ESP_OK && preserve_current) err = nvs_set_str(nvs, "password_alt", current_password);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    memset(current_password, 0, sizeof(current_password));
    memset(fallback_password, 0, sizeof(fallback_password));
    return err;
}

esp_err_t pet_config_store_wifi_fallback(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) >= PET_SSID_MAX ||
        strlen(password) >= PET_PASSWORD_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    char primary_ssid[PET_SSID_MAX] = {0};
    char primary_password[PET_PASSWORD_MAX] = {0};
    size_t primary_bytes = sizeof(primary_ssid);
    size_t primary_password_bytes = sizeof(primary_password);
    esp_err_t primary_err = nvs_get_str(nvs, "ssid", primary_ssid, &primary_bytes);
    esp_err_t err = primary_err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : primary_err;
    if (err == ESP_OK && primary_ssid[0] && !strcmp(primary_ssid, ssid)) {
        err = ESP_ERR_INVALID_ARG;
    }
    bool have_primary = err == ESP_OK && primary_ssid[0] &&
        nvs_get_str(nvs, "password", primary_password,
                    &primary_password_bytes) == ESP_OK;
    if (err == ESP_OK) {
        const char *remember_ssids[] = {have_primary ? primary_ssid : NULL, ssid};
        const char *remember_passwords[] = {have_primary ? primary_password : NULL,
                                            password};
        err = remember_wifi(nvs, remember_ssids, remember_passwords,
                            sizeof(remember_ssids) / sizeof(remember_ssids[0]));
    }
    if (err == ESP_OK) err = nvs_set_str(nvs, "ssid_alt", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password_alt", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    memset(primary_password, 0, sizeof(primary_password));
    return err;
}

esp_err_t pet_config_restore_wifi_fallback(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG,
                        "open NVS");
    char fallback_ssid[PET_SSID_MAX] = {0};
    char fallback_password[PET_PASSWORD_MAX] = {0};
    size_t ssid_bytes=sizeof(fallback_ssid);
    size_t password_bytes=sizeof(fallback_password);
    esp_err_t err=nvs_get_str(nvs,"ssid_alt",fallback_ssid,&ssid_bytes);
    if(err==ESP_OK)err=nvs_get_str(nvs,"password_alt",fallback_password,
                                   &password_bytes);
    if(err==ESP_OK&&fallback_ssid[0])err=nvs_set_str(nvs,"ssid",fallback_ssid);
    if(err==ESP_OK)err=nvs_set_str(nvs,"password",fallback_password);
    if(err==ESP_OK)err=nvs_set_str(nvs,"ssid_alt","");
    if(err==ESP_OK)err=nvs_set_str(nvs,"password_alt","");
    if(err==ESP_OK)err=nvs_commit(nvs);
    nvs_close(nvs);
    memset(fallback_password,0,sizeof(fallback_password));
    return err;
}

esp_err_t pet_config_list_wifi_networks(pet_wifi_network_metadata_t *networks,
                                        size_t capacity, size_t *count)
{
    if (!networks || !capacity || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    saved_wifi_profile_t *profiles = calloc(PET_WIFI_PROFILE_MAX,
                                             sizeof(*profiles));
    if (!profiles) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs = 0;
    uint8_t stored_count = 0;
    esp_err_t err = nvs_open("aipet", NVS_READONLY, &nvs);
    if (err == ESP_OK) err = load_saved_wifi(nvs, profiles, &stored_count);
    if (err == ESP_OK) {
        size_t output_count = stored_count < capacity ? stored_count : capacity;
        for (size_t index = 0; index < output_count; ++index) {
            strlcpy(networks[index].ssid, profiles[index].ssid,
                    sizeof(networks[index].ssid));
            networks[index].priority = stored_count <= 1 ? 100 :
                (uint8_t)(100 - ((index * 100) / (stored_count - 1)));
        }
        *count = output_count;
    }
    if (nvs) nvs_close(nvs);
    memset(profiles, 0, PET_WIFI_PROFILE_MAX * sizeof(*profiles));
    free(profiles);
    return err;
}

esp_err_t pet_config_set_wifi_priority(const char *ssid, uint8_t priority)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= PET_SSID_MAX || priority > 100)
        return ESP_ERR_INVALID_ARG;
    saved_wifi_profile_t *profiles = calloc(PET_WIFI_PROFILE_MAX,
                                             sizeof(*profiles));
    if (!profiles) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs = 0;
    uint8_t count = 0;
    esp_err_t err = nvs_open("aipet", NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = load_saved_wifi(nvs, profiles, &count);
    uint8_t found = count;
    for (uint8_t index = 0; index < count; ++index) {
        if (!strcmp(profiles[index].ssid, ssid)) { found = index; break; }
    }
    if (err == ESP_OK && found >= count) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK && count > 1) {
        saved_wifi_profile_t selected = profiles[found];
        for (uint8_t index = found; index + 1 < count; ++index)
            profiles[index] = profiles[index + 1];
        uint8_t target = (uint8_t)(((100 - priority) * (count - 1) + 50) / 100);
        if (target >= count) target = count - 1;
        for (uint8_t index = count - 1; index > target; --index)
            profiles[index] = profiles[index - 1];
        profiles[target] = selected;
        memset(&selected, 0, sizeof(selected));
    }
    for (uint8_t index = 0; err == ESP_OK && index < count; ++index) {
        char ssid_key[16]; char password_key[16];
        wifi_slot_key(ssid_key, sizeof(ssid_key), "wifi_ssid_", index);
        wifi_slot_key(password_key, sizeof(password_key), "wifi_pass_", index);
        err = nvs_set_str(nvs, ssid_key, profiles[index].ssid);
        if (err == ESP_OK)
            err = nvs_set_str(nvs, password_key, profiles[index].password);
    }
    if (err == ESP_OK) err = nvs_set_str(nvs, "ssid", profiles[0].ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password", profiles[0].password);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ssid_alt",
                                         count > 1 ? profiles[1].ssid : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, "password_alt",
                                         count > 1 ? profiles[1].password : "");
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs) nvs_close(nvs);
    memset(profiles, 0, PET_WIFI_PROFILE_MAX * sizeof(*profiles));
    free(profiles);
    return err;
}

esp_err_t pet_config_forget_wifi(const char *ssid)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= PET_SSID_MAX)
        return ESP_ERR_INVALID_ARG;
    saved_wifi_profile_t *profiles = calloc(PET_WIFI_PROFILE_MAX,
                                             sizeof(*profiles));
    if (!profiles) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("aipet", NVS_READWRITE, &nvs);
    uint8_t count = 0;
    if (err == ESP_OK) err = load_saved_wifi(nvs, profiles, &count);
    uint8_t found = PET_WIFI_PROFILE_MAX;
    for (uint8_t index = 0; index < count; ++index) {
        if (!strcmp(profiles[index].ssid, ssid)) { found = index; break; }
    }
    if (err == ESP_OK && (found >= count || count <= 1))
        err = found >= count ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        for (uint8_t index = found; index + 1 < count; ++index)
            profiles[index] = profiles[index + 1];
        count--;
        for (uint8_t index = 0; err == ESP_OK && index < PET_WIFI_PROFILE_MAX; ++index) {
            char ssid_key[16]; char password_key[16];
            wifi_slot_key(ssid_key, sizeof(ssid_key), "wifi_ssid_", index);
            wifi_slot_key(password_key, sizeof(password_key), "wifi_pass_", index);
            if (index < count) {
                err = nvs_set_str(nvs, ssid_key, profiles[index].ssid);
                if (err == ESP_OK) err = nvs_set_str(nvs, password_key,
                                                     profiles[index].password);
            } else {
                esp_err_t erased = nvs_erase_key(nvs, ssid_key);
                if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) err = erased;
                erased = nvs_erase_key(nvs, password_key);
                if (err == ESP_OK && erased != ESP_OK &&
                    erased != ESP_ERR_NVS_NOT_FOUND) err = erased;
            }
        }
        if (err == ESP_OK) err = nvs_set_u8(nvs, "wifi_count", count);
        if (err == ESP_OK) err = nvs_set_str(nvs, "ssid", profiles[0].ssid);
        if (err == ESP_OK) err = nvs_set_str(nvs, "password", profiles[0].password);
        if (err == ESP_OK) err = nvs_set_str(nvs, "ssid_alt",
                                             count > 1 ? profiles[1].ssid : "");
        if (err == ESP_OK) err = nvs_set_str(nvs, "password_alt",
                                             count > 1 ? profiles[1].password : "");
        if (err == ESP_OK) err = nvs_commit(nvs);
    }
    if (nvs) nvs_close(nvs);
    memset(profiles, 0, PET_WIFI_PROFILE_MAX * sizeof(*profiles));
    free(profiles);
    return err;
}

esp_err_t pet_config_store_wifi_operation(const char *operation_id,
                                          const char *ssid)
{
    if (!operation_id || strlen(operation_id) != 36 || !ssid || !ssid[0] ||
        strlen(ssid) >= PET_SSID_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "wifi_op", operation_id);
    if (err == ESP_OK) err = nvs_set_str(nvs, "wifi_op_ssid", ssid);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_clear_wifi_operation(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_erase_key(nvs, "wifi_op");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    esp_err_t ssid_err = nvs_erase_key(nvs, "wifi_op_ssid");
    if (err == ESP_OK && ssid_err != ESP_OK && ssid_err != ESP_ERR_NVS_NOT_FOUND)
        err = ssid_err;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_stage_credential(const char *operation_id,
                                      uint32_t version,
                                      const char *credential)
{
    if (!operation_id || strlen(operation_id) != 36 || !version ||
        !credential || strlen(credential) < 32 ||
        strlen(credential) >= PET_TOKEN_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG,
                        "open NVS");
    esp_err_t err = nvs_set_str(nvs, "pending_token", credential);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "pending_ver", version);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pending_op", operation_id);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_commit_credential(const char *operation_id,
                                       uint32_t version)
{
    if (!operation_id || strlen(operation_id) != 36 || !version)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG,
                        "open NVS");
    char pending_token[PET_TOKEN_MAX] = {0};
    char pending_operation[PET_OPERATION_ID_MAX] = {0};
    uint32_t pending_version = 0;
    size_t token_bytes = sizeof(pending_token);
    size_t operation_bytes = sizeof(pending_operation);
    esp_err_t err = nvs_get_str(nvs, "pending_token", pending_token,
                                &token_bytes);
    if (err == ESP_OK)
        err = nvs_get_str(nvs, "pending_op", pending_operation,
                          &operation_bytes);
    if (err == ESP_OK) err = nvs_get_u32(nvs, "pending_ver", &pending_version);
    if (err == ESP_OK && (strcmp(pending_operation, operation_id) ||
                         pending_version != version ||
                         strlen(pending_token) < 32)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = nvs_set_str(nvs, "token", pending_token);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "credver", pending_version);
    if (err == ESP_OK) err = nvs_erase_key(nvs, "pending_token");
    if (err == ESP_OK) err = nvs_erase_key(nvs, "pending_ver");
    if (err == ESP_OK) err = nvs_erase_key(nvs, "pending_op");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    memset(pending_token, 0, sizeof(pending_token));
    return err;
}

esp_err_t pet_config_store_volume(uint8_t volume)
{
    if (volume > 100) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u8(nvs, "volume", volume);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t store_u8(const char *key, uint8_t value)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u8(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_brightness(uint8_t brightness)
{
    if (brightness < PET_BRIGHTNESS_MIN || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_u8("brightness", brightness);
}

esp_err_t pet_config_store_shake_sensitivity(uint8_t sensitivity)
{
    if (sensitivity > 100) return ESP_ERR_INVALID_ARG;
    return store_u8("shake_sens", sensitivity);
}

esp_err_t pet_config_store_face_profiles(const pet_face_profiles_t *profiles)
{
    if (!face_profiles_valid(profiles)) return ESP_ERR_INVALID_ARG;
    face_profiles_blob_t blob = {
        .magic = FACE_PROFILES_MAGIC,
        .version = FACE_PROFILES_VERSION,
        .bytes = sizeof(blob.profiles),
        .profiles = *profiles,
    };
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_blob(nvs, "face_profiles", &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool pet_config_recording_timeout_valid(uint16_t seconds)
{
    for (uint8_t i = 0; i < PET_RECORDING_TIMEOUT_COUNT; ++i) {
        if (s_recording_timeouts[i] == seconds) return true;
    }
    return false;
}

uint16_t pet_config_recording_timeout_for_index(uint8_t index)
{
    return index < PET_RECORDING_TIMEOUT_COUNT ? s_recording_timeouts[index] :
                                                 PET_RECORDING_TIMEOUT_DEFAULT_SECONDS;
}

uint8_t pet_config_recording_timeout_index(uint16_t seconds)
{
    for (uint8_t i = 0; i < PET_RECORDING_TIMEOUT_COUNT; ++i) {
        if (s_recording_timeouts[i] == seconds) return i;
    }
    return 2;
}

esp_err_t pet_config_store_recording_timeout(uint16_t seconds)
{
    if (!pet_config_recording_timeout_valid(seconds)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u16(nvs, "record_max_s", seconds);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_animation_profile(pet_animation_profile_t profile)
{
    if (!pet_animation_profile_valid(profile)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u8(nvs, "anim_profile", (uint8_t)profile);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_face_id(const char *face_id)
{
    if (!pet_face_id_valid(face_id)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "face_id", face_id);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_voice(pet_voice_t voice)
{
    if (voice < PET_VOICE_PUCK || voice >= PET_VOICE_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u8(nvs, "voice", (uint8_t)voice);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_synced_settings(uint32_t version, uint8_t volume,
                                           uint16_t recording_timeout_seconds,
                                           const char *face_id,
                                           const char *ai_pet_id,
                                           pet_voice_t voice)
{
    /* Version zero is the pre-sync baseline and is needed when rolling back a
     * failed first settings transaction. Network messages still require a
     * positive version before they reach this storage primitive. */
    if (volume > 100 || !pet_config_recording_timeout_valid(recording_timeout_seconds) ||
        !pet_face_id_valid(face_id) || !pet_config_ai_pet_id_valid(ai_pet_id) ||
        voice < PET_VOICE_PUCK || voice >= PET_VOICE_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u32(nvs, "settings_ver", version);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "volume", volume);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "record_max_s", recording_timeout_seconds);
    if (err == ESP_OK) err = nvs_set_str(nvs, "face_id", face_id);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ai_pet_id", ai_pet_id);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "voice", (uint8_t)voice);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_synced_settings_v2(uint32_t version,
                                              const char *fingerprint,
                                              uint8_t volume,
                                              uint8_t brightness,
                                              uint8_t shake_sensitivity,
                                              uint16_t recording_timeout_seconds,
                                              pet_animation_profile_t animation_profile,
                                              const char *face_id,
                                              const char *ai_pet_id,
                                              pet_speech_profile_t speech_profile)
{
    if (!fingerprint || strlen(fingerprint) != 64 || volume > 100 ||
        brightness < PET_BRIGHTNESS_MIN || brightness > 100 ||
        shake_sensitivity > 100 ||
        !pet_config_recording_timeout_valid(recording_timeout_seconds) ||
        !pet_animation_profile_valid(animation_profile) ||
        !pet_face_id_valid(face_id) || !pet_config_ai_pet_id_valid(ai_pet_id) ||
        (speech_profile != PET_SPEECH_PROFILE_CARTESIA_BATCH &&
         speech_profile != PET_SPEECH_PROFILE_CARTESIA_REALTIME)) return ESP_ERR_INVALID_ARG;
    for (size_t index = 0; index < 64; ++index) {
        char value = fingerprint[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
            return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u8(nvs, "config_schema", 2);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "settings_ver", version);
    if (err == ESP_OK) err = nvs_set_str(nvs, "config_fp", fingerprint);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "volume", volume);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "brightness", brightness);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "shake_sens", shake_sensitivity);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "record_max_s", recording_timeout_seconds);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "anim_profile", (uint8_t)animation_profile);
    if (err == ESP_OK) err = nvs_set_str(nvs, "face_id", face_id);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ai_pet_id", ai_pet_id);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "stt_pref_ver", version);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "speech_profile", (uint8_t)speech_profile);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_ai_mode(pet_ai_mode_t mode)
{
    return mode < PET_AI_MODE_COUNT ? store_u8("ai_mode", (uint8_t)mode) : ESP_ERR_INVALID_ARG;
}

esp_err_t pet_config_store_realtime_model(pet_realtime_model_t model)
{
    return model < PET_REALTIME_MODEL_COUNT ? store_u8("rt_model", (uint8_t)model) : ESP_ERR_INVALID_ARG;
}

esp_err_t pet_config_store_realtime_voice(pet_realtime_voice_t voice)
{
    return voice < PET_REALTIME_VOICE_COUNT ? store_u8("rt_voice", (uint8_t)voice) : ESP_ERR_INVALID_ARG;
}

esp_err_t pet_config_store_realtime_boost(pet_realtime_boost_t boost)
{
    return boost >= PET_REALTIME_BOOST_OFF && boost < PET_REALTIME_BOOST_COUNT ?
        store_u8("rt_boost", (uint8_t)boost) : ESP_ERR_INVALID_ARG;
}

esp_err_t pet_config_store_cartesia_voice_gender(pet_cartesia_voice_gender_t gender)
{
    return gender < PET_CARTESIA_VOICE_GENDER_COUNT ?
        store_u8("cart_gender", (uint8_t)gender) : ESP_ERR_INVALID_ARG;
}

esp_err_t pet_config_store_speech_mouth_mode(pet_speech_mouth_mode_t mode)
{
    return pet_speech_mouth_mode_valid(mode) ?
        store_u8("mouth_mode", (uint8_t)mode) : ESP_ERR_INVALID_ARG;
}

esp_err_t pet_config_store_speech_mouth_offset(bool present, int16_t offset_ms)
{
    if (present && !pet_speech_mouth_offset_valid(offset_ms)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = present ? nvs_set_i16(nvs, "mouth_ofs_ms", offset_ms) :
                              nvs_erase_key(nvs, "mouth_ofs_ms");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t pet_config_store_speech_profile(uint32_t version, pet_speech_profile_t mode)
{
    if (!version || mode >= PET_SPEECH_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("aipet", NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_u32(nvs, "stt_pref_ver", version);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "speech_profile", (uint8_t)mode);
    /* Keep the legacy key synchronized so a rollback still restores the
     * Cartesia choice. Old firmware safely treats Fish (2) as its default. */
    if (err == ESP_OK) err = nvs_set_u8(nvs, "stt_mode", (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool parse_realtime_boost(const char *value, pet_realtime_boost_t *boost)
{
    if (!value || !boost) return false;
    if (!strcasecmp(value, "off")) *boost = PET_REALTIME_BOOST_OFF;
    else if (!strcmp(value, "3") || !strcmp(value, "+3")) *boost = PET_REALTIME_BOOST_3_DB;
    else if (!strcmp(value, "6") || !strcmp(value, "+6")) *boost = PET_REALTIME_BOOST_6_DB;
    else if (!strcmp(value, "9") || !strcmp(value, "+9")) *boost = PET_REALTIME_BOOST_9_DB;
    else return false;
    return true;
}

static bool parse_voice(const char *value, pet_voice_t *voice)
{
    if (!value || !voice) return false;
    for (pet_voice_t candidate = PET_VOICE_PUCK; candidate < PET_VOICE_COUNT; candidate++) {
        if (!strcasecmp(value, pet_voice_name(candidate))) {
            *voice = candidate;
            return true;
        }
    }
    return false;
}

static void console_task(void *arg)
{
    (void)arg;
    char line[384];
    print_help();
    while (fgets(line, sizeof(line), stdin)) {
        char *command = trim(line);
        if (!strncmp(command, "add-wifi ", 9)) {
            char *ssid = trim(command + 9);
            char *space = strchr(ssid, ' ');
            esp_err_t err = ESP_ERR_INVALID_ARG;
            if (space) {
                *space = 0;
                char *password = trim(space + 1);
                err = pet_config_store_wifi_fallback(ssid, password);
            }
            printf(err == ESP_OK ? "Fallback Wi-Fi saved.\n" :
                                   "Rejected (%s).\n", esp_err_to_name(err));
        } else if (!strncmp(command, "set ", 4)) {
            char *key = command + 4;
            char *space = strchr(key, ' ');
            if (!space) {
                printf("Missing value\n> ");
                continue;
            }
            *space = 0;
            char *value = trim(space + 1);
            esp_err_t err = ESP_ERR_INVALID_ARG;
            if (!strcmp(key, "ssid") && strlen(value) < PET_SSID_MAX) err = store_value("ssid", value);
            else if (!strcmp(key, "password") && strlen(value) < PET_PASSWORD_MAX) err = store_value("password", value);
            else if (!strcmp(key, "gateway") && strlen(value) < PET_GATEWAY_MAX) err = store_value("gateway", value);
            else if (!strcmp(key, "token") && strlen(value) >= 16 && strlen(value) < PET_TOKEN_MAX) err = store_value("token", value);
            else if (!strcmp(key, "credential-version")) {
                unsigned long version = strtoul(value, NULL, 10);
                if (version > 0 && version <= UINT32_MAX) {
                    nvs_handle_t nvs;
                    if (nvs_open("aipet", NVS_READWRITE, &nvs) == ESP_OK) {
                        err = nvs_set_u32(nvs, "credver", (uint32_t)version);
                        if (err == ESP_OK) err = nvs_commit(nvs);
                        nvs_close(nvs);
                    }
                }
            }
            else if (!strcmp(key, "volume")) {
                long volume = strtol(value, NULL, 10);
                if (volume >= 0 && volume <= 100) err = pet_config_store_volume((uint8_t)volume);
            }
            else if (!strcmp(key, "brightness")) {
                long brightness = strtol(value, NULL, 10);
                if (brightness >= PET_BRIGHTNESS_MIN && brightness <= 100)
                    err = pet_config_store_brightness((uint8_t)brightness);
            }
            else if (!strcmp(key, "shake-sensitivity")) {
                long sensitivity = strtol(value, NULL, 10);
                if (sensitivity >= 0 && sensitivity <= 100)
                    err = pet_config_store_shake_sensitivity((uint8_t)sensitivity);
            }
            else if (!strcmp(key, "recording-timeout")) {
                long seconds = strtol(value, NULL, 10);
                if (seconds > 0 && seconds <= UINT16_MAX) {
                    err = pet_config_store_recording_timeout((uint16_t)seconds);
                }
            }
            else if (!strcmp(key, "animation")) {
                pet_animation_profile_t profile;
                if (pet_animation_profile_parse_wire(value, &profile)) {
                    err = pet_config_store_animation_profile(profile);
                }
            }
            else if (!strcmp(key, "voice")) {
                pet_voice_t voice;
                if (parse_voice(value, &voice)) err = pet_config_store_voice(voice);
            }
            else if (!strcmp(key, "ai-mode")) {
                if (!strcasecmp(value, "openrouter")) err = pet_config_store_ai_mode(PET_AI_MODE_OPENROUTER);
                else if (!strcasecmp(value, "openai-realtime")) err = pet_config_store_ai_mode(PET_AI_MODE_OPENAI_REALTIME);
                else if (!strcasecmp(value, "cartesia-agent")) err = pet_config_store_ai_mode(PET_AI_MODE_CARTESIA_AGENT);
            }
            else if (!strcmp(key, "realtime-model")) {
                for (pet_realtime_model_t model = PET_REALTIME_MODEL_2_1_MINI; model < PET_REALTIME_MODEL_COUNT; model++) {
                    if (!strcasecmp(value, pet_realtime_model_name(model))) { err = pet_config_store_realtime_model(model); break; }
                }
            }
            else if (!strcmp(key, "realtime-voice")) {
                for (pet_realtime_voice_t voice = PET_REALTIME_VOICE_CEDAR; voice < PET_REALTIME_VOICE_COUNT; voice++) {
                    if (!strcasecmp(value, pet_realtime_voice_name(voice))) { err = pet_config_store_realtime_voice(voice); break; }
                }
            }
            else if (!strcmp(key, "realtime-boost")) {
                pet_realtime_boost_t boost;
                if (parse_realtime_boost(value, &boost)) err = pet_config_store_realtime_boost(boost);
            }
            else if (!strcmp(key, "cartesia-gender")) {
                for (pet_cartesia_voice_gender_t gender = PET_CARTESIA_VOICE_NEUTRAL;
                     gender < PET_CARTESIA_VOICE_GENDER_COUNT; gender++) {
                    if (!strcasecmp(value, pet_cartesia_voice_gender_name(gender))) {
                        err = pet_config_store_cartesia_voice_gender(gender);
                        break;
                    }
                }
            }
            else if (!strcmp(key, "speech-mouth")) {
                pet_speech_mouth_mode_t mode;
                if (pet_speech_mouth_mode_parse(value, &mode)) {
                    err = pet_config_store_speech_mouth_mode(mode);
                }
            }
            else if (!strcmp(key, "secure") && (!strcmp(value, "on") || !strcmp(value, "off"))) {
                nvs_handle_t nvs;
                if (nvs_open("aipet", NVS_READWRITE, &nvs) == ESP_OK) {
                    err = nvs_set_u8(nvs, "secure", !strcmp(value, "on") ? 1 : 0);
                    if (err == ESP_OK) err = nvs_commit(nvs);
                    nvs_close(nvs);
                }
            }
            else if (!strcmp(key, "port")) {
                long port = strtol(value, NULL, 10);
                if (port > 0 && port <= 65535) {
                    nvs_handle_t nvs;
                    if (nvs_open("aipet", NVS_READWRITE, &nvs) == ESP_OK) {
                        err = nvs_set_u16(nvs, "port", (uint16_t)port);
                        if (err == ESP_OK) err = nvs_commit(nvs);
                        nvs_close(nvs);
                    }
                }
            }
            printf(err == ESP_OK ? "Saved.\n" : "Rejected (%s).\n", esp_err_to_name(err));
            if (err == ESP_OK && s_changed_callback) s_changed_callback();
        } else if (!strcmp(command, "sound deployment-complete")) {
            /* Report success only after the chime actually played. */
            esp_err_t err = pet_sfx_play_and_wait(PET_SFX_DEPLOY_COMPLETE, 3000);
            printf(err == ESP_OK ? "DEPLOYMENT_COMPLETE_SOUND_OK\n" :
                                   "Deployment sound failed (%s).\n",
                   esp_err_to_name(err));
        } else if (!strcmp(command, "show")) {
            pet_config_t config;
            pet_config_load(&config);
            printf("SSID: %s\nFallback SSID: %s\nGateway: %s:%u (%s)\nPassword: %s\nFallback password: %s\nToken: %s\nCredential version: %lu\nVolume: %u\nBrightness: %u\nShake sensitivity: %u\nFace profiles: %u\nRecording timeout: %u seconds\nAnimation: %s\nClassic voice: %s\nAI mode: %s\nRealtime model: %s\nRealtime voice: %s\nRealtime boost: %s\nCartesia voice gender: %s\nSpeech mouth: %s\nMouth timing: %d ms\nReady: %s\n",
                   config.wifi_ssid[0] ? config.wifi_ssid : "<unset>",
                   config.wifi_fallback_ssid[0] ? config.wifi_fallback_ssid : "<unset>",
                   config.gateway_host[0] ? config.gateway_host : "<unset>", config.gateway_port,
                   config.gateway_secure ? "secure" : "local insecure",
                   config.wifi_password[0] ? "<stored>" : "<unset>",
                   config.wifi_fallback_password[0] ? "<stored>" : "<unset>",
                   config.pairing_token[0] ? "<stored>" : "<unset>",
                   (unsigned long)config.credential_version,
                   config.volume,
                   config.brightness,
                   config.shake_sensitivity,
                   config.face_profiles.count,
                   config.recording_timeout_seconds,
                   pet_animation_profile_name(config.animation_profile),
                   pet_voice_name(config.voice),
                   pet_ai_mode_name(config.ai_mode),
                   pet_realtime_model_name(config.realtime_model),
                   pet_realtime_voice_name(config.realtime_voice),
                   pet_realtime_boost_name(config.realtime_boost),
                   pet_cartesia_voice_gender_name(config.cartesia_voice_gender),
                   pet_speech_mouth_mode_name(config.speech_mouth_mode),
                   config.speech_mouth_offset_ms,
                   pet_config_ready(&config) ? "yes" : "no");
        } else if (!strcmp(command, "clear")) {
            nvs_handle_t nvs;
            if (nvs_open("aipet", NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_erase_all(nvs);
                nvs_commit(nvs);
                nvs_close(nvs);
                printf("Provisioning cleared.\n");
                if (s_changed_callback) s_changed_callback();
            }
        } else if (!strcmp(command, "restart")) {
            esp_restart();
        } else if (!strcmp(command, "help") || !*command) {
            print_help();
            continue;
        } else {
            printf("Unknown command. Type help.\n");
        }
        printf("> ");
        fflush(stdout);
    }
    vTaskDelete(NULL);
}

void pet_config_start_console(void (*changed_callback)(void))
{
    s_changed_callback = changed_callback;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    }
    usb_serial_jtag_vfs_use_driver();
    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
#endif
    /*
     * The console can invoke the codec-backed deployment-complete SFX.  That
     * path needs more stack than the provisioning-only commands and overflowed
     * the original 4 KiB task on hardware.  Keep enough headroom for the audio
     * driver call and its logging/formatting frames.
     */
    xTaskCreate(console_task, "pet_console", 8192, NULL, 2, NULL);
}
