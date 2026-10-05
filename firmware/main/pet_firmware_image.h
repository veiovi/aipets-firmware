#pragma once
#include "pet_firmware_receipt.h"
#include "pet_ota_selection.h"
#include "esp_ota_ops.h"

typedef struct {
    pet_flash_layout_t layout;
    char partition_sha256[65];
    pet_ota_selection_t selection;
    uint32_t running_offset;
    unsigned running_slot;
} pet_firmware_boot_state_t;

typedef struct {
    const esp_partition_t *partition;
    pet_firmware_receipt_store_t *receipt;
    esp_ota_handle_t handle;
    uint32_t written,image_bytes;
    char operation_id[37],sha256[65],version[81];
    bool active;
} pet_firmware_image_t;

/* Serialized control worker only, while it exclusively owns flash. These
 * functions never alter NVS, the pet partition or the partition table.
 * Release must be the freshly signature-verified exact cloud operation.
 * begin requires a persisted/acknowledged receipt BEFORE erasing the inactive
 * application. On restart firmware download starts from zero; cloud progress
 * remains a high-water mark. Pet download checkpoints are a separate protocol. */
bool pet_firmware_image_begin(pet_firmware_image_t *image,pet_firmware_receipt_store_t *receipt,
                              const pet_firmware_release_t *release,const pet_firmware_bootloader_t *bootloader,
                              const pet_firmware_protection_t *protection);
bool pet_firmware_image_write(pet_firmware_image_t *image,uint32_t offset,const void *bytes,size_t length);
/* Complete file read-back SHA, IDF image validation, exact image length,
 * project/version identity. Success permits the caller to persist STAGED. */
bool pet_firmware_image_finish(pet_firmware_image_t *image);
bool pet_firmware_image_abort(pet_firmware_image_t *image);
/* Requires durable REBOOTING receipt and exact cloud ACK; re-verifies signed
 * requirements, layout and target flash image. No reboot or healthy report is
 * implied. A false result may follow an ambiguous otadata write: retain the
 * receipt and reconcile boot selection; never report an unexamined failure. */
bool pet_firmware_image_select(pet_firmware_receipt_store_t *receipt,const pet_firmware_release_t *release,
                               const char *current_boot_id,const pet_firmware_bootloader_t *bootloader,
                               const pet_firmware_protection_t *protection);
/* Exact complete app-file hash, not esp_app_desc.app_elf_sha256 and not the
 * appended image digest (which omits its own bytes). Read-only; computed once
 * per boot. */
bool pet_firmware_image_running(char sha256[65],uint32_t *bytes);
/* Read-only qualification used before advertising a bootloader capability. */
bool pet_firmware_image_bootloader_matches(const pet_firmware_bootloader_t *trusted_profile);
/* Physical layout + both raw OTA records, never get_state_partition's first
 * matching record or get_boot_partition's implicit fallback. Read-only. */
bool pet_firmware_image_boot_state(pet_firmware_boot_state_t *state);
/* Worker must first establish real UI/storage/Wi-Fi/control health and own the
 * flash gate. Marks only a proven active/running PENDING image, then verifies
 * the same sequence is VALID. Already VALID is idempotent. Never rewrites boot
 * selection to "restore" a previous app; ambiguity remains fenced. */
bool pet_firmware_image_confirm(uint32_t expected_offset,const char *expected_sha256,
                                const pet_firmware_bootloader_t *trusted_profile);
