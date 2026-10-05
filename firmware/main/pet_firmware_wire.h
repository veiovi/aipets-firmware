#pragma once
#include "pet_firmware_receipt.h"

typedef struct {
    pet_fw_phase_t phase; /* EMPTY means queued; STAGED is local-only. */
    uint32_t sequence,downloaded_bytes;
    pet_fw_storage_t healthy_storage;
    char boot_id[37],running_sha256[65],error_code[81];
} pet_firmware_report_t;
typedef struct {
    pet_firmware_release_t release;
    char id[37],request_id[37],previous_sha256[65],previous_boot_id[37];
    pet_firmware_report_t report;
} pet_firmware_operation_t;
typedef struct {
    char revision[37];
    bool has_operation;
    pet_firmware_operation_t operation;
} pet_firmware_poll_t;

/* Decode only bounded, duplicate-free JSON. The release envelope is checked
 * with pinned keys; its unsigned convenience copy must match the signed
 * payload. The expected device comes from local enrollment, not the response. */
bool pet_firmware_wire_poll(const char *json,size_t bytes,const char *device_id,
                            const pet_pack_trust_key_t *keys,size_t key_count,pet_firmware_poll_t *out);
bool pet_firmware_wire_operation(const char *json,size_t bytes,const char *device_id,
                                 const pet_pack_trust_key_t *keys,size_t key_count,pet_firmware_operation_t *out);
bool pet_firmware_wire_matches_receipt(const pet_firmware_operation_t *operation,const pet_firmware_receipt_t *receipt);
/* Identity only, not an ACK. Cloud may lag a durable local report; a caller
 * must reconcile sequence/payload separately and fence cloud-ahead conflicts. */
bool pet_firmware_wire_identity_matches(const pet_firmware_operation_t *operation,const pet_firmware_receipt_t *receipt);
/* Encoding never accepts a caller-provided URL, provider key or Wi-Fi config.
 * Unknown protection is {known:false} and keeps recovery polling possible. A
 * three-pet layout also reports `installed`, every other installed pet. */
bool pet_firmware_wire_encode_poll(const pet_firmware_requirements_t *current,const char *running_sha256,
                                   const char *boot_id,const pet_firmware_protection_t *protection,
                                   char *json,size_t capacity);
/* Explicit minimal recovery poll when physical layout or image is unreadable.
 * No fabricated capabilities, running hash or pack protection. Never a download
 * grant; signed pending identity and saved report replay remain available. */
bool pet_firmware_wire_encode_recovery_poll(const char *boot_id,char *json,size_t capacity);
bool pet_firmware_wire_encode_report(const pet_firmware_receipt_t *receipt,char *json,size_t capacity);
