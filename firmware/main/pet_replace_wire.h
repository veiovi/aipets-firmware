#pragma once
#include "pet_release_v2.h"

/* Cloud state is not a local journal phase or permission to touch flash. */
typedef enum {
    PET_CLOUD_QUEUED, PET_CLOUD_FENCED, PET_CLOUD_INVALIDATED, PET_CLOUD_DOWNLOADING,
    PET_CLOUD_VERIFIED, PET_CLOUD_ACTIVATING, PET_CLOUD_INSTALLED, PET_CLOUD_RECOVERY,
    PET_CLOUD_CANCELLED, PET_CLOUD_SUPERSEDED
} pet_replace_cloud_phase_t;
typedef struct {
    pet_replace_cloud_phase_t phase;
    pet_replace_pack_t pack;
    char operation_id[37], fence_id[37];
    uint64_t generation;
    uint32_t downloaded_bytes;
    char prefix_sha256[65], binding_revision[81], relationship_id[37], config_version[81];
} pet_replace_report_t;
typedef struct {
    pet_replace_cloud_phase_t phase;
    char id[37], request_id[37], fence_id[37], expected_revision[81];
    bool fence_confirmed, flash_reserved, cancel_requested, supersedes, has_report, has_result;
    char previous_id[37], previous_fence_id[37];
    uint64_t previous_generation;
    pet_release_v2_t release;
    pet_replace_report_t report;
    pet_control_binding_t binding;
    pet_control_config_t config;
    /* Original signature bytes retained for PSM2, never reconstructed payload. */
    char signed_payload[7001], key_id[81], signature[87];
} pet_replace_operation_t;
#define PET_REMOVAL_MAX 3u
typedef struct {
    char id[37];
    pet_replace_pack_t pack;
} pet_replace_removal_t;
typedef struct {
    pet_control_context_t context;
    bool has_operation;
    pet_replace_operation_t operation;
    bool has_removals;
    unsigned removal_count;
    pet_replace_removal_t removals[PET_REMOVAL_MAX];
} pet_replace_poll_t;

/* Allocate these large outputs off the task stack. False clears the output.
 * Operation parsing requires account identity from authenticated control context.
 * Poll may bootstrap that context from the authenticated device endpoint; an
 * already-known account is checked when expected_account is non-NULL.
 * Signed identity alone is never an erase/activation acknowledgement. */
bool pet_replace_wire_operation(const char *json,size_t bytes,const char *device,const char *account,
                                const pet_pack_trust_key_t *keys,size_t key_count,pet_replace_operation_t *out);
bool pet_replace_wire_poll(const char *json,size_t bytes,const char *device,const char *expected_account,
                           const pet_pack_trust_key_t *keys,size_t key_count,pet_replace_poll_t *out);
bool pet_replace_wire_encode_poll(const char *boot_id,char *json,size_t capacity);
/* Only removal-capable workers negotiate this optional poll extension. */
bool pet_replace_wire_encode_removal_poll(const char *boot_id, const char ack[][37], unsigned count,
                                          char *json, size_t capacity);
bool pet_replace_wire_encode_report(const pet_replace_report_t *report,char *json,size_t capacity);
/* Three-pet selection: talk as the installed pet with this build and hash
 * (cloud device-protocol pet-select v2). Exactly {version,bootId,buildId,sha256}. The
 * reply is a v2 poll result, parsed with pet_replace_wire_poll. */
bool pet_replace_wire_encode_select(const char *boot_id,const char *build_id,const char *sha256,char *json,size_t capacity);
