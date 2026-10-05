#pragma once

#include "esp_err.h"
#include "pet_enrollment.h"
#include "pet_link.h"

/* Single worker owns NVS and HTTPS. UI only queues a transient setup code. */
typedef void (*pet_setup_status_callback_t)(pet_enrollment_result_t status);
esp_err_t pet_setup_start(const char *https_origin, pet_setup_status_callback_t callback);
esp_err_t pet_setup_submit(const char *code);
/* The link code, on the same worker. The screen gets the
 * link's state, on the worker, whenever it changes: register before start.
 * The owner's actions never wait, and the latest one counts. START on a device
 * that has an account moves it to another one: only the deck asks for that. */
typedef void (*pet_setup_link_callback_t)(const pet_link_t *link);
void pet_setup_on_link(pet_setup_link_callback_t callback);
typedef enum { PET_SETUP_LINK_START, PET_SETUP_LINK_YES, PET_SETUP_LINK_NO, PET_SETUP_LINK_LEAVE } pet_setup_link_action_t;
esp_err_t pet_setup_link(pet_setup_link_action_t action);
/* Available only after redundant completion is durable; immutable thereafter.
 * Caller owns private buffers and must clear them when no longer needed. */
bool pet_setup_identity(char device_id[PET_ENROLLMENT_DEVICE_ID_BYTES],
                         char credential[PET_ENROLLMENT_CREDENTIAL_BYTES]);
bool pet_setup_control_ready(void); /* Clock synchronized and setup confirmed this boot. */
