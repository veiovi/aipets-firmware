#pragma once

#include "pet_control_wire.h"

typedef struct {
    const char *key_id;
    uint8_t public_point[65]; /* SEC1 uncompressed 04 || X || Y, P-256 */
} pet_pack_trust_key_t;
typedef struct {
    char account_id[37], project_id[37], face_id[81], version[81];
    uint32_t bytes;
} pet_pack_verified_manifest_t;

/* Exact bounded ES256 bytes, shared by versioned pet and firmware envelopes.
 * Signature verification alone does not authorize a payload kind or device. */
bool pet_release_verify_signature(const char *payload, size_t payload_capacity,
                                  const char *key_id, size_t key_id_capacity,
                                  const char *signature, size_t signature_capacity,
                                  const pet_pack_trust_key_t *keys, size_t key_count);

/* Trust keys are pinned release inputs, never taken from the HTTP response.
 * expected_account comes from authenticated context, not the signed candidate. */
bool pet_pack_verify_manifest(const pet_control_operation_t *operation,
                               const char *expected_account,
                               const pet_pack_trust_key_t *keys, size_t key_count,
                               pet_pack_verified_manifest_t *manifest);
bool pet_pack_verify_bytes(const void *bytes, size_t length, const char *expected_sha256);
