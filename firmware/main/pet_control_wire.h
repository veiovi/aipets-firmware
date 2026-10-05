#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_install.h"

typedef struct cJSON cJSON;
#define PET_CONTROL_RESPONSE_MAX 32768
#define PET_CONTROL_LIBRARY_MAX 10

typedef struct {
    bool assigned;
    char revision[81], relationship_id[37], build_id[37], sha256[65];
} pet_control_binding_t;
typedef struct {
    char version[81];
    uint8_t volume, brightness, shake_sensitivity;
    uint16_t recording_timeout;
    uint8_t animation_profile, speech_profile;
    char ai_pet_id[81], face_id[64];
} pet_control_config_t;
typedef struct {
    char device_id[81], account_id[37];
    bool selection_allowed;
    pet_control_binding_t binding;
    pet_control_config_t config;
    char pending_id[37];
    uint16_t next_poll_seconds;
} pet_control_context_t;
typedef struct {
    char project_id[37], build_id[37], face_id[81], name[97], pack_version[81], sha256[65];
    uint32_t bytes;
    bool selected, installable;
    char unavailable_code[81];
} pet_control_library_item_t;
typedef struct {
    pet_control_library_item_t items[PET_CONTROL_LIBRARY_MAX];
    size_t count;
    char next_cursor[513];
} pet_control_library_t;
typedef enum { PET_OP_QUEUED, PET_OP_DOWNLOADING, PET_OP_VERIFIED, PET_OP_ACTIVATING,
               PET_OP_INSTALLED, PET_OP_FAILED, PET_OP_CANCELLED } pet_control_op_state_t;
typedef struct {
    char id[37], build_id[37], sha256[65], download_path[257];
    uint32_t bytes, downloaded_bytes;
    pet_control_op_state_t state;
    pet_control_binding_t resulting_binding;
    /* Exact signed UTF-8 string. Never reconstruct from parsed fields. */
    char signed_payload[7001], key_id[161], signature[87];
} pet_control_operation_t;

/* Bounded recursion/node count, no embedded NUL, duplicate keys or trailing
 * JSON. Caller owns returned object and releases it with cJSON_Delete. */
cJSON *pet_control_json(const char *data, size_t bytes, size_t limit);
bool pet_control_version(const cJSON *root);
bool pet_control_binding(const cJSON *object, pet_control_binding_t *out);
bool pet_control_config(const cJSON *object, pet_control_config_t *out);
bool pet_control_context(const cJSON *root, pet_control_context_t *out);
bool pet_control_context_encode(const pet_control_context_t *context,char *json,size_t capacity);
bool pet_control_library(const cJSON *root, pet_control_library_t *out);
bool pet_control_operation(const cJSON *object, pet_control_operation_t *out);
bool pet_control_same_binding(const pet_control_binding_t *a, const pet_control_binding_t *b);
/* Lost-ACK reconciliation must match the operation's committed result AND the
 * current cloud context. A newer unrelated binding never blesses an old pack. */
bool pet_control_commit_install(pet_install_t *state, const pet_control_operation_t *operation,
                                 const pet_control_context_t *context);
bool pet_control_install_encode(const pet_install_t *state, char *json, size_t capacity, size_t *bytes);
bool pet_control_install_decode(const char *json, size_t bytes, pet_install_t *state);
