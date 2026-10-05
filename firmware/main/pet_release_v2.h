#pragma once
#include "pet_firmware_release.h"
#include "pet_replace.h"

/* Firmware epoch 3 accepts authored step timing (release sampling version 3),
 * the timing of Codex sprite packs. Firmware without it stays at epoch 2. */
#define PET_RELEASE_V2_AUTHORED_TIMING_EPOCH 3u
/* A three-pet (Pocket Terminal) release is at most 3,000,000 bytes, a little
 * under its slot's pack area, and needs firmware epoch 3 or later, as the
 * cloud's THREE_PET_MAX_RELEASE_BYTES and THREE_PET_MINIMUM_FIRMWARE_EPOCH. */
#define PET_RELEASE_V2_THREE_PET_MAX_BYTES 3000000u
#define PET_RELEASE_V2_THREE_PET_EPOCH 3u
#define PET_RELEASE_V2_TARGETED_EPOCH 5u

/* Compact authenticated facts; larger inventory, action and source documents
 * are bound by their signed hashes, not trusted from an unsigned HTTP copy.
 * An imported release (compiler null) signs the uploaded bytes unchanged: its
 * validated-import provenance replaces the compiler facts, which stay empty. */
typedef struct {
    pet_replace_pack_t pack;
    bool has_replacement_target;
    pet_replace_pack_t replacement_target;
    char account_id[37], project_id[37], face_id[81], version[81];
    char input_hash[65], character_fingerprint[65];
    char compiler_version[81], compiler_commit[41], compiler_sha256[65];
    char inventory_sha256[65], action_map_sha256[65], source_manifest_sha256[65];
    char approval_actor[37], approval_at[41];
    bool promoted_approval;
    char reviewed_sha256[65], candidate_id[37], approval_id[37];
    bool imported;
    char import_id[37], original_sha256[65], validation_sha256[65], configuration_sha256[65];
    pet_firmware_protected_pack_t requirements;
} pet_release_v2_t;

/* expected_account is authenticated control context, never the candidate's
 * accountId. expected_pack is the exact immutable cloud operation/journal
 * target. This verifies signature + schema + both bindings, not authorization
 * to erase, structural pack validity, or cloud activation acknowledgement. */
bool pet_release_v2_verify(const cJSON *envelope,const char *expected_account,
                           const pet_replace_pack_t *expected_pack,
                           const pet_pack_trust_key_t *keys,size_t key_count,
                           pet_release_v2_t *release);
/* Call with current independently observed layout/hash and qualified running
 * firmware facts. A zero/unqualified bootloader never permits installation. */
bool pet_release_v2_compatible(const pet_release_v2_t *release,
                               const pet_firmware_requirements_t *current);

/* Canonical PSM2 record occupies exactly the existing 8192-byte reservation.
 * It stores original signed payload bytes, key ID and signature, a SHA-256
 * transport checksum and erased padding. No credentials or private NVS.
 * Caller allocates the record off stack. Writers still need the durable fence,
 * invalidation and detach gates enforced by pet_replace_writer_manifest. */
bool pet_release_v2_record_encode(const cJSON *envelope,const char *expected_account,
                                  const pet_replace_pack_t *expected_pack,
                                  const pet_pack_trust_key_t *keys,size_t key_count,
                                  void *record,size_t record_bytes);
bool pet_release_v2_record_verify(const void *record,size_t record_bytes,
                                  const char *expected_account,const pet_replace_pack_t *expected_pack,
                                  const pet_pack_trust_key_t *keys,size_t key_count,
                                  pet_release_v2_t *release);
