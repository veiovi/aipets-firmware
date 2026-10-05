#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define PET_OTA_RELEASE_ID_MAX 65
#define PET_OTA_VERSION_MAX 33
#define PET_OTA_URL_MAX 321
#define PET_OTA_TOKEN_MAX 513

typedef struct {
    char release_id[PET_OTA_RELEASE_ID_MAX];
    char version[PET_OTA_VERSION_MAX];
    char url[PET_OTA_URL_MAX];
    char token[PET_OTA_TOKEN_MAX];
    char sha256[65];
    size_t bytes;
} pet_ota_request_t;

typedef void (*pet_ota_status_callback_t)(const char *release_id,
                                          const char *status,
                                          unsigned progress,
                                          const char *error_code);

typedef enum {
    PET_OTA_HEALTH_PRE_ENROLLMENT,
    PET_OTA_HEALTH_ENROLLED_NO_PET,
    PET_OTA_HEALTH_INSTALLED_PET,
} pet_ota_health_stage_t;

typedef struct {
    bool native_ui_ready;
    bool setup_service_ready;
    bool storage_ready;
    bool authenticated_control;
    bool pet_runtime_ready;
} pet_ota_health_evidence_t;

esp_err_t pet_ota_init(pet_ota_status_callback_t status_callback);
esp_err_t pet_ota_start(const pet_ota_request_t *request);
esp_err_t pet_ota_confirm_healthy(void);
/* Confirm a pending image only from the evidence appropriate to its lifecycle
 * stage. Wi-Fi connectivity is deliberately not health evidence. */
esp_err_t pet_ota_confirm_health_stage(
    pet_ota_health_stage_t stage,
    const pet_ota_health_evidence_t *evidence);
bool pet_ota_busy(void);
