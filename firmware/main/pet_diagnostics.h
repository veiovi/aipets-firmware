#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_face.h"

#define PET_DIAGNOSTICS_BOOT_ID_LENGTH 17
#define PET_DIAGNOSTICS_BACKTRACE_DEPTH 16
#define PET_DIAGNOSTICS_CRASH_ELF_SHA_MAX 64

typedef enum {
    PET_DIAGNOSTICS_RECOVERY_LISTENING_STALE,
    PET_DIAGNOSTICS_RECOVERY_SPEAKING_STALE,
    PET_DIAGNOSTICS_RECOVERY_THINKING_STALE,
    PET_DIAGNOSTICS_RECOVERY_DISPLAY_STALLED,
} pet_diagnostics_recovery_t;

typedef void (*pet_diagnostics_recovery_callback_t)(pet_diagnostics_recovery_t reason);

typedef struct {
    char boot_id[PET_DIAGNOSTICS_BOOT_ID_LENGTH];
    pet_face_state_t state;
    uint32_t state_age_ms;
    int32_t last_event;
    char last_event_name[32];
    uint32_t last_event_age_ms;
    uint32_t input_stream_id;
    uint32_t output_stream_id;
    bool capturing;
    bool playing;
    uint32_t app_queue_depth;
    uint32_t dropped_app_events;
    uint32_t min_free_heap;
    uint32_t min_free_internal_heap;
    uint32_t min_free_psram;
    uint32_t largest_internal_block;
    uint32_t largest_psram_block;
    uint32_t app_stack_free;
    uint32_t capture_stack_free;
    uint32_t playback_stack_free;
    uint32_t audio_tx_stack_free;
    uint32_t telemetry_stack_free;
    uint32_t lvgl_stack_free;
    uint32_t websocket_stack_free;
    uint32_t display_heartbeat_age_ms;
    bool coredump_present;
    char panic_reason[128];
    char crash_task[16];
    char crash_elf_sha[PET_DIAGNOSTICS_CRASH_ELF_SHA_MAX + 1];
    uint32_t crash_pc;
    uint32_t crash_exception_cause;
    uint32_t crash_exception_address;
    uint32_t crash_backtrace[PET_DIAGNOSTICS_BACKTRACE_DEPTH];
    uint8_t crash_backtrace_depth;
    bool crash_backtrace_corrupted;
} pet_diagnostics_snapshot_t;

esp_err_t pet_diagnostics_init(void);
esp_err_t pet_diagnostics_start(pet_diagnostics_recovery_callback_t recovery_callback);
const char *pet_diagnostics_boot_id(void);
const char *pet_diagnostics_state_name(pet_face_state_t state);
void pet_diagnostics_note_app_event(int32_t event, const char *event_name,
                                    uint32_t queue_depth);
void pet_diagnostics_note_streams(uint32_t input_stream_id, uint32_t output_stream_id);
void pet_diagnostics_note_dropped_app_event(void);
/* Only the visible native screen's LVGL timer may advance this heartbeat.
 * A hidden setup screen must not mask a stopped pet animation timer. */
void pet_diagnostics_note_native_ui_heartbeat(void);
void pet_diagnostics_get_snapshot(pet_diagnostics_snapshot_t *snapshot);
/* The gateway's pet.crash.acknowledged: the cloud has stored the crash summary
 * identified by elf_sha and pc. Only an exact match of the stored summary
 * schedules its erase (ESP_OK); anything else is ignored with a warning
 * (ESP_ERR_INVALID_ARG when malformed, ESP_ERR_NOT_FOUND otherwise). It never
 * touches flash, so the websocket task may call it. */
esp_err_t pet_diagnostics_acknowledge_crash(const char *elf_sha, uint32_t pc);
/* Erases an acknowledged core dump and clears the crash summary, logging one
 * line. Call it from a task that may block for the flash erase. Returns
 * ESP_ERR_NOT_FOUND when nothing is acknowledged; a failed erase keeps the
 * summary for a later acknowledgement. */
esp_err_t pet_diagnostics_clear_acknowledged_crash(void);
