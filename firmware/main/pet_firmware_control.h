#pragma once
#include "pet_control_http.h"
#include "pet_firmware_image.h"
#include "pet_firmware_wire.h"

typedef struct {
    pet_control_http_t http;
    const pet_pack_trust_key_t *keys;
    size_t key_count;
    char boot_id[37];
    uint32_t firmware_epoch;
    pet_firmware_bootloader_t bootloader;
    /* Called on the serialized worker. Failure means unknown, never no pet.
     * fresh: prove every pet from flash now (accepting an update, starting its
     * image, switching to it); otherwise the last proof of the same store
     * state may answer. */
    void (*protection)(void *context,bool fresh,pet_firmware_protection_t *out);
    /* Actual UI + storage/recovery + Wi-Fi evidence, independent of a pet's
     * socket. Control authentication is additionally established here. */
    pet_fw_storage_t (*health)(void *context);
    void (*freeze)(void *context);
    void (*restart)(void *context);
    uint64_t (*now_ms)(void *context); /* Monotonic; Retry-After starts at response completion. */
    void *context;
} pet_firmware_control_config_t;
typedef enum {PET_FW_CONTROL_IDLE,PET_FW_CONTROL_WORKING,PET_FW_CONTROL_WAITING,
              PET_FW_CONTROL_RECOVERY,PET_FW_CONTROL_RESTARTING} pet_firmware_control_status_t;
typedef struct {
    pet_firmware_control_config_t config;
    pet_firmware_receipt_store_t *store;
    pet_firmware_image_t image;
    pet_firmware_operation_t operation;
    bool has_operation,authenticated,writer_fault;
    uint64_t next_poll_ms,retry_at_ms;
    pet_firmware_control_status_t status;
    pet_control_download_t download; /* Its range connection, only while an image downloads. */
    char error_code[81];
    /* Allocate this bounded worker object off task stack (PSRAM on ESP32).
     * No response is retained as a secret, log entry or NVS blob. */
    char request[8192],response[PET_CONTROL_RESPONSE_MAX];
    uint8_t chunk[0x10000];
} pet_firmware_control_t;

/* No allocation, flash write or network during initialization. store may be
 * unloaded/corrupt: the independent recovery poll must still work. */
bool pet_firmware_control_init(pet_firmware_control_t *worker,const pet_firmware_control_config_t *config,
                               pet_firmware_receipt_store_t *store);
/* One serialized worker, under the same flash/runtime gate as pet replacement.
 * At most one bounded HTTP request per step. Saved reports (including terminal
 * reports) are replayed independently of the poll's current operation. */
pet_firmware_control_status_t pet_firmware_control_step(pet_firmware_control_t *worker,uint64_t now_ms);
bool pet_firmware_control_blocks_pet(const pet_firmware_control_t *worker);
/* Call even offline, outside transport backoff. Only a receipt-bound actual
 * target PENDING boot is reset at deadline; ambiguous state is not rewritten.
 * A normal restart lets the qualified bootloader preserve its rollback policy. */
bool pet_firmware_control_health_deadline(pet_firmware_control_t *worker,uint64_t elapsed_boot_ms);
