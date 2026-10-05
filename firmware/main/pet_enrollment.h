#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure C enrollment engine. One worker owns an instance and all callbacks.
 * Records contain credentials: private NVS only, never logs or telemetry. */
#define PET_ENROLLMENT_RECORD_BYTES 72
#define PET_ENROLLMENT_DEVICE_ID_BYTES 37
#define PET_ENROLLMENT_CREDENTIAL_BYTES 44
#define PET_ENROLLMENT_CODE_LENGTH 4
#define PET_ENROLLMENT_CODE_BYTES (PET_ENROLLMENT_CODE_LENGTH + 1)

typedef enum {
    PET_ENROLL_COMPLETE,
    PET_ENROLL_NEEDS_CODE,
    PET_ENROLL_RETRY,
    PET_ENROLL_REJECTED,
    PET_ENROLL_STORAGE_ERROR,
    PET_ENROLL_INVALID_CODE,
    PET_ENROLL_WAIT_WIFI,
    PET_ENROLL_WAIT_CLOCK,
    /* The setup worker holds a typed code until the cloud's wait ends. */
    PET_ENROLL_WAIT_CLOUD,
} pet_enrollment_result_t;

typedef enum {
    PET_ENROLL_REPLY_COMPLETE,
    PET_ENROLL_REPLY_CONFIRM_REQUIRED,
    PET_ENROLL_REPLY_NOT_CLAIMED,
    PET_ENROLL_REPLY_RETRY,
    PET_ENROLL_REPLY_REJECTED,
} pet_enrollment_reply_t;

typedef struct {
    /* read: 1 = exact record, 0 = key absent, -1 = I/O/length failure.
     * write: true only after durable commit. Failure may be ambiguous. */
    int (*read)(void *context, unsigned slot, uint8_t record[PET_ENROLLMENT_RECORD_BYTES]);
    bool (*write)(void *context, unsigned slot, const uint8_t record[PET_ENROLLMENT_RECORD_BYTES]);
    bool (*random)(void *context, uint8_t *bytes, size_t length);
    pet_enrollment_reply_t (*request)(void *context, bool redeem,
                                     const char *device_id, const char *credential,
                                     const char *code);
    void *context;
} pet_enrollment_io_t;

typedef struct {
    pet_enrollment_io_t io;
    uint8_t identity[16];
    uint8_t secret[32];
    uint64_t generation;
    bool complete;
    bool loaded;
    bool may_redeem;
} pet_enrollment_t;

/* Load or create redundant records; no network calls. Never replaces a corrupt
 * existing identity. Call again after a storage error before retrying any action. */
pet_enrollment_result_t pet_enrollment_open(pet_enrollment_t *state,
                                           const pet_enrollment_io_t *io);
/* Completion: persists COMPLETE. An explicit NOT_CLAIMED permits redemption.
 * Without a number it finishes only a claim without one, a typed code's: the
 * cloud refuses it for a claim of the device's own code (SETUP_CLAIM_CHANGED).
 * The setup worker calls it for an identity already complete, after a tap on
 * Link (with the number), after its own redeem, once setup/status answered
 * complete, and while link codes are off. Otherwise it permits redemption
 * (may_redeem) after setup/status answered NOT_CLAIMED. */
pet_enrollment_result_t pet_enrollment_resume(pet_enrollment_t *state);
pet_enrollment_result_t pet_enrollment_submit(pet_enrollment_t *state, const char *code);
bool pet_enrollment_code_valid(const char *code);
bool pet_enrollment_identity(const pet_enrollment_t *state,
                             char device_id[PET_ENROLLMENT_DEVICE_ID_BYTES],
                             char credential[PET_ENROLLMENT_CREDENTIAL_BYTES]);
void pet_enrollment_clear(void *data, size_t bytes);
