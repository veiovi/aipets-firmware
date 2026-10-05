#pragma once
#include "pet_firmware_release.h"

/* Separate redundant private-NVS records, never the pet activation journal.
 * All fields are release/update metadata; credentials and saved settings do not
 * belong here. Every changed report is persisted BEFORE network delivery. */
#define PET_FIRMWARE_RECEIPT_BYTES 768u
typedef enum {PET_FW_EMPTY,PET_FW_DOWNLOADING,PET_FW_STAGED,PET_FW_REBOOTING,
              PET_FW_HEALTHY,PET_FW_FAILED,PET_FW_ROLLED_BACK} pet_fw_phase_t;
typedef enum {PET_FW_STORAGE_NONE,PET_FW_STORAGE_PACK,PET_FW_STORAGE_SETUP,PET_FW_STORAGE_RECOVERY} pet_fw_storage_t;
typedef struct {
    pet_fw_phase_t phase;
    uint32_t image_bytes,downloaded_bytes,sequence,acknowledged_sequence;
    uint32_t firmware_epoch,target_offset;
    pet_layout_id_t layout_id;
    bool image_verified;
    pet_fw_storage_t healthy_storage;
    char operation_id[37],release_id[37],target_sha256[65],previous_sha256[65];
    char previous_boot_id[37],report_boot_id[37],running_sha256[65],partition_sha256[65];
    char firmware_version[81],error_code[81];
} pet_firmware_receipt_t;
typedef struct {
    int (*read)(void *context,unsigned slot,uint8_t record[PET_FIRMWARE_RECEIPT_BYTES]); /* 0 missing, 1 exact, -1 error */
    bool (*write)(void *context,unsigned slot,const uint8_t record[PET_FIRMWARE_RECEIPT_BYTES]); /* durable commit */
    void *context;
} pet_firmware_receipt_io_t;
typedef struct {
    pet_firmware_receipt_io_t io;
    pet_firmware_receipt_t value;
    uint64_t generation;
    unsigned slot;
    bool loaded;
} pet_firmware_receipt_store_t;

bool pet_firmware_receipt_valid(const pet_firmware_receipt_t *receipt);
bool pet_firmware_receipt_open(pet_firmware_receipt_store_t *store,const pet_firmware_receipt_io_t *io);
/* Failure is ambiguous: loaded becomes false; reopen before doing anything.
 * No mutation is safe merely because a new value exists in caller memory. */
bool pet_firmware_receipt_save(pet_firmware_receipt_store_t *store,const pet_firmware_receipt_t *next);
bool pet_firmware_receipt_begin(pet_firmware_receipt_t *receipt,const pet_firmware_release_t *release,
                                const char *operation_id,const char *previous_sha256,const char *boot_id,uint32_t target_offset);
/* A queued operation may be accepted after a power cycle. Its immutable
 * previous boot remains cloud-bound; the first receipt names the actual boot. */
bool pet_firmware_receipt_begin_at_boot(pet_firmware_receipt_t *receipt,const pet_firmware_release_t *release,
                                        const char *operation_id,const char *previous_sha256,
                                        const char *previous_boot_id,const char *current_boot_id,uint32_t target_offset);
bool pet_firmware_receipt_progress(pet_firmware_receipt_t *receipt,uint32_t downloaded,const char *boot_id);
/* A download restart reads from zero but never regresses the cloud high-water
 * mark. Only the independently checked complete image hash permits staging. */
bool pet_firmware_receipt_stage(pet_firmware_receipt_t *receipt,const char *verified_sha256);
bool pet_firmware_receipt_reboot(pet_firmware_receipt_t *receipt);
/* After STAGED recovery, exact cloud sequence reconciliation must prove no
 * later reboot report exists before recording a new selection boot. */
bool pet_firmware_receipt_reboot_at_boot(pet_firmware_receipt_t *receipt,const char *boot_id);
/* Real checks precede esp_ota_mark_app_valid_cancel_rollback. Persist HEALTHY
 * only after that succeeds, so a lost cloud ACK cannot later claim health for
 * an image that the bootloader has already rolled back. */
bool pet_firmware_receipt_healthy(pet_firmware_receipt_t *receipt,const char *boot_id,const char *running_sha256,
                                 bool ui_ready,bool wifi_ready,bool control_authenticated,bool application_validated,pet_fw_storage_t storage);
bool pet_firmware_receipt_fail(pet_firmware_receipt_t *receipt,bool rolled_back,
                              const char *boot_id,const char *running_sha256,const char *error_code);
bool pet_firmware_receipt_ack(pet_firmware_receipt_t *receipt,uint32_t exact_sequence);
const char *pet_firmware_receipt_status(const pet_firmware_receipt_t *receipt);
