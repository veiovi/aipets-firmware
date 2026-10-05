#include "pet_diagnostics.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_audio.h"
#include "pet_face.h"

#define DIAGNOSTICS_RTC_MAGIC 0x50455444u
#define DIAGNOSTICS_POLL_MS 250u
#define LISTENING_STALE_MS 3000u
#define SPEAKING_STALE_MS 5000u
/* Speaking with no playback is a stall only once the silence lasts: a reply
 * cut by a recoverable error stops playback a moment before the face moves on,
 * and recovering then replaced a working session. */
#define SPEAKING_SILENT_MS 1500u
#define THINKING_STALE_MS 60000u
#define DISPLAY_STALE_MS 5000u

typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
    uint32_t boot_id_high;
    uint32_t boot_id_low;
    uint32_t state;
    int32_t last_event;
    char last_event_name[24];
    uint32_t input_stream_id;
    uint32_t output_stream_id;
    uint32_t uptime_ms;
} diagnostics_rtc_breadcrumb_t;

static const char *TAG = "pet_diagnostics";
static RTC_NOINIT_ATTR diagnostics_rtc_breadcrumb_t s_rtc;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pet_diagnostics_snapshot_t s_snapshot;
static pet_diagnostics_recovery_callback_t s_recovery_callback;
static bool s_started;
static uint32_t s_state_changed_ms;
static uint32_t s_last_event_ms;
static uint32_t s_last_display_heartbeat_ms;
static uint32_t s_last_display_heartbeat;
static bool s_recovery_latched;
static bool s_crash_ack_pending; /* guarded by s_lock */
static atomic_uint s_native_ui_heartbeat;

void pet_diagnostics_note_native_ui_heartbeat(void)
{
    atomic_fetch_add(&s_native_ui_heartbeat, 1);
}

static uint32_t display_heartbeat(void)
{
    return pet_face_render_heartbeat() + atomic_load(&s_native_ui_heartbeat);
}

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

const char *pet_diagnostics_state_name(pet_face_state_t state)
{
    switch (state) {
        case PET_FACE_BOOTING: return "booting";
        case PET_FACE_PROVISIONING: return "provisioning";
        case PET_FACE_CONNECTING: return "connecting";
        case PET_FACE_IDLE: return "idle";
        case PET_FACE_LISTENING: return "listening";
        case PET_FACE_THINKING: return "thinking";
        case PET_FACE_SPEAKING: return "speaking";
        case PET_FACE_OFFLINE: return "offline";
        case PET_FACE_ERROR: return "error";
        default: return "unknown";
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN: return "unknown";
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external-pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt-watchdog";
        case ESP_RST_TASK_WDT: return "task-watchdog";
        case ESP_RST_WDT: return "other-watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power-glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
        default: return "unmapped";
    }
}

static void log_previous_breadcrumb(void)
{
    if (s_rtc.magic != DIAGNOSTICS_RTC_MAGIC || s_rtc.magic_inverse != ~DIAGNOSTICS_RTC_MAGIC) return;
    ESP_LOGW(TAG,
             "PREVIOUS_BOOT bootId=%08" PRIx32 "%08" PRIx32
             " state=%s lastEvent=%" PRId32 "(%s) inputStream=%" PRIu32
             " outputStream=%" PRIu32 " lastUptimeMs=%" PRIu32,
             s_rtc.boot_id_high, s_rtc.boot_id_low,
             pet_diagnostics_state_name((pet_face_state_t)s_rtc.state), s_rtc.last_event,
             s_rtc.last_event_name,
             s_rtc.input_stream_id, s_rtc.output_stream_id, s_rtc.uptime_ms);
}

static void update_breadcrumb(void)
{
    portENTER_CRITICAL(&s_lock);
    s_rtc.state = (uint32_t)s_snapshot.state;
    s_rtc.last_event = s_snapshot.last_event;
    strlcpy(s_rtc.last_event_name, s_snapshot.last_event_name,
            sizeof(s_rtc.last_event_name));
    s_rtc.input_stream_id = s_snapshot.input_stream_id;
    s_rtc.output_stream_id = s_snapshot.output_stream_id;
    s_rtc.uptime_ms = uptime_ms();
    portEXIT_CRITICAL(&s_lock);
}

/* ESP-IDF reports an erased core dump partition, whose size word reads
 * 0xFFFFFFFF, as ESP_ERR_INVALID_SIZE. That is no core dump rather than an
 * unreadable one; any other invalid size is still an error. */
static bool coredump_partition_erased(void)
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    uint32_t size = 0;
    return partition && esp_partition_read(partition, 0, &size, sizeof(size)) == ESP_OK && size == UINT32_MAX;
}

static void load_coredump_summary(void)
{
    esp_err_t check = esp_core_dump_image_check();
    if (check == ESP_ERR_NOT_FOUND || (check == ESP_ERR_INVALID_SIZE && coredump_partition_erased())) {
        ESP_LOGI(TAG, "no retained core dump");
        return;
    }
    if (check != ESP_OK) {
        ESP_LOGE(TAG, "retained core dump is unreadable: %s", esp_err_to_name(check));
        return;
    }

    s_snapshot.coredump_present = true;
    esp_core_dump_summary_t summary = {0};
    if (esp_core_dump_get_panic_reason(s_snapshot.panic_reason,
                                       sizeof(s_snapshot.panic_reason)) != ESP_OK) {
        strlcpy(s_snapshot.panic_reason, "panic reason unavailable",
                sizeof(s_snapshot.panic_reason));
    }
    esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "core dump summary unavailable: %s", esp_err_to_name(err));
        return;
    }

    strlcpy(s_snapshot.crash_task, summary.exc_task, sizeof(s_snapshot.crash_task));
    strlcpy(s_snapshot.crash_elf_sha, (const char *)summary.app_elf_sha256,
            sizeof(s_snapshot.crash_elf_sha));
    s_snapshot.crash_pc = summary.exc_pc;
    s_snapshot.crash_exception_cause = summary.ex_info.exc_cause;
    s_snapshot.crash_exception_address = summary.ex_info.exc_vaddr;
    s_snapshot.crash_backtrace_depth = summary.exc_bt_info.depth > PET_DIAGNOSTICS_BACKTRACE_DEPTH ?
        PET_DIAGNOSTICS_BACKTRACE_DEPTH : (uint8_t)summary.exc_bt_info.depth;
    s_snapshot.crash_backtrace_corrupted = summary.exc_bt_info.corrupted;
    memcpy(s_snapshot.crash_backtrace, summary.exc_bt_info.bt,
           s_snapshot.crash_backtrace_depth * sizeof(uint32_t));

    ESP_LOGE(TAG,
             "CRASH_SUMMARY reason=\"%s\" task=%s pc=0x%08" PRIx32
             " cause=%" PRIu32 " address=0x%08" PRIx32
             " elfSha=%s backtraceDepth=%u backtraceCorrupted=%d",
             s_snapshot.panic_reason, s_snapshot.crash_task, s_snapshot.crash_pc,
             s_snapshot.crash_exception_cause, s_snapshot.crash_exception_address,
             s_snapshot.crash_elf_sha, s_snapshot.crash_backtrace_depth,
             s_snapshot.crash_backtrace_corrupted);
    for (uint8_t i = 0; i < s_snapshot.crash_backtrace_depth; ++i) {
        ESP_LOGE(TAG, "CRASH_BACKTRACE[%u]=0x%08" PRIx32, i,
                 s_snapshot.crash_backtrace[i]);
    }
}

static uint32_t task_stack_free(const char *name)
{
#if INCLUDE_xTaskGetHandle == 1
    TaskHandle_t task = xTaskGetHandle(name);
    return task ? (uint32_t)uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t) : 0;
#else
    (void)name;
    return 0;
#endif
}

static void refresh_health_snapshot(uint32_t now)
{
    pet_face_state_t state = pet_face_get_state();
    if (state != s_snapshot.state) {
        ESP_LOGI(TAG, "STATE %s -> %s after %" PRIu32 "ms",
                 pet_diagnostics_state_name(s_snapshot.state),
                 pet_diagnostics_state_name(state), now - s_state_changed_ms);
        s_snapshot.state = state;
        s_state_changed_ms = now;
        s_recovery_latched = false;
    }
    s_snapshot.state_age_ms = now - s_state_changed_ms;
    s_snapshot.last_event_age_ms = now - s_last_event_ms;
    s_snapshot.capturing = pet_audio_is_capturing();
    s_snapshot.playing = pet_audio_is_playing();
    s_snapshot.min_free_heap = esp_get_minimum_free_heap_size();
    s_snapshot.min_free_internal_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_snapshot.min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    s_snapshot.largest_internal_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_snapshot.largest_psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    s_snapshot.app_stack_free = task_stack_free("pet_app");
    s_snapshot.capture_stack_free = task_stack_free("pet_capture");
    s_snapshot.playback_stack_free = task_stack_free("pet_playback");
    s_snapshot.audio_tx_stack_free = task_stack_free("pet_audio_tx");
    s_snapshot.telemetry_stack_free = task_stack_free("pet_telemetry");
    s_snapshot.lvgl_stack_free = task_stack_free("LVGL task");
    if (!s_snapshot.lvgl_stack_free) s_snapshot.lvgl_stack_free = task_stack_free("lvgl");
    s_snapshot.websocket_stack_free = task_stack_free("websocket_task");

    uint32_t heartbeat = display_heartbeat();
    if (heartbeat != s_last_display_heartbeat) {
        s_last_display_heartbeat = heartbeat;
        s_last_display_heartbeat_ms = now;
    }
    s_snapshot.display_heartbeat_age_ms = now - s_last_display_heartbeat_ms;
    update_breadcrumb();
}

static void request_recovery(pet_diagnostics_recovery_t reason, const char *label)
{
    if (s_recovery_latched) return;
    s_recovery_latched = true;
    ESP_LOGE(TAG,
             "HEALTH_RECOVERY reason=%s state=%s stateAgeMs=%" PRIu32
             " event=%" PRId32 "(%s) eventAgeMs=%" PRIu32
             " capturing=%d playing=%d inputStream=%" PRIu32
             " outputStream=%" PRIu32 " queueDepth=%" PRIu32
             " droppedEvents=%" PRIu32 " displayHeartbeatAgeMs=%" PRIu32,
             label, pet_diagnostics_state_name(s_snapshot.state),
             s_snapshot.state_age_ms, s_snapshot.last_event,
             s_snapshot.last_event_name,
             s_snapshot.last_event_age_ms, s_snapshot.capturing, s_snapshot.playing,
             s_snapshot.input_stream_id, s_snapshot.output_stream_id,
             s_snapshot.app_queue_depth, s_snapshot.dropped_app_events,
             s_snapshot.display_heartbeat_age_ms);
    if (s_recovery_callback) s_recovery_callback(reason);
}

static bool s_speaking_silent;
static uint32_t s_speaking_silent_since_ms;

static bool speaking_stalled(uint32_t now)
{
    if (s_snapshot.state != PET_FACE_SPEAKING || s_snapshot.playing) {
        s_speaking_silent = false;
        return false;
    }
    if (!s_speaking_silent) {
        s_speaking_silent = true;
        s_speaking_silent_since_ms = now;
    }
    return s_snapshot.state_age_ms > SPEAKING_STALE_MS && now - s_speaking_silent_since_ms >= SPEAKING_SILENT_MS;
}

static void diagnostics_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAGNOSTICS_POLL_MS));
        uint32_t now = uptime_ms();
        refresh_health_snapshot(now);
        if (s_snapshot.display_heartbeat_age_ms > DISPLAY_STALE_MS) {
            request_recovery(PET_DIAGNOSTICS_RECOVERY_DISPLAY_STALLED, "display-stalled");
        } else if (s_snapshot.state == PET_FACE_LISTENING && !s_snapshot.capturing &&
                   s_snapshot.state_age_ms > LISTENING_STALE_MS) {
            request_recovery(PET_DIAGNOSTICS_RECOVERY_LISTENING_STALE, "listening-without-capture");
        } else if (speaking_stalled(now)) {
            request_recovery(PET_DIAGNOSTICS_RECOVERY_SPEAKING_STALE, "speaking-without-playback");
        } else if (s_snapshot.state == PET_FACE_THINKING &&
                   s_snapshot.state_age_ms > THINKING_STALE_MS) {
            request_recovery(PET_DIAGNOSTICS_RECOVERY_THINKING_STALE, "thinking-timeout");
        }
    }
}

esp_err_t pet_diagnostics_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_crash_ack_pending = false;
    s_snapshot.last_event = -1;
    strlcpy(s_snapshot.last_event_name, "none", sizeof(s_snapshot.last_event_name));
    s_snapshot.state = PET_FACE_BOOTING;
    uint32_t boot_high = esp_random();
    uint32_t boot_low = esp_random();
    snprintf(s_snapshot.boot_id, sizeof(s_snapshot.boot_id), "%08" PRIx32 "%08" PRIx32,
             boot_high, boot_low);
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGW(TAG, "BOOT_DIAGNOSTICS bootId=%s resetReason=%d(%s)",
             s_snapshot.boot_id, reason, reset_reason_name(reason));
    log_previous_breadcrumb();
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = DIAGNOSTICS_RTC_MAGIC;
    s_rtc.magic_inverse = ~DIAGNOSTICS_RTC_MAGIC;
    s_rtc.boot_id_high = boot_high;
    s_rtc.boot_id_low = boot_low;
    s_state_changed_ms = uptime_ms();
    s_last_event_ms = s_state_changed_ms;
    s_last_display_heartbeat_ms = s_state_changed_ms;
    load_coredump_summary();
    update_breadcrumb();
    return ESP_OK;
}

esp_err_t pet_diagnostics_start(pet_diagnostics_recovery_callback_t recovery_callback)
{
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_recovery_callback = recovery_callback;
    s_last_display_heartbeat = display_heartbeat();
    s_last_display_heartbeat_ms = uptime_ms();
    if (xTaskCreate(diagnostics_task, "pet_health", 5120, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

const char *pet_diagnostics_boot_id(void)
{
    return s_snapshot.boot_id;
}

void pet_diagnostics_note_app_event(int32_t event, const char *event_name,
                                    uint32_t queue_depth)
{
    uint32_t now = uptime_ms();
    portENTER_CRITICAL(&s_lock);
    s_snapshot.last_event = event;
    strlcpy(s_snapshot.last_event_name, event_name ? event_name : "unknown",
            sizeof(s_snapshot.last_event_name));
    s_snapshot.app_queue_depth = queue_depth;
    s_last_event_ms = now;
    portEXIT_CRITICAL(&s_lock);
    update_breadcrumb();
}

void pet_diagnostics_note_streams(uint32_t input_stream_id, uint32_t output_stream_id)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.input_stream_id = input_stream_id;
    s_snapshot.output_stream_id = output_stream_id;
    portEXIT_CRITICAL(&s_lock);
    update_breadcrumb();
}

void pet_diagnostics_note_dropped_app_event(void)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.dropped_app_events++;
    portEXIT_CRITICAL(&s_lock);
}

void pet_diagnostics_get_snapshot(pet_diagnostics_snapshot_t *snapshot)
{
    if (!snapshot) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t pet_diagnostics_acknowledge_crash(const char *elf_sha, uint32_t pc)
{
    size_t length = elf_sha ? strnlen(elf_sha, PET_DIAGNOSTICS_CRASH_ELF_SHA_MAX + 1) : 0;
    if (!length || length > PET_DIAGNOSTICS_CRASH_ELF_SHA_MAX) {
        ESP_LOGW(TAG, "CRASH_ACK ignored: malformed crash identity");
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    bool matches = s_snapshot.coredump_present && s_snapshot.crash_pc == pc &&
                   !strcmp(s_snapshot.crash_elf_sha, elf_sha);
    if (matches) s_crash_ack_pending = true;
    portEXIT_CRITICAL(&s_lock);
    if (matches) return ESP_OK;
    ESP_LOGW(TAG, "CRASH_ACK ignored: elfSha=%s pc=0x%08" PRIx32
             " is not the stored crash summary", elf_sha, pc);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t pet_diagnostics_clear_acknowledged_crash(void)
{
    char elf_sha[PET_DIAGNOSTICS_CRASH_ELF_SHA_MAX + 1];
    portENTER_CRITICAL(&s_lock);
    bool pending = s_crash_ack_pending;
    s_crash_ack_pending = false;
    strlcpy(elf_sha, s_snapshot.crash_elf_sha, sizeof(elf_sha));
    uint32_t pc = s_snapshot.crash_pc;
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return ESP_ERR_NOT_FOUND;
    /* Outside the critical section: erasing the partition blocks on flash. */
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CRASH_ACK core dump erase failed: %s; the summary stays",
                 esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_snapshot.coredump_present = false;
    memset(s_snapshot.panic_reason, 0, sizeof(s_snapshot.panic_reason));
    memset(s_snapshot.crash_task, 0, sizeof(s_snapshot.crash_task));
    memset(s_snapshot.crash_elf_sha, 0, sizeof(s_snapshot.crash_elf_sha));
    s_snapshot.crash_pc = 0;
    s_snapshot.crash_exception_cause = 0;
    s_snapshot.crash_exception_address = 0;
    memset(s_snapshot.crash_backtrace, 0, sizeof(s_snapshot.crash_backtrace));
    s_snapshot.crash_backtrace_depth = 0;
    s_snapshot.crash_backtrace_corrupted = false;
    /* A repeated acknowledgement during the erase has nothing left to erase. */
    s_crash_ack_pending = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "CRASH_CLEARED elfSha=%s pc=0x%08" PRIx32
             ": the cloud stored the summary, so the core dump was erased", elf_sha, pc);
    return ESP_OK;
}
