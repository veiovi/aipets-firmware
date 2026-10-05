#pragma once
#include "pet_flash_layout.h"
#include "pet_pack_verify.h"
/* Exact qualified rollback-enabled deployed bytes, from trusted release/build
 * inputs. An arbitrary observed hash is not qualification. */
typedef struct {uint32_t bytes;char sha256[65];} pet_firmware_bootloader_t;

/* Compact validated facts. No private key, URL, credential, Wi-Fi or NVS image
 * belongs in a generic firmware release. Masks use (1u << format/codec ID).
 * imported_release is renderer.importedRelease: the firmware verifies imported
 * pet releases. Formats, codecs or epoch alone never imply it. */
typedef struct {
    pet_flash_layout_t layout;
    char partition_sha256[65];
    uint32_t firmware_epoch;
    uint8_t formats, codecs;
    bool imported_release;
    pet_firmware_bootloader_t bootloader; /* bytes=0: unqualified current bootloader; never valid in a release. */
} pet_firmware_requirements_t;
typedef struct {
    char release_id[37], version[81], sha256[65];
    char artifact_manifest_sha256[65], source_commit[41];
    uint32_t bytes;
    pet_firmware_requirements_t requirements;
} pet_firmware_release_t;
typedef struct {
    bool present;
    pet_layout_id_t layout_id;
    char partition_sha256[65];
    uint32_t minimum_firmware_epoch, bytes;
    uint8_t format, resolution_divisor, codecs; /* resolution=120*divisor */
    bool imported_release; /* requirements.importedRelease */
} pet_firmware_protected_pack_t;
typedef struct {
    pet_firmware_protected_pack_t requirements;
    char build_id[37],sha256[65];
} pet_firmware_pack_identity_t;
/* Every pet a firmware update must keep readable. known=false means UNKNOWN,
 * never "no pet". A three-pet layout also lists each other installed pet, so
 * every slot is covered; single-pet layouts list none. */
#define PET_FIRMWARE_INSTALLED_MAX 3u
typedef struct {
    bool known;
    pet_firmware_pack_identity_t active,interrupted;
    unsigned installed_count;
    pet_firmware_pack_identity_t installed[PET_FIRMWARE_INSTALLED_MAX];
} pet_firmware_protection_t;

/* Input object must come from pet_control_json (bounded, duplicate-free JSON).
 * This function independently verifies the exact payload bytes and kind. */
bool pet_firmware_release_verify(const cJSON *envelope,const pet_pack_trust_key_t *keys,
                                 size_t key_count,pet_firmware_release_t *release);
/* Every protected pack protects rollback: active, interrupted and, on a
 * three-pet layout, each other installed pet, at most one per slot. An
 * imported pack needs a release that also verifies imported releases.
 * Unknown protection always rejects application changes but does not
 * disable control. */
bool pet_firmware_release_compatible(const pet_firmware_release_t *release,
                                     const pet_flash_layout_t *physical_layout,
                                     const char *partition_sha256,const pet_firmware_bootloader_t *verified_bootloader,
                                     const pet_firmware_protection_t *protection);
