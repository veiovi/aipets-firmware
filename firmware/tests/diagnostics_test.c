/* Runs the real pet_diagnostics.c against stubbed ESP-IDF core dump, flash and
 * FreeRTOS boundaries: the boot summary, the exact-match acknowledgement and
 * the erase that clears it. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
static size_t strlcpy(char *to, const char *from, size_t size)
{
    size_t length = strlen(from);
    if (size) {
        size_t copied = length < size - 1 ? length : size - 1;
        memcpy(to, from, copied);
        to[copied] = '\0';
    }
    return length;
}
#endif
#include "../main/pet_diagnostics.c"

#define SHA "9dc3a7b1f"
#define PC 0x4037c3fau

static int critical_depth;
static esp_err_t image_check = ESP_OK, summary_result = ESP_OK, erase_result = ESP_OK;
static unsigned erase_calls;
static bool acknowledge_during_erase;
static esp_core_dump_summary_t stored;
static char log_lines[64][256];
static char log_levels[64];
static unsigned log_count;

void pet_test_critical_enter(portMUX_TYPE *mux)
{
    assert(critical_depth == 0 && mux->depth == 0);
    ++critical_depth;
    ++mux->depth;
}
void pet_test_critical_exit(portMUX_TYPE *mux)
{
    assert(critical_depth == 1 && mux->depth == 1);
    --critical_depth;
    --mux->depth;
}
void pet_test_log(char level, const char *tag, const char *format, ...)
{
    (void)tag;
    assert(log_count < sizeof(log_lines) / sizeof(log_lines[0]));
    va_list args;
    va_start(args, format);
    vsnprintf(log_lines[log_count], sizeof(log_lines[0]), format, args);
    va_end(args);
    log_levels[log_count++] = level;
}
esp_err_t esp_core_dump_image_check(void) { return image_check; }
/* The core dump partition's first word: its stored size, 0xFFFFFFFF when erased. */
static uint32_t coredump_size_word = 0xFFFFFFFFu;
static const esp_partition_t coredump_partition = {0x10000};
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label)
{
    (void)label;
    return type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_COREDUMP ? &coredump_partition : NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst, size_t size)
{
    assert(partition == &coredump_partition && src_offset == 0 && size == sizeof(coredump_size_word));
    memcpy(dst, &coredump_size_word, size);
    return ESP_OK;
}
esp_err_t esp_core_dump_get_panic_reason(char *reason, size_t size)
{
    snprintf(reason, size, "Stack overflow in task websocket_task has been detected.");
    return ESP_OK;
}
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *summary)
{
    if (summary_result == ESP_OK) *summary = stored;
    return summary_result;
}
esp_err_t esp_core_dump_image_erase(void)
{
    assert(critical_depth == 0); /* A blocking flash erase never runs under the spinlock. */
    ++erase_calls;
    /* The same acknowledgement repeated from another task while flash is busy. */
    if (acknowledge_during_erase) assert(pet_diagnostics_acknowledge_crash(SHA, PC) == ESP_OK);
    if (erase_result == ESP_OK) image_check = ESP_ERR_NOT_FOUND;
    return erase_result;
}
pet_face_state_t pet_face_get_state(void) { return PET_FACE_IDLE; }
uint32_t pet_face_render_heartbeat(void) { return 0; }
bool pet_audio_is_capturing(void) { return false; }
bool pet_audio_is_playing(void) { return false; }
int64_t esp_timer_get_time(void) { return 1000000; }
uint32_t esp_get_minimum_free_heap_size(void) { return 0; }
size_t heap_caps_get_minimum_free_size(unsigned capabilities) { (void)capabilities; return 0; }
size_t heap_caps_get_largest_free_block(unsigned capabilities) { (void)capabilities; return 0; }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_PANIC; }
uint32_t esp_random(void) { return 0x5eed; }
int xTaskCreate(void (*task)(void *), const char *name, uint32_t stack, void *arg,
                unsigned priority, TaskHandle_t *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; }

static void boot(esp_err_t check, esp_err_t summary)
{
    image_check = check;
    summary_result = summary;
    erase_result = ESP_OK;
    erase_calls = 0;
    acknowledge_during_erase = false;
    memset(&stored, 0, sizeof(stored));
    strlcpy(stored.exc_task, "websocket_task", sizeof(stored.exc_task));
    stored.exc_pc = PC;
    stored.exc_bt_info.bt[0] = PC;
    stored.exc_bt_info.bt[1] = 0x42001234u;
    stored.exc_bt_info.depth = 2;
    memcpy(stored.app_elf_sha256, SHA, sizeof(SHA));
    stored.ex_info.exc_cause = 1;
    stored.ex_info.exc_vaddr = 0x3fc00000u;
    assert(pet_diagnostics_init() == ESP_OK);
    log_count = 0;
}

static unsigned logged(char level, const char *text)
{
    unsigned count = 0;
    for (unsigned i = 0; i < log_count; ++i)
        if (log_levels[i] == level && strstr(log_lines[i], text)) ++count;
    return count;
}

static void expect_retained(void)
{
    pet_diagnostics_snapshot_t snapshot;
    pet_diagnostics_get_snapshot(&snapshot);
    assert(snapshot.coredump_present && !strcmp(snapshot.crash_elf_sha, SHA));
    assert(snapshot.crash_pc == PC && !strcmp(snapshot.crash_task, "websocket_task"));
    assert(snapshot.crash_backtrace_depth == 2 && snapshot.crash_backtrace[1] == 0x42001234u);
    assert(snapshot.crash_exception_cause == 1 && snapshot.crash_exception_address == 0x3fc00000u);
    assert(strstr(snapshot.panic_reason, "websocket_task"));
}

static void expect_cleared(void)
{
    pet_diagnostics_snapshot_t snapshot, empty;
    pet_diagnostics_get_snapshot(&snapshot);
    memset(&empty, 0, sizeof(empty));
    assert(!snapshot.coredump_present && !snapshot.crash_backtrace_corrupted);
    assert(!memcmp(snapshot.panic_reason, empty.panic_reason, sizeof(empty.panic_reason)));
    assert(!memcmp(snapshot.crash_task, empty.crash_task, sizeof(empty.crash_task)));
    assert(!memcmp(snapshot.crash_elf_sha, empty.crash_elf_sha, sizeof(empty.crash_elf_sha)));
    assert(!snapshot.crash_pc && !snapshot.crash_exception_cause && !snapshot.crash_exception_address);
    assert(!snapshot.crash_backtrace_depth);
    assert(!memcmp(snapshot.crash_backtrace, empty.crash_backtrace, sizeof(empty.crash_backtrace)));
    assert(!strcmp(snapshot.boot_id, "00005eed00005eed")); /* Only crash fields change. */
}

int main(void)
{
    /* An erased core dump partition is no core dump. ESP-IDF reports it as
     * ESP_ERR_INVALID_SIZE, which the Pocket logged as an error on every boot
     * (3 Oct). Any other invalid size is still an error. */
    image_check = ESP_ERR_INVALID_SIZE;
    log_count = 0;
    assert(pet_diagnostics_init() == ESP_OK);
    assert(logged('I', "no retained core dump") == 1 && !logged('E', "unreadable"));
    coredump_size_word = 0x00001234u;
    log_count = 0;
    assert(pet_diagnostics_init() == ESP_OK);
    assert(logged('E', "unreadable") == 1);

    /* Boot reads the retained dump; nothing is erased without an acknowledgement. */
    boot(ESP_OK, ESP_OK);
    expect_retained();
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_ERR_NOT_FOUND && !erase_calls);

    /* Malformed or mismatched identities are ignored with a warning. */
    char too_long[PET_DIAGNOSTICS_CRASH_ELF_SHA_MAX + 2];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    assert(pet_diagnostics_acknowledge_crash(NULL, PC) == ESP_ERR_INVALID_ARG);
    assert(pet_diagnostics_acknowledge_crash("", PC) == ESP_ERR_INVALID_ARG);
    assert(pet_diagnostics_acknowledge_crash(too_long, PC) == ESP_ERR_INVALID_ARG);
    assert(logged('W', "CRASH_ACK ignored: malformed") == 3);
    const char *other_sha[] = {"9dc3a7b1e", "9dc3a7b1", "9dc3a7b1f0", "9DC3A7B1F"};
    for (size_t i = 0; i < sizeof(other_sha) / sizeof(other_sha[0]); ++i)
        assert(pet_diagnostics_acknowledge_crash(other_sha[i], PC) == ESP_ERR_NOT_FOUND);
    assert(pet_diagnostics_acknowledge_crash(SHA, PC + 1) == ESP_ERR_NOT_FOUND);
    assert(pet_diagnostics_acknowledge_crash(SHA, 0) == ESP_ERR_NOT_FOUND);
    assert(logged('W', "is not the stored crash summary") == 6);
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_ERR_NOT_FOUND && !erase_calls);
    expect_retained();

    /* An exact match only schedules the erase; the websocket task never waits on flash. */
    assert(pet_diagnostics_acknowledge_crash(SHA, PC) == ESP_OK);
    assert(!erase_calls);
    expect_retained();

    /* A failed erase keeps the summary and is not retried until acknowledged again. */
    erase_result = ESP_FAIL;
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_FAIL && erase_calls == 1);
    assert(logged('E', "CRASH_ACK core dump erase failed") == 1);
    expect_retained();
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_ERR_NOT_FOUND && erase_calls == 1);

    /* The next acknowledgement erases once, clears every crash field and logs one line. */
    erase_result = ESP_OK;
    acknowledge_during_erase = true;
    log_count = 0;
    assert(pet_diagnostics_acknowledge_crash(SHA, PC) == ESP_OK);
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_OK && erase_calls == 2);
    expect_cleared();
    assert(log_count == 1 && logged('I', "CRASH_CLEARED elfSha=" SHA " pc=0x4037c3fa") == 1);
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_ERR_NOT_FOUND && erase_calls == 2);
    acknowledge_during_erase = false;
    assert(pet_diagnostics_acknowledge_crash(SHA, PC) == ESP_ERR_NOT_FOUND);
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_ERR_NOT_FOUND && erase_calls == 2);
    expect_cleared();

    /* No retained dump: nothing to acknowledge. */
    boot(ESP_ERR_NOT_FOUND, ESP_OK);
    expect_cleared();
    assert(pet_diagnostics_acknowledge_crash(SHA, PC) == ESP_ERR_NOT_FOUND);
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_ERR_NOT_FOUND && !erase_calls);

    /* A dump without a readable summary has no identity, so no acknowledgement erases it. */
    boot(ESP_OK, ESP_FAIL);
    pet_diagnostics_snapshot_t unsummarized;
    pet_diagnostics_get_snapshot(&unsummarized);
    assert(unsummarized.coredump_present && !unsummarized.crash_elf_sha[0] && !unsummarized.crash_pc);
    assert(pet_diagnostics_acknowledge_crash("", 0) == ESP_ERR_INVALID_ARG);
    assert(pet_diagnostics_acknowledge_crash(SHA, 0) == ESP_ERR_NOT_FOUND);
    assert(pet_diagnostics_clear_acknowledged_crash() == ESP_ERR_NOT_FOUND && !erase_calls);

    /* A reply cut by a recoverable error after 5 s of speaking stops playback a
     * moment before the face moves on: that is no stall. Lasting silence is. */
    s_snapshot.state = PET_FACE_SPEAKING;
    s_snapshot.playing = false;
    s_snapshot.state_age_ms = 6000;
    assert(!speaking_stalled(10000));
    assert(!speaking_stalled(10250));
    assert(speaking_stalled(11500));
    s_snapshot.playing = true;
    assert(!speaking_stalled(11750));
    /* A face that never played: still recovered once the state is stale. */
    s_snapshot.playing = false;
    s_snapshot.state_age_ms = 2000;
    assert(!speaking_stalled(20000) && !speaking_stalled(23000));
    s_snapshot.state_age_ms = 5001;
    assert(speaking_stalled(23250));
    puts("diagnostics crash acknowledgement and speaking stall: passed");
    return 0;
}
