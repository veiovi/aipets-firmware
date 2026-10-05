#include "pet_ota.h"
#include "pet_flash_layout_esp.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#define OTA_TASK_STACK_BYTES 8192
#define OTA_HEALTH_DEADLINE_MS 90000
#define OTA_READ_BYTES 4096

static const char *TAG = "pet_ota";
static pet_ota_status_callback_t s_status_callback;
static volatile bool s_busy;
static volatile bool s_pending_verify;
static volatile bool s_rolled_back;
static char s_pending_release[PET_OTA_RELEASE_ID_MAX];
typedef struct {
    uint32_t magic;
    char release_id[PET_OTA_RELEASE_ID_MAX];
    char version[PET_OTA_VERSION_MAX];
} ota_reboot_marker_t;
static RTC_NOINIT_ATTR ota_reboot_marker_t s_reboot_marker;
#define OTA_REBOOT_MARKER 0x4f544131u

static void report(const char *release_id, const char *status,
                   unsigned progress, const char *error_code)
{
    if (s_status_callback) s_status_callback(release_id, status, progress, error_code);
}

static bool exact_lower_hex(const char *value, size_t count)
{
    if (!value || strlen(value) != count) return false;
    for (size_t index = 0; index < count; ++index)
        if (!isdigit((unsigned char)value[index]) &&
            !(value[index] >= 'a' && value[index] <= 'f')) return false;
    return true;
}

static bool request_valid(const pet_ota_request_t *request)
{
    static const char prefix[] = CONFIG_PET_VNEXT_CONTROL_ORIGIN "/v1/device/firmware/";
    return request && exact_lower_hex(request->release_id, 64) &&
        exact_lower_hex(request->sha256, 64) &&
        !strcmp(request->release_id, request->sha256) &&
        request->version[0] && request->token[0] &&
        request->bytes >= 1024 && request->bytes <= 0x620000 &&
        !strncmp(request->url, prefix, sizeof(prefix) - 1) &&
        !strcmp(request->url + sizeof(prefix) - 1, request->release_id);
}

static bool decode_sha256(const char *hex, uint8_t output[32])
{
    if (!exact_lower_hex(hex, 64)) return false;
    for (size_t index = 0; index < 32; ++index) {
        char pair[3] = { hex[index * 2], hex[index * 2 + 1], 0 };
        output[index] = (uint8_t)strtoul(pair, NULL, 16);
    }
    return true;
}

static void ota_health_deadline_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_HEALTH_DEADLINE_MS));
    if (s_pending_verify) {
        ESP_LOGE(TAG, "new application did not become healthy; rebooting for rollback");
        esp_restart();
    }
    vTaskDelete(NULL);
}

static void ota_task(void *arg)
{
    pet_ota_request_t request = *(pet_ota_request_t *)arg;
    free(arg);
    esp_err_t err = ESP_FAIL;
    const char *error_code = "OTA_FAILED";
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t handle = 0;
    bool ota_started = false;
    uint8_t *buffer = NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || request.bytes > partition->size) {
        error_code = "OTA_IMAGE_TOO_LARGE";
        goto done;
    }
    if (!strcmp(request.version, esp_app_get_description()->version)) {
        error_code = "OTA_VERSION_CURRENT";
        goto done;
    }

    char authorization[PET_OTA_TOKEN_MAX + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", request.token);
    esp_http_client_config_t config = {
        .url = request.url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
        .buffer_size = OTA_READ_BYTES,
    };
    client = esp_http_client_init(&config);
    if (!client) { error_code = "OTA_HTTP_INIT"; goto done; }
    esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, sizeof(authorization));
    if (esp_http_client_open(client, 0) != ESP_OK) {
        error_code = "OTA_HTTP_OPEN";
        goto done;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        error_code = "OTA_HTTP_STATUS";
        goto done;
    }
    if (content_length >= 0 && (size_t)content_length != request.bytes) {
        error_code = "OTA_LENGTH_HEADER";
        goto done;
    }
    if (esp_ota_begin(partition, request.bytes, &handle) != ESP_OK) {
        error_code = "OTA_BEGIN";
        goto done;
    }
    ota_started = true;
    buffer = malloc(OTA_READ_BYTES);
    if (!buffer) { error_code = "OTA_NO_MEMORY"; goto done; }
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        error_code = "OTA_HASH_INIT";
        goto done;
    }

    report(request.release_id, "downloading", 0, NULL);
    size_t received = 0;
    unsigned last_progress = 0;
    for (;;) {
        int count = esp_http_client_read(client, (char *)buffer, OTA_READ_BYTES);
        if (count < 0) { error_code = "OTA_HTTP_READ"; goto done; }
        if (count == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (received + (size_t)count > request.bytes) {
            error_code = "OTA_LENGTH_OVERFLOW";
            goto done;
        }
        if (esp_ota_write(handle, buffer, (size_t)count) != ESP_OK) {
            error_code = "OTA_WRITE";
            goto done;
        }
        if (mbedtls_sha256_update(&sha, buffer, (size_t)count) != 0) {
            error_code = "OTA_HASH_UPDATE";
            goto done;
        }
        received += (size_t)count;
        unsigned progress = (unsigned)(received * 100 / request.bytes);
        if (progress >= last_progress + 10) {
            last_progress = progress;
            report(request.release_id, "downloading", progress, NULL);
        }
    }
    if (received != request.bytes) { error_code = "OTA_LENGTH_MISMATCH"; goto done; }
    uint8_t actual[32], expected[32];
    if (mbedtls_sha256_finish(&sha, actual) != 0 ||
        !decode_sha256(request.sha256, expected) ||
        memcmp(actual, expected, sizeof(actual))) {
        error_code = "OTA_HASH_MISMATCH";
        goto done;
    }
    if (esp_ota_end(handle) != ESP_OK) { error_code = "OTA_IMAGE_INVALID"; goto done; }
    ota_started = false;
    if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        error_code = "OTA_BOOT_SLOT";
        goto done;
    }
    report(request.release_id, "verified", 100, NULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    report(request.release_id, "rebooting", 100, NULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    s_reboot_marker.magic = OTA_REBOOT_MARKER;
    strlcpy(s_reboot_marker.release_id, request.release_id, sizeof(s_reboot_marker.release_id));
    strlcpy(s_reboot_marker.version, request.version, sizeof(s_reboot_marker.version));
    err = ESP_OK;

done:
    if (ota_started) esp_ota_abort(handle);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (buffer) { memset(buffer, 0, OTA_READ_BYTES); free(buffer); }
    memset(request.token, 0, sizeof(request.token));
    mbedtls_sha256_free(&sha);
    s_busy = false;
    if (err == ESP_OK) esp_restart();
    ESP_LOGE(TAG, "OTA failed: %s", error_code);
    report(request.release_id, "failed", 0, error_code);
    vTaskDelete(NULL);
}

esp_err_t pet_ota_init(pet_ota_status_callback_t status_callback)
{
    s_status_callback = status_callback;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool marker_valid = s_reboot_marker.magic == OTA_REBOOT_MARKER &&
        exact_lower_hex(s_reboot_marker.release_id, 64) && s_reboot_marker.version[0];
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        if (marker_valid) strlcpy(s_pending_release, s_reboot_marker.release_id, sizeof(s_pending_release));
        ESP_LOGW(TAG, "OTA image pending authenticated health confirmation");
        if (xTaskCreate(ota_health_deadline_task, "ota_health", 3072, NULL, 7, NULL) != pdPASS)
            return ESP_ERR_NO_MEM;
    } else if (marker_valid && strcmp(esp_app_get_description()->version, s_reboot_marker.version)) {
        s_rolled_back = true;
        strlcpy(s_pending_release, s_reboot_marker.release_id, sizeof(s_pending_release));
    }
    return ESP_OK;
}

esp_err_t pet_ota_start(const pet_ota_request_t *request)
{
    if (!request_valid(request)) return ESP_ERR_INVALID_ARG;
    /* A queued legacy socket command may arrive after USB migration. Reject it
     * locally before allocation, acceptance or any flash operation. The v2
     * updater has its own signed requirements and serialized flash ownership. */
    pet_flash_layout_t layout;
    char partition_sha256[65];
    if (pet_flash_layout_read(&layout, partition_sha256) != ESP_OK ||
        !pet_flash_layout_allows_legacy_ota(&layout)) return ESP_ERR_NOT_SUPPORTED;
    if (s_busy || s_pending_verify) return ESP_ERR_INVALID_STATE;
    pet_ota_request_t *copy = malloc(sizeof(*copy));
    if (!copy) return ESP_ERR_NO_MEM;
    *copy = *request;
    s_busy = true;
    strlcpy(s_pending_release, request->release_id, sizeof(s_pending_release));
    report(request->release_id, "accepted", 0, NULL);
    if (xTaskCreate(ota_task, "pet_ota", OTA_TASK_STACK_BYTES, copy, 6, NULL) != pdPASS) {
        memset(copy, 0, sizeof(*copy));
        free(copy);
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t pet_ota_confirm_healthy(void)
{
    if (s_rolled_back) {
        report(s_pending_release, "rolled_back", 0, "OTA_HEALTH_ROLLBACK");
        s_rolled_back = false;
        memset(&s_reboot_marker, 0, sizeof(s_reboot_marker));
        return ESP_OK;
    }
    if (!s_pending_verify) return ESP_OK;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_pending_verify = false;
        if (s_pending_release[0]) report(s_pending_release, "healthy", 100, NULL);
        memset(&s_reboot_marker, 0, sizeof(s_reboot_marker));
        ESP_LOGI(TAG, "OTA image confirmed after validated lifecycle health gate");
    }
    return err;
}

esp_err_t pet_ota_confirm_health_stage(
    pet_ota_health_stage_t stage,
    const pet_ota_health_evidence_t *evidence)
{
    if(!evidence||!evidence->native_ui_ready)return ESP_ERR_INVALID_ARG;
    bool sufficient=false;
    switch(stage) {
        case PET_OTA_HEALTH_PRE_ENROLLMENT:
            sufficient=evidence->setup_service_ready;
            break;
        case PET_OTA_HEALTH_ENROLLED_NO_PET:
            sufficient=evidence->setup_service_ready&&evidence->storage_ready&&
                evidence->authenticated_control;
            break;
        case PET_OTA_HEALTH_INSTALLED_PET:
            sufficient=evidence->setup_service_ready&&evidence->storage_ready&&
                evidence->authenticated_control&&evidence->pet_runtime_ready;
            break;
        default:return ESP_ERR_INVALID_ARG;
    }
    if(!sufficient)return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG,"OTA health gate passed stage=%u",(unsigned)stage);
    return pet_ota_confirm_healthy();
}

bool pet_ota_busy(void) { return s_busy; }
