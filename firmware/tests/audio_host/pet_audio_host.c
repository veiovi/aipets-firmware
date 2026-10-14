/* Speech playback through the real firmware/main/pet_audio.c: the end of a
 * reply and the story's loops (test_playback_tail.py), and mouth timing
 * (test_mouth_timing.py).
 *
 * Its tasks run on POSIX threads (fake_rtos.c). The board (pet_audio_board.h)
 * is a model of the I2S TX DMA ring the device has: CONFIG_PET_AUDIO_DMA_BUFFERS
 * buffers of 240 frames, drained in real time. A write blocks until the ring
 * has room, as i2s_channel_write does, and closing the speaker drops what the
 * ring still holds, as the device does. Once written, a ring that runs dry
 * counts one underflow per buffer it plays empty, as the driver's
 * on_send_q_ovf does. A scenario can stall the speaker: the ring then keeps
 * what it holds and writes block until it resumes. It can also hold the
 * playback task off before a write, as the renderer did: the ring keeps
 * playing and runs dry. The face records every mouth level it is given, with
 * its time.
 *
 * Usage: pet_audio_host <scenario>; exit status 0 when it passes. The
 * scenarios run in their own processes so each starts from a fresh module. */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_codec_dev.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdatomic.h>
#include "pet_atmosphere.h"
#include "pet_audio.h"
#include "pet_audio_board.h"
#include "pet_face.h"
#include "pet_sound_bank.h"

#ifndef CONFIG_PET_PLAYBACK_TAIL_MS
#error "Build with -DCONFIG_PET_PLAYBACK_TAIL_MS=<the Kconfig default>"
#endif
#ifndef CONFIG_PET_AUDIO_DMA_BUFFERS
#error "Build with -DCONFIG_PET_AUDIO_DMA_BUFFERS=<the Kconfig default, or a ring to try>"
#endif

#define TAIL_MS ((int64_t)CONFIG_PET_PLAYBACK_TAIL_MS)
#define DRAIN_FLOOR_MS 300
#define DMA_FRAMES ((uint32_t)CONFIG_PET_AUDIO_DMA_BUFFERS * PET_AUDIO_DMA_FRAMES)
/* The ring at 24 kHz: 60 ms with the default 6 buffers. */
#define DMA_MS ((int)((int64_t)DMA_FRAMES * 1000 / 24000))
#define MAX_WRITES 8192
#define MAX_EVENTS 64
#define MAX_LEVELS 8192
/* Scheduling slack allowed on a loaded host. */
#define LATE_MS 250
/* Nothing may wait longer than this for the codec while a tail runs. */
#define TAKEOVER_MS 60
/* A tail that stopped feeding the ring would leave it dry for up to the whole
 * tail; host jitter alone stays well under this. */
#define DRY_TOLERANCE_US 50000

struct fake_codec {
    bool output;
};

typedef struct {
    int64_t start_us;
    int64_t end_us;
    /* When the ring finishes playing this write, unless the speaker stalls. */
    int64_t heard_end_us;
    uint32_t frames;
    uint32_t rate;
    uint64_t speech_frames_before;
    bool silent;
    /* The largest magnitude written, that of the last sample, and how often
     * the sign changed. */
    uint16_t peak;
    uint16_t last;
    uint16_t crossings;
} write_record_t;

typedef struct {
    int64_t at_us;
    uint8_t level;
} level_record_t;

static struct fake_codec g_speaker_device = { .output = true };
static struct fake_codec g_microphone_device = { .output = false };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_verbose;
static int g_failures;

/* Guarded by g_lock. */
static struct {
    bool speaker_open;
    uint32_t rate;
    int opens;
    int closes;
    int reopen_ignored;
    int writes_while_closed;
    int64_t open_us[MAX_EVENTS];
    uint32_t open_rate[MAX_EVENTS];
    int64_t close_us[MAX_EVENTS];
    /* The DMA ring: microseconds of audio queued, as of stamp_us. */
    double fill_us;
    int64_t stamp_us;
    bool fed;
    bool stalled;
    double max_dry_us;
    /* Buffers played empty after the first write since the speaker opened. */
    double underflow_buffers;
    /* The playback task is held off this long before its speech write number
     * starve_at_write (1-based; 0: never). */
    uint32_t speech_writes;
    uint32_t starve_at_write;
    int64_t starve_us;
    /* The last "playback audio" statistics line pet_audio.c logged. */
    char stats[640];
    write_record_t writes[MAX_WRITES];
    size_t write_count;
    uint64_t speech_frames;
    int64_t last_speech_end_us;
    bool microphone_open;
    /* The speaker's last volume (esp_codec_dev_set_out_vol). */
    int speaker_volume;
    esp_codec_dev_sample_info_t microphone_format;
    int microphone_opens;
    int microphone_closes;
    int64_t microphone_open_us;
    int64_t microphone_close_us;
    int face_states;
    pet_face_state_t last_state;
    int64_t last_idle_us;
    uint8_t last_level;
    level_record_t levels[MAX_LEVELS];
    size_t level_count;
    int done_count;
    uint32_t done_stream[MAX_EVENTS];
    int64_t done_us[MAX_EVENTS];
    bool playing_at_done[MAX_EVENTS];
    int capture_chunks;
    int capture_bad_pcm;
    int capture_done_count;
    uint32_t capture_done_stream;
} g;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #condition); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        g_failures++; \
    } \
} while (0)
/* The rest of a scenario means nothing once this fails. */
#define REQUIRE(condition, ...) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #condition); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        return 1; \
    } \
} while (0)

void fake_log(char level, const char *tag, const char *format, ...)
{
    char line[640];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (!strncmp(line, "playback audio stream=", 22)) {
        pthread_mutex_lock(&g_lock);
        memcpy(g.stats, line, sizeof(g.stats));
        pthread_mutex_unlock(&g_lock);
    }
    if (g_verbose) printf("%10.1f %c %s: %s\n", esp_timer_get_time() / 1000.0, level, tag, line);
}

static int64_t now_us(void) { return esp_timer_get_time(); }

static void sleep_us(int64_t us)
{
    if (us <= 0) return;
    vTaskDelay((TickType_t)((us + 999) / 1000));
}

static void sleep_until(int64_t when_us) { sleep_us(when_us - now_us()); }

/* ---- Device stand-ins ------------------------------------------------- */

esp_err_t pet_audio_board_init(esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *microphone)
{
    *speaker = &g_speaker_device;
    *microphone = &g_microphone_device;
    return ESP_OK;
}

uint32_t pet_audio_board_tx_ring_frames(void) { return DMA_FRAMES; }

uint32_t pet_audio_board_tx_underflows(void)
{
    pthread_mutex_lock(&g_lock);
    uint32_t count = (uint32_t)g.underflow_buffers;
    pthread_mutex_unlock(&g_lock);
    return count;
}

/* Called with g_lock held: the ring plays out what it holds. A stalled
 * speaker plays nothing and keeps it. Written, then dry, it plays empty
 * buffers: underflows. */
static void dma_advance(int64_t now)
{
    double elapsed = g.stalled ? 0.0 : (double)(now - g.stamp_us);
    if (g.fed && elapsed > g.fill_us) {
        double dry = elapsed - g.fill_us;
        if (dry > g.max_dry_us) g.max_dry_us = dry;
        if (g.rate) g.underflow_buffers += dry / (PET_AUDIO_DMA_FRAMES * 1e6 / g.rate);
    }
    g.fill_us = elapsed >= g.fill_us ? 0.0 : g.fill_us - elapsed;
    g.stamp_us = now;
}

int esp_codec_dev_open(esp_codec_dev_handle_t handle, esp_codec_dev_sample_info_t *fs)
{
    pthread_mutex_lock(&g_lock);
    if (handle->output) {
        if (g.speaker_open) {
            /* esp_codec_dev returns OK and keeps the old format. */
            g.reopen_ignored++;
        } else if (g.opens < MAX_EVENTS) {
            g.speaker_open = true;
            g.rate = fs->sample_rate;
            g.open_us[g.opens] = now_us();
            g.open_rate[g.opens] = fs->sample_rate;
            g.opens++;
            g.fill_us = 0;
            g.stamp_us = now_us();
            g.fed = false;
        }
    } else {
        g.microphone_open = true;
        g.microphone_format = *fs;
        g.microphone_opens++;
        g.microphone_open_us = now_us();
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int esp_codec_dev_close(esp_codec_dev_handle_t handle)
{
    pthread_mutex_lock(&g_lock);
    if (handle->output) {
        if (g.speaker_open && g.closes < MAX_EVENTS) {
            dma_advance(now_us());
            g.close_us[g.closes++] = now_us();
        }
        /* The ring is dropped, as i2s_channel_disable does. */
        g.speaker_open = false;
        g.fed = false;
        g.fill_us = 0;
    } else if (g.microphone_open) {
        g.microphone_open = false;
        g.microphone_closes++;
        g.microphone_close_us = now_us();
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int esp_codec_dev_write(esp_codec_dev_handle_t handle, void *data, int len)
{
    if (!handle->output || len <= 0 || (len & 1)) abort();
    const int16_t *pcm = data;
    uint32_t frames = (uint32_t)len / 2u;
    uint16_t peak = 0;
    uint16_t crossings = 0;
    for (uint32_t i = 0; i < frames; i++) {
        uint16_t magnitude = (uint16_t)abs(pcm[i]);
        if (magnitude > peak) peak = magnitude;
        if (i && (pcm[i] < 0) != (pcm[i - 1] < 0)) crossings++;
    }
    bool silent = peak == 0;
    pthread_mutex_lock(&g_lock);
    int64_t starve = 0;
    if (!silent && g.speaker_open && ++g.speech_writes == g.starve_at_write) starve = g.starve_us;
    pthread_mutex_unlock(&g_lock);
    /* Held off (a busier task on its core): the ring plays on meanwhile. */
    sleep_us(starve);
    pthread_mutex_lock(&g_lock);
    if (!g.speaker_open) {
        g.writes_while_closed++;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    uint32_t rate = g.rate;
    double duration_us = frames * 1e6 / rate;
    double capacity_us = DMA_FRAMES * 1e6 / rate;
    int64_t start = now_us();
    /* i2s_channel_write blocks until the ring has room. */
    for (;;) {
        dma_advance(now_us());
        double wait_us = g.fill_us + duration_us - capacity_us;
        if (wait_us <= 0) break;
        int64_t sleep = g.stalled ? 2000 : (int64_t)wait_us;
        pthread_mutex_unlock(&g_lock);
        sleep_us(sleep);
        pthread_mutex_lock(&g_lock);
    }
    int64_t end = now_us();
    g.fill_us += duration_us;
    g.fed = true;
    if (g.write_count < MAX_WRITES) {
        g.writes[g.write_count++] = (write_record_t){
            .start_us = start, .end_us = end, .heard_end_us = end + (int64_t)g.fill_us,
            .frames = frames, .rate = rate, .speech_frames_before = g.speech_frames, .silent = silent,
            .peak = peak, .last = (uint16_t)abs(pcm[frames - 1]), .crossings = crossings,
        };
    }
    if (!silent) {
        g.speech_frames += frames;
        g.last_speech_end_us = end;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int esp_codec_dev_read(esp_codec_dev_handle_t handle, void *data, int len)
{
    if (handle->output) abort();
    static const int16_t stereo[][2] = {
        {1200, 3400}, {-24000, -10000}, {INT16_MIN, INT16_MAX}, {INT16_MAX, INT16_MAX},
    };
    pthread_mutex_lock(&g_lock);
    int bits = g.microphone_format.bits_per_sample;
    pthread_mutex_unlock(&g_lock);
    int frames = len / (2 * bits / 8);
    for (int i = 0; i < frames; ++i) {
        for (int channel = 0; channel < 2; ++channel) {
            int16_t sample = stereo[i % 4][channel];
            if (bits == 32) ((int32_t *)data)[i * 2 + channel] = (int32_t)sample * 65536;
            else ((int16_t *)data)[i * 2 + channel] = sample;
        }
    }
    vTaskDelay(20);
    pthread_mutex_lock(&g_lock);
    bool open = g.microphone_open;
    pthread_mutex_unlock(&g_lock);
    return open ? 0 : -1;
}

int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t handle, int volume)
{
    (void)handle;
    pthread_mutex_lock(&g_lock);
    g.speaker_volume = volume;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t handle, float db_value)
{
    (void)handle;
    (void)db_value;
    return 0;
}

void pet_face_set_state(pet_face_state_t state)
{
    pthread_mutex_lock(&g_lock);
    g.face_states++;
    g.last_state = state;
    if (state == PET_FACE_IDLE) g.last_idle_us = now_us();
    pthread_mutex_unlock(&g_lock);
}

void pet_face_set_audio_level(uint8_t level)
{
    pthread_mutex_lock(&g_lock);
    g.last_level = level;
    if (g.level_count < MAX_LEVELS) g.levels[g.level_count++] = (level_record_t){ now_us(), level };
    pthread_mutex_unlock(&g_lock);
}

void pet_face_set_speech_articulation(uint8_t level) { (void)level; }
void pet_face_begin_speech_stream(pet_speech_mouth_mode_t mode) { (void)mode; }

/* The sound bank: steady 16 kHz tones at half of full scale that
 * test_playback_tail.py writes. */
#define BANK_SOUND(name) \
    extern const uint8_t name##_sound_start[] __asm__("_binary_" #name "_adpcm_start"); \
    extern const uint8_t name##_sound_end[] __asm__("_binary_" #name "_adpcm_end");
BANK_SOUND(campfire)
BANK_SOUND(dice)
BANK_SOUND(hit)
static const struct {
    const char *id;
    pet_sound_kind_t kind;
    const uint8_t *start;
    const uint8_t *end;
} BANK[] = {
    { "campfire", PET_SOUND_LOOP, campfire_sound_start, campfire_sound_end },
    { "dice", PET_SOUND_ONE_SHOT, dice_sound_start, dice_sound_end },
    { "hit", PET_SOUND_ONE_SHOT, hit_sound_start, hit_sound_end },
};

static atomic_uint bank_generation = 1;

int pet_sound_bank_find(const char *id, pet_sound_kind_t kind, uint32_t *generation)
{
    if (generation) *generation = atomic_load(&bank_generation);
    for (size_t i = 0; id && i < sizeof(BANK) / sizeof(BANK[0]); i++) {
        if (BANK[i].kind == kind && !strcmp(BANK[i].id, id)) return (int)i;
    }
    return -1;
}

static atomic_bool bank_stalled, bank_entered;
static pthread_t media_thread;
uint32_t pet_sound_bank_generation(void)
{
    return atomic_load(&bank_generation);
}

bool pet_sound_bank_copy(int index, uint8_t *buffer, size_t capacity, size_t *bytes, uint32_t *rate)
{
    assert(pthread_equal(pthread_self(), media_thread));
    atomic_store(&bank_entered, true);
    while (atomic_load(&bank_stalled)) vTaskDelay(1);
    if (index < 0 || (size_t)index >= sizeof(BANK) / sizeof(BANK[0])) return false;
    size_t length = (size_t)(BANK[index].end - BANK[index].start);
    if (length > capacity) return false;
    memcpy(buffer, BANK[index].start, length);
    *bytes = length;
    *rate = 16000;
    return true;
}

/* The reply's cues have played (pet_audio_playback_hold_for_sounds). */
static int g_played;
static uint32_t g_played_stream;
static int64_t g_played_us;
static void sounds_played(uint32_t stream_id)
{
    pthread_mutex_lock(&g_lock);
    g_played++;
    g_played_stream = stream_id;
    g_played_us = esp_timer_get_time();
    pthread_mutex_unlock(&g_lock);
}

static void playback_done(uint32_t stream_id)
{
    bool playing = pet_audio_is_playing();
    pthread_mutex_lock(&g_lock);
    if (g.done_count < MAX_EVENTS) {
        g.done_stream[g.done_count] = stream_id;
        g.done_us[g.done_count] = now_us();
        g.playing_at_done[g.done_count] = playing;
    }
    g.done_count++;
    pthread_mutex_unlock(&g_lock);
}

static esp_err_t capture_chunk(uint32_t stream_id, uint32_t sequence, const int16_t *pcm, size_t samples)
{
    (void)stream_id;
    (void)sequence;
    pthread_mutex_lock(&g_lock);
    static const int16_t expected[] = {2300, -17000, 0, INT16_MAX};
    if (samples != 320) g.capture_bad_pcm++;
    for (size_t i = 0; i < samples; ++i) {
        if (pcm[i] != expected[i % 4]) g.capture_bad_pcm++;
    }
    g.capture_chunks++;
    pthread_mutex_unlock(&g_lock);
    return ESP_OK;
}

static void capture_done(uint32_t stream_id, uint32_t final_sequence, uint32_t duration_ms, esp_err_t result)
{
    (void)final_sequence;
    (void)duration_ms;
    (void)result;
    pthread_mutex_lock(&g_lock);
    g.capture_done_count++;
    g.capture_done_stream = stream_id;
    pthread_mutex_unlock(&g_lock);
}

/* ---- Helpers ---------------------------------------------------------- */

typedef bool (*predicate_t)(void);

static bool wait_for(predicate_t predicate, int64_t timeout_ms)
{
    int64_t deadline = now_us() + timeout_ms * 1000;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        bool met = predicate();
        pthread_mutex_unlock(&g_lock);
        if (met) return true;
        if (now_us() >= deadline) return false;
        vTaskDelay(2);
    }
}

static uint64_t s_expected_speech_frames;
static int s_expected_done;
static int s_expected_closes;
static bool speech_written(void) { return g.speech_frames >= s_expected_speech_frames; }
static bool reply_done(void) { return g.done_count >= s_expected_done; }
static bool speaker_closed(void) { return g.closes >= s_expected_closes && !g.speaker_open; }
static bool microphone_opened(void) { return g.microphone_opens > 0; }
static bool capture_finished(void) { return g.capture_done_count > 0; }

/* The amplitude of the speech stream_reply sends. */
static int16_t s_speech_level = 6000;

/* Streams one reply: 40 ms frames of a square wave, then the final marker. */
static int stream_reply(uint32_t stream, uint32_t rate, int frames_before_gap, int frames_after_gap,
                        int64_t gap_ms)
{
    int16_t pcm[1024];
    size_t samples = rate / 25u;
    for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)((i / 12u) % 2u ? s_speech_level : -s_speech_level);
    pthread_mutex_lock(&g_lock);
    s_expected_speech_frames = g.speech_frames + (uint64_t)samples * (uint64_t)(frames_before_gap + frames_after_gap);
    pthread_mutex_unlock(&g_lock);
    REQUIRE(pet_audio_playback_start(stream, rate, PET_REALTIME_BOOST_OFF,
                                     PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED) == ESP_OK, "start");
    uint32_t sequence = 0;
    for (int i = 0; i < frames_before_gap + frames_after_gap; i++) {
        if (i == frames_before_gap && gap_ms) sleep_us(gap_ms * 1000);
        REQUIRE(pet_audio_playback_enqueue(stream, sequence++, (const uint8_t *)pcm,
                                           samples * sizeof(int16_t)) == ESP_OK, "enqueue %d", i);
    }
    pet_audio_playback_finish(stream);
    return 0;
}

static int speaker_opens_minus_closes(void)
{
    pthread_mutex_lock(&g_lock);
    int open = g.opens - g.closes;
    pthread_mutex_unlock(&g_lock);
    return open;
}

/* A short reply, heard to the end and reported done; returns when the reply
 * sits in its tail, `offset_ms` after its last speech left for the DMA. */
static int reply_then_wait_in_tail(uint32_t stream, int64_t offset_ms)
{
    if (stream_reply(stream, 24000, 4, 0, 0)) return 1;
    REQUIRE(wait_for(speech_written, 2000), "the reply's speech was written");
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 1500), "the reply was reported done");
    pthread_mutex_lock(&g_lock);
    int64_t last_speech = g.last_speech_end_us;
    pthread_mutex_unlock(&g_lock);
    sleep_until(last_speech + offset_ms * 1000);
    REQUIRE(speaker_opens_minus_closes() == 1,
            "the speaker is still open %lld ms after the last speech (tail)", (long long)offset_ms);
    return 0;
}

static int64_t speaker_writes_started_after(int64_t when_us)
{
    int64_t count = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g.write_count; i++) count += g.writes[i].start_us > when_us;
    pthread_mutex_unlock(&g_lock);
    return count;
}

/* The speaker stops taking audio, or takes it again. */
static void speaker_stall(bool stalled)
{
    pthread_mutex_lock(&g_lock);
    dma_advance(now_us());
    g.stalled = stalled;
    pthread_mutex_unlock(&g_lock);
}

/* The number after `name=` in the last statistics line; -1 when absent. */
static long stats_value(const char *name)
{
    char key[48];
    snprintf(key, sizeof(key), " %s=", name);
    pthread_mutex_lock(&g_lock);
    const char *at = strstr(g.stats, key);
    long value = at ? strtol(at + strlen(key), NULL, 10) : -1;
    pthread_mutex_unlock(&g_lock);
    return value;
}

/* ---- Scenarios -------------------------------------------------------- */

/* The final marker leaves the speaker open; the reply is reported done once
 * drained; the speaker closes when the tail ends; the ring never runs dry. */
static int scenario_complete(void)
{
    /* Six frames prime playback; the gap exercises the silence bridge. */
    if (stream_reply(11, 24000, 6, 3, 350)) return 1;
    REQUIRE(wait_for(speech_written, 3000), "all speech was written");
    pthread_mutex_lock(&g_lock);
    int64_t last_speech = g.last_speech_end_us;
    pthread_mutex_unlock(&g_lock);

    /* Sample "playing" through the drain. */
    int64_t playing_until = 0;
    while (now_us() < last_speech + (DRAIN_FLOOR_MS + 200) * 1000) {
        if (pet_audio_is_playing()) playing_until = now_us();
        vTaskDelay(5);
    }
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 1000), "the reply was reported done");
    s_expected_closes = 1;
    REQUIRE(wait_for(speaker_closed, TAIL_MS + 1500), "the speaker closed after the tail");
    vTaskDelay(100);

    pthread_mutex_lock(&g_lock);
    int64_t done_ms = (g.done_us[0] - last_speech) / 1000;
    int64_t close_ms = (g.close_us[0] - last_speech) / 1000;
    int64_t idle_ms = (g.last_idle_us - last_speech) / 1000;
    CHECK(g.done_count == 1 && g.done_stream[0] == 11, "done=%d stream=%lu", g.done_count,
          (unsigned long)g.done_stream[0]);
    CHECK(!g.playing_at_done[0], "no longer playing when reported done");
    CHECK(g.opens == 1 && g.closes == 1, "opens=%d closes=%d", g.opens, g.closes);
    CHECK(done_ms >= DRAIN_FLOOR_MS && done_ms <= DRAIN_FLOOR_MS + LATE_MS,
          "reported done %lld ms after the last speech, expected >= %d", (long long)done_ms, DRAIN_FLOOR_MS);
    CHECK(close_ms >= TAIL_MS && close_ms <= TAIL_MS + LATE_MS,
          "closed %lld ms after the last speech, expected >= %lld", (long long)close_ms, (long long)TAIL_MS);
    CHECK(g.last_state == PET_FACE_IDLE && idle_ms >= DRAIN_FLOOR_MS && idle_ms <= done_ms,
          "face idle %lld ms after the last speech (state %d)", (long long)idle_ms, (int)g.last_state);
    CHECK(g.last_level == 0, "mouth closed, level %u", g.last_level);
    CHECK(g.writes_while_closed == 0 && g.reopen_ignored == 0, "writes while closed %d, ignored opens %d",
          g.writes_while_closed, g.reopen_ignored);
    CHECK(g.max_dry_us <= DRY_TOLERANCE_US, "the DMA ran dry for %.1f ms", g.max_dry_us / 1000.0);
    double model_underflows = g.underflow_buffers;
    pthread_mutex_unlock(&g_lock);
    long underflows = stats_value("dma_underflows"), ring_ms = stats_value("dma_ms"), gap_ms = stats_value("feed_gap_ms");
    CHECK(ring_ms == DMA_MS, "the statistics give a %ld ms ring, expected %lld", ring_ms, (long long)DMA_MS);
    /* The 350 ms network gap is bridged with silence: the ring is fed throughout. */
    CHECK(gap_ms >= 15 && gap_ms <= DMA_MS + DRY_TOLERANCE_US / 1000, "the longest feed gap was %ld ms", gap_ms);
    CHECK(underflows >= 0 && underflows <= (long)model_underflows && underflows <= DRY_TOLERANCE_US / 10000,
          "%ld DMA underflows reported (the ring played %.1f buffers empty)", underflows, model_underflows);
    pthread_mutex_lock(&g_lock);
    bool bridged = false;
    size_t tail_blocks = 0;
    for (size_t i = 0; i < g.write_count; i++) {
        const write_record_t *w = &g.writes[i];
        if (w->silent && w->speech_frames_before == 6u * 960u) bridged = true;
        if (w->start_us >= last_speech) {
            tail_blocks++;
            CHECK(w->silent && w->frames <= 480u, "tail write %zu: %u frames, silent %d", i, w->frames, w->silent);
            CHECK(w->end_us <= g.close_us[0], "tail write %zu after the close", i);
        }
    }
    CHECK(bridged, "silence bridged the gap between frames six and seven");
    CHECK(tail_blocks * 20u >= (size_t)(TAIL_MS - 100), "%zu silence blocks fed the tail", tail_blocks);
    pthread_mutex_unlock(&g_lock);
    CHECK(playing_until >= last_speech + (DRAIN_FLOOR_MS - 20) * 1000,
          "playing until %lld ms after the last speech (-1: not after it)",
          playing_until ? (long long)((playing_until - last_speech) / 1000) : -1LL);
    CHECK(!pet_audio_is_playing(), "not playing after the tail");
    return 0;
}

/* A stop during the tail closes the speaker before it returns. */
static int scenario_stop_in_tail(void)
{
    if (reply_then_wait_in_tail(21, 700)) return 1;
    int64_t before = now_us();
    pet_audio_playback_cancel();
    int64_t after = now_us();
    vTaskDelay(150);
    pthread_mutex_lock(&g_lock);
    CHECK(g.closes == 1 && g.close_us[0] >= before && g.close_us[0] <= after,
          "closed during the stop (closes %d)", g.closes);
    CHECK(g.done_count == 1, "reported done once, %d", g.done_count);
    CHECK(g.writes_while_closed == 0, "writes while closed %d", g.writes_while_closed);
    pthread_mutex_unlock(&g_lock);
    CHECK(after - before <= TAKEOVER_MS * 1000, "the stop waited %lld ms", (long long)((after - before) / 1000));
    CHECK(speaker_writes_started_after(after) == 0, "the tail wrote after the stop");
    return 0;
}

/* A stop while the reply's last audio drains: nothing is reported. */
static int scenario_stop_while_draining(void)
{
    if (stream_reply(22, 24000, 4, 0, 0)) return 1;
    REQUIRE(wait_for(speech_written, 2000), "the reply's speech was written");
    pthread_mutex_lock(&g_lock);
    int64_t last_speech = g.last_speech_end_us;
    pthread_mutex_unlock(&g_lock);
    sleep_until(last_speech + 80 * 1000);
    REQUIRE(pet_audio_is_playing(), "still playing 80 ms after the last speech");
    REQUIRE(speaker_opens_minus_closes() == 1, "the speaker is open while the reply drains");
    int64_t before = now_us();
    pet_audio_playback_cancel();
    int64_t after = now_us();
    CHECK(!pet_audio_is_playing(), "not playing after the stop");
    vTaskDelay(DRAIN_FLOOR_MS + 300);
    pthread_mutex_lock(&g_lock);
    CHECK(g.done_count == 0, "a stopped reply is not reported done (%d)", g.done_count);
    CHECK(g.closes == 1 && g.close_us[0] >= before && g.close_us[0] <= after, "closed during the stop");
    pthread_mutex_unlock(&g_lock);
    CHECK(after - before <= TAKEOVER_MS * 1000, "the stop waited %lld ms", (long long)((after - before) / 1000));
    CHECK(speaker_writes_started_after(after) == 0, "the speaker was written after the stop");
    return 0;
}

/* A new reply during the tail opens the speaker in its own format at once, and
 * the old tail's timer cannot close it. */
static int scenario_new_reply_in_tail(void)
{
    if (reply_then_wait_in_tail(31, 600)) return 1;
    int64_t before = now_us();
    if (stream_reply(32, 16000, 4, 0, 0)) return 1;
    int64_t started = now_us();
    REQUIRE(wait_for(speech_written, 2000), "the new reply's speech was written");
    s_expected_done = 2;
    REQUIRE(wait_for(reply_done, 1500), "the new reply was reported done");
    s_expected_closes = 2;
    REQUIRE(wait_for(speaker_closed, TAIL_MS + 1500), "the new reply's tail closed");
    pthread_mutex_lock(&g_lock);
    CHECK(g.opens == 2 && g.closes == 2, "opens=%d closes=%d", g.opens, g.closes);
    CHECK(g.close_us[0] >= before && g.close_us[0] <= g.open_us[1], "the old tail closed before the new open");
    CHECK(g.open_rate[1] == 16000, "the new reply opened at %lu Hz", (unsigned long)g.open_rate[1]);
    CHECK(g.open_us[1] - before <= TAKEOVER_MS * 1000 + 50000,
          "the new reply waited %lld ms for the speaker", (long long)((g.open_us[1] - before) / 1000));
    CHECK(g.reopen_ignored == 0, "an open was ignored");
    CHECK(g.done_count == 2 && g.done_stream[1] == 32, "reported %d, second stream %lu", g.done_count,
          (unsigned long)g.done_stream[1]);
    CHECK((g.close_us[1] - g.last_speech_end_us) / 1000 >= TAIL_MS,
          "the new reply's speaker closed %lld ms after its speech",
          (long long)((g.close_us[1] - g.last_speech_end_us) / 1000));
    for (size_t i = 0; i < g.write_count; i++) {
        if (g.writes[i].start_us > started) {
            CHECK(g.writes[i].rate == 16000, "write %zu at %lu Hz", i, (unsigned long)g.writes[i].rate);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* Tap to talk during the tail: the speaker closes first, the microphone opens
 * at once and the tail never writes again. */
static int scenario_capture_in_tail(void)
{
    if (reply_then_wait_in_tail(41, 600)) return 1;
    int64_t before = now_us();
    REQUIRE(pet_audio_capture_start(42) == ESP_OK, "the microphone may start after the reply");
    REQUIRE(wait_for(microphone_opened, 500), "the microphone opened");
    vTaskDelay(120);
    pet_audio_capture_stop();
    REQUIRE(wait_for(capture_finished, 1000), "the capture finished");
    vTaskDelay(100);
    pthread_mutex_lock(&g_lock);
    int64_t microphone_open = g.microphone_open_us;
    CHECK(g.microphone_format.bits_per_sample == 16 && g.microphone_format.channel == 2 &&
          g.microphone_format.sample_rate == 16000, "capture keeps duplex DMA at 16-bit stereo/16kHz");
    CHECK(g.capture_bad_pcm == 0, "capture preserves signed stereo averaging in 20ms mono chunks");
    CHECK(g.closes == 1 && g.close_us[0] >= before && g.close_us[0] <= microphone_open,
          "the speaker closed before the microphone opened");
    CHECK(microphone_open - before <= TAKEOVER_MS * 1000, "the microphone waited %lld ms",
          (long long)((microphone_open - before) / 1000));
    CHECK(g.capture_chunks > 0 && g.capture_done_stream == 42, "capture chunks %d stream %lu",
          g.capture_chunks, (unsigned long)g.capture_done_stream);
    CHECK(g.done_count == 1, "the reply was reported once (%d)", g.done_count);
    CHECK(g.writes_while_closed == 0, "writes while closed %d", g.writes_while_closed);
    pthread_mutex_unlock(&g_lock);
    CHECK(speaker_writes_started_after(microphone_open) == 0, "the speaker was written after the microphone opened");
    return 0;
}

/* A cue during the tail takes the speaker in its own format. */
static int scenario_cue_in_tail(void)
{
    if (reply_then_wait_in_tail(51, 600)) return 1;
    static int16_t cue[2400];
    for (size_t i = 0; i < sizeof(cue) / sizeof(cue[0]); i++) cue[i] = (i / 6u) % 2u ? 3000 : -3000;
    pthread_mutex_lock(&g_lock);
    uint64_t speech_before = g.speech_frames;
    pthread_mutex_unlock(&g_lock);
    int64_t before = now_us();
    CHECK(pet_audio_play_local((const uint8_t *)cue, sizeof(cue), NULL, NULL) == ESP_OK, "the cue played");
    int64_t after = now_us();
    vTaskDelay(150);
    pthread_mutex_lock(&g_lock);
    CHECK(g.opens == 2 && g.closes == 2, "opens=%d closes=%d", g.opens, g.closes);
    CHECK(g.close_us[0] >= before && g.close_us[0] <= g.open_us[1], "the tail closed before the cue opened");
    CHECK(g.open_us[1] - before <= TAKEOVER_MS * 1000, "the cue waited %lld ms",
          (long long)((g.open_us[1] - before) / 1000));
    CHECK(g.open_rate[1] == 24000 && g.reopen_ignored == 0, "the cue opened at %lu Hz",
          (unsigned long)g.open_rate[1]);
    CHECK(g.speech_frames - speech_before == 2400, "the whole cue was written (%llu frames)",
          (unsigned long long)(g.speech_frames - speech_before));
    CHECK(g.close_us[1] <= after, "the cue closed the speaker");
    pthread_mutex_unlock(&g_lock);
    CHECK(speaker_writes_started_after(after) == 0, "the tail wrote after the cue");
    return 0;
}

/* The renderer holds the playback task off for 200 ms in the middle of a
 * reply: the ring (60 ms) runs dry and plays empty buffers, clicks the
 * queue's underrun counter never sees. The reply's statistics count them. */
static int scenario_dma_underflow(void)
{
    enum { STARVE_MS = 200 };
    pthread_mutex_lock(&g_lock);
    g.starve_at_write = g.speech_writes + 6; /* 100 ms of speech in, with the ring full */
    g.starve_us = STARVE_MS * 1000;
    pthread_mutex_unlock(&g_lock);
    if (stream_reply(95, 24000, 9, 0, 0)) return 1;
    REQUIRE(wait_for(speech_written, 3000), "all speech was written");
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 1500), "the reply was reported done");
    pthread_mutex_lock(&g_lock);
    double dry_ms = g.max_dry_us / 1000.0, model = g.underflow_buffers;
    pthread_mutex_unlock(&g_lock);
    long underflows = stats_value("dma_underflows"), underruns = stats_value("underruns");
    long gap_ms = stats_value("feed_gap_ms");
    printf("held off %d ms: the ring ran dry for %.1f ms; dma_underflows=%ld underruns=%ld feed_gap_ms=%ld\n",
           STARVE_MS, dry_ms, underflows, underruns, gap_ms);
    CHECK(gap_ms >= STARVE_MS && gap_ms <= STARVE_MS + LATE_MS, "the longest feed gap was %ld ms", gap_ms);
    /* 200 ms held off, less the 60 ms the ring held: 14 buffers of 10 ms. */
    CHECK(dry_ms >= STARVE_MS - DMA_MS - 20, "the ring ran dry for only %.1f ms", dry_ms);
    CHECK(underflows >= (STARVE_MS - DMA_MS - 40) / 10 && underflows <= (STARVE_MS - DMA_MS + LATE_MS) / 10 &&
          underflows <= (long)model, "%ld DMA underflows reported, the ring played %.1f buffers empty", underflows,
          model);
    CHECK(underruns == 0, "the queue never ran empty, yet %ld underruns", underruns);
    return 0;
}

/* Speech arrives slower than real time, as over a phone hotspot: 40 ms frames
 * every 70 ms. After the first underrun the reply pauses on silence until a
 * deeper reserve waits, so it plays in a few long runs instead of a 20 ms gap
 * between every frame. The DMA never runs dry and every frame is heard. */
static int scenario_slow_link(void)
{
    enum { FRAMES = 60, EVERY_MS = 70 };
    int16_t pcm[1024];
    size_t samples = 24000u / 25u;
    for (size_t i = 0; i < samples; i++) pcm[i] = (i / 12u) % 2u ? 6000 : -6000;
    pthread_mutex_lock(&g_lock);
    s_expected_speech_frames = g.speech_frames + (uint64_t)samples * FRAMES;
    size_t first_write = g.write_count;
    pthread_mutex_unlock(&g_lock);
    REQUIRE(pet_audio_playback_start(97, 24000, PET_REALTIME_BOOST_OFF,
                                     PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED) == ESP_OK, "start");
    for (int i = 0; i < FRAMES; i++) {
        REQUIRE(pet_audio_playback_enqueue(97, (uint32_t)i, (const uint8_t *)pcm,
                                           samples * sizeof(int16_t)) == ESP_OK, "enqueue %d", i);
        sleep_us(EVERY_MS * 1000);
    }
    pet_audio_playback_finish(97);
    REQUIRE(wait_for(speech_written, 5000), "all speech was written");
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 2000), "the reply was reported done");
    /* Runs of speech writes (20 ms each) between pauses, up to the reply's
     * last speech write; that last run ends with the reply and may be short. */
    /* Short runs: under 240 ms. The first gap of a slow stretch is bridged
     * like a single late frame, so one short run may come before the pause. */
    int runs = 0, short_runs = 0, current = 0;
    pthread_mutex_lock(&g_lock);
    size_t last_speech = first_write;
    for (size_t i = first_write; i < g.write_count; i++) {
        if (!g.writes[i].silent) last_speech = i;
    }
    for (size_t i = first_write; i <= last_speech; i++) {
        if (!g.writes[i].silent) {
            current++;
        } else if (current) {
            runs++;
            short_runs += current < 12;
            current = 0;
        }
    }
    if (current) runs++;
    pthread_mutex_unlock(&g_lock);
    long rebuffers = stats_value("rebuffers"), underflows = stats_value("dma_underflows");
    printf("slow link: %d speech runs, %d short; rebuffers=%ld dma_underflows=%ld\n", runs, short_runs,
           rebuffers, underflows);
    CHECK(rebuffers >= 1 && rebuffers <= 6, "%ld rebuffers", rebuffers);
    CHECK(runs <= 8, "the reply played in %d runs: chopped", runs);
    CHECK(short_runs <= 1, "%d runs under 240 ms between pauses: chopped", short_runs);
    CHECK(underflows == 0, "the DMA ran dry %ld times while the reply paused", underflows);
    return 0;
}

/* ---- Mouth timing ---------------------------------------------------- */

/* The mouth waits for its slice to be heard, less the display's own delay:
 * the DMA ring (6 x 240 frames by default, 60 ms at 24 kHz), half a 20 ms
 * slice and the 5 ms codec allowance, less half the face's 33 ms tick and half
 * LVGL's 20 ms refresh: 49 ms by default. The offset moves it from there. */
#define MOUTH_BASE_MS (DMA_MS + 20 / 2 + 5 - (33 + 20) / 2)
/* The playback task releases values after each write: up to 10 ms early,
 * and late by up to a write on a busy host. */
#define MOUTH_EARLY_MS 12
#define MOUTH_LATE_MS 40

static int64_t first_speech_write_us(void)
{
    for (size_t i = 0; i < g.write_count; i++) {
        if (!g.writes[i].silent) return g.writes[i].start_us;
    }
    return 0;
}

static int64_t first_mouth_us(void)
{
    for (size_t i = 0; i < g.level_count; i++) {
        if (g.levels[i].level) return g.levels[i].at_us;
    }
    return 0;
}

/* The first mouth value reaches the face the configured delay after the
 * first slice's level is computed, just before that slice's write. */
static int mouth_onset(int16_t offset_ms, int64_t expected_ms)
{
    pet_audio_set_speech_mouth_offset(offset_ms);
    if (stream_reply(61, 24000, 4, 0, 0)) return 1;
    REQUIRE(wait_for(speech_written, 2000), "the reply's speech was written");
    vTaskDelay(600);
    pthread_mutex_lock(&g_lock);
    int64_t written = first_speech_write_us(), shown = first_mouth_us();
    pthread_mutex_unlock(&g_lock);
    REQUIRE(written && shown, "a speech write (%lld) and a mouth value (%lld)", (long long)written, (long long)shown);
    double onset_ms = (shown - written) / 1000.0;
    printf("offset %d ms: the mouth moved %.1f ms after its slice's level was computed\n", offset_ms, onset_ms);
    CHECK(onset_ms >= expected_ms - MOUTH_EARLY_MS && onset_ms <= expected_ms + MOUTH_LATE_MS,
          "offset %d ms: the mouth moved %.1f ms after its slice, expected %lld", offset_ms, onset_ms,
          (long long)expected_ms);
    if (expected_ms == 0) CHECK(shown <= written, "with no delay the mouth moves before the write");
    return 0;
}

static int scenario_mouth_default(void) { return mouth_onset(0, MOUTH_BASE_MS); }
static int scenario_mouth_later(void) { return mouth_onset(100, MOUTH_BASE_MS + 100); }
static int scenario_mouth_earlier(void) { return mouth_onset(-30, MOUTH_BASE_MS - 30); }
/* The mouth cannot run ahead of the analysis: the earliest is no delay. */
static int scenario_mouth_earliest(void) { return mouth_onset(-300, 0); }

static int scenario_mouth_clamp(void)
{
    CHECK(pet_audio_get_speech_mouth_offset() == 0, "the default is 0, got %d", pet_audio_get_speech_mouth_offset());
    pet_audio_set_speech_mouth_offset(1000);
    CHECK(pet_audio_get_speech_mouth_offset() == 300, "clamped to 300, got %d", pet_audio_get_speech_mouth_offset());
    pet_audio_set_speech_mouth_offset(-1000);
    CHECK(pet_audio_get_speech_mouth_offset() == -300, "clamped to -300, got %d", pet_audio_get_speech_mouth_offset());
    pet_audio_set_speech_mouth_offset(-20);
    CHECK(pet_audio_get_speech_mouth_offset() == -20, "kept -20, got %d", pet_audio_get_speech_mouth_offset());
    return 0;
}

/* With the mouth 300 ms later, the reply ends only after its last mouth
 * values have been shown. */
static int scenario_mouth_drain(void)
{
    pet_audio_set_speech_mouth_offset(300);
    if (stream_reply(71, 24000, 4, 0, 0)) return 1;
    REQUIRE(wait_for(speech_written, 2000), "the reply's speech was written");
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 2000), "the reply was reported done");
    vTaskDelay(100);
    pthread_mutex_lock(&g_lock);
    int64_t last_speech = g.last_speech_end_us, done = g.done_us[0];
    /* The reply's own loud slices, not the release after it: without the
     * delay the mouth has decayed below 10 by then. */
    bool late_mouth = false;
    for (size_t i = 0; i < g.level_count; i++) {
        late_mouth = late_mouth || (g.levels[i].level >= 100 && g.levels[i].at_us > last_speech + 250 * 1000 &&
                                    g.levels[i].at_us < done);
    }
    CHECK(late_mouth, "the delayed speech still opened the mouth 250 ms after the last speech");
    CHECK((done - last_speech) / 1000 >= 300 + MOUTH_BASE_MS - 30 &&
          (done - last_speech) / 1000 <= 300 + MOUTH_BASE_MS + LATE_MS,
          "reported done %lld ms after the last speech", (long long)((done - last_speech) / 1000));
    CHECK(g.last_level == 0, "the mouth closed at the end, level %u", g.last_level);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* A stop drops the mouth values still waiting: none reaches the face. */
static int scenario_mouth_cancel(void)
{
    pet_audio_set_speech_mouth_offset(300);
    if (stream_reply(81, 24000, 4, 0, 0)) return 1;
    s_expected_speech_frames = 1;
    REQUIRE(wait_for(speech_written, 2000), "speech began");
    pthread_mutex_lock(&g_lock);
    int64_t written = first_speech_write_us();
    pthread_mutex_unlock(&g_lock);
    sleep_until(written + 100 * 1000);
    pet_audio_playback_cancel();
    vTaskDelay(600);
    pthread_mutex_lock(&g_lock);
    int64_t shown = first_mouth_us();
    CHECK(!shown, "a mouth value reached the face %lld ms after speech began",
          (long long)((shown - written) / 1000));
    CHECK(g.last_level == 0, "the mouth is closed, level %u", g.last_level);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* pet_vnext.c audio_chunk: a speech frame waits, in 10 ms polls and for up to
 * 600 ms, for room for itself and the end marker. A frame the speaker still
 * refuses (ESP_ERR_NO_MEM) is skipped, and the reply goes on. */
#define SPEAKER_ROOM_WAIT_MS 600
#define SPEAKER_ROOM_POLL_MS 10
/* The face shows a value half its tick and half LVGL's refresh after it is
 * given one, and the model has no codec: a value reaches the face this long
 * before the middle of its slice is heard. */
#define MOUTH_HEARD_LEAD_MS ((33 + 20) / 2 - 5)

static esp_err_t enqueue_like_vnext(uint32_t stream, uint32_t sequence, const int16_t *pcm, size_t samples)
{
    for (unsigned waited = 0; pet_audio_is_playing() && !pet_audio_playback_queue_healthy() &&
         waited < SPEAKER_ROOM_WAIT_MS; waited += SPEAKER_ROOM_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(SPEAKER_ROOM_POLL_MS));
    }
    return pet_audio_playback_enqueue(stream, sequence, (const uint8_t *)pcm, samples * sizeof(int16_t));
}

/* After a network stall a burst of speech overfills the speaker's queue, and
 * pet_vnext.c skips a frame that still finds no room after its wait. The
 * mouth follows only what the speaker plays, each value timed from its own
 * write. The skipped frames are loud and never move it; the frame heard next
 * is softer and moves it on time, and only as far as its own level. The
 * speaker stalls with its ring full, which keeps the queue full. */
static int scenario_mouth_skipped_frames(void)
{
    enum { FRAME = 960 }; /* 40 ms at 24 kHz */
    /* Softer than any slice of the loud frames: those open the mouth past
     * 150, the softer frame alone to about 55. */
    enum { SOFT_MAX_LEVEL = 100 };
    static int16_t quiet[FRAME], loud[FRAME], soft[FRAME];
    for (size_t i = 0; i < FRAME; i++) {
        loud[i] = (i / 12u) % 2u ? 12000 : -12000;
        soft[i] = (i / 12u) % 2u ? 2000 : -2000;
    }
    pet_audio_set_speech_mouth_offset(0);
    REQUIRE(pet_audio_playback_start(91, 24000, PET_REALTIME_BOOST_OFF,
                                     PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED) == ESP_OK, "start");
    uint32_t sequence = 0;
    /* Six frames prime playback; four more follow in real time. */
    for (int i = 0; i < 10; i++) {
        if (i >= 6) vTaskDelay(40);
        REQUIRE(enqueue_like_vnext(91, sequence++, quiet, FRAME) == ESP_OK, "quiet frame %d", i);
    }
    vTaskDelay(60);
    speaker_stall(true);
    /* The playback task blocks in its next write. The burst takes every place
     * but the last at once; the next frame waits, then takes that one. */
    vTaskDelay(60);
    for (int burst = 0; pet_audio_playback_queue_healthy(); burst++) {
        REQUIRE(burst < 40, "the queue never filled");
        REQUIRE(enqueue_like_vnext(91, sequence++, quiet, FRAME) == ESP_OK, "burst frame %d", burst);
    }
    REQUIRE(enqueue_like_vnext(91, sequence++, quiet, FRAME) == ESP_OK, "the last place");
    int skipped = 0;
    for (int i = 0; i < 2; i++) {
        int64_t before = now_us();
        esp_err_t result = enqueue_like_vnext(91, sequence++, loud, FRAME);
        int64_t waited_ms = (now_us() - before) / 1000;
        CHECK(result == ESP_ERR_NO_MEM && waited_ms >= SPEAKER_ROOM_WAIT_MS,
              "loud frame %d: result %d after %lld ms, expected a skip after the wait", i, (int)result,
              (long long)waited_ms);
        skipped += result == ESP_ERR_NO_MEM;
    }
    REQUIRE(skipped == 2, "%d frames were skipped", skipped);
    speaker_stall(false);
    /* The frame heard right after the skipped ones. */
    REQUIRE(enqueue_like_vnext(91, sequence++, soft, FRAME) == ESP_OK, "the frame after the skip");
    for (int i = 0; i < 8; i++) {
        REQUIRE(enqueue_like_vnext(91, sequence++, quiet, FRAME) == ESP_OK, "quiet frame %d after the skip", i);
    }
    pet_audio_playback_finish(91);
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 3000), "the reply was reported done");
    vTaskDelay(100);

    pthread_mutex_lock(&g_lock);
    /* The next frame's first slice: its level is computed just before its
     * write, and its middle is heard half a slice before the ring finishes it. */
    int64_t computed = 0, heard_middle = 0;
    for (size_t i = 0; i < g.write_count && !computed; i++) {
        if (g.writes[i].silent) continue;
        computed = g.writes[i].start_us;
        heard_middle = g.writes[i].heard_end_us - 10000;
    }
    int64_t shown = first_mouth_us();
    uint8_t highest = 0;
    for (size_t i = 0; i < g.level_count; i++) {
        if (g.levels[i].level > highest) highest = g.levels[i].level;
    }
    int done_count = g.done_count;
    uint8_t last_level = g.last_level;
    pthread_mutex_unlock(&g_lock);
    REQUIRE(computed && shown, "a speech write (%lld) and a mouth value (%lld)", (long long)computed, (long long)shown);
    double onset_ms = (shown - computed) / 1000.0;
    double lead_ms = (heard_middle - shown) / 1000.0;
    printf("%d frames skipped after %d ms waits; the next frame moved the mouth %.1f ms after its level was "
           "computed, %.1f ms before its middle was heard; highest level %u\n", skipped, SPEAKER_ROOM_WAIT_MS,
           onset_ms, lead_ms, highest);
    CHECK(shown >= computed, "the mouth moved %.1f ms before the next frame reached the speaker", -onset_ms);
    CHECK(highest && highest < SOFT_MAX_LEVEL, "the mouth reached %u: the skipped frames moved it", highest);
    CHECK(onset_ms >= MOUTH_BASE_MS - MOUTH_EARLY_MS && onset_ms <= MOUTH_BASE_MS + MOUTH_LATE_MS,
          "the mouth moved %.1f ms after the next frame's level was computed, expected %d", onset_ms, MOUTH_BASE_MS);
    CHECK(lead_ms >= MOUTH_HEARD_LEAD_MS - MOUTH_LATE_MS && lead_ms <= MOUTH_HEARD_LEAD_MS + MOUTH_EARLY_MS,
          "the mouth moved %.1f ms before the next frame was heard, expected %d", lead_ms, MOUTH_HEARD_LEAD_MS);
    CHECK(done_count == 1, "the reply was reported %d times", done_count);
    CHECK(last_level == 0, "the mouth closed at the end, level %u", last_level);
    return 0;
}

/* ---- Story loops ------------------------------------------------------ */

/* The test loops (test_playback_tail.py): a steady tone at half of full
 * scale, 500 Hz and victory's 750 Hz. pet_audio.c plays them at a quarter
 * under speech and at 40% after it. */
#define LOOP_PEAK 16384
#define UNDER_SPEECH_PEAK (LOOP_PEAK / 8)
#define HOLD_PEAK (LOOP_PEAK / 5)
/* Tap to talk in a loop's hold waits for one 20 ms fade block and silence
 * for the ring and one DMA buffer more, after the codec takeover. */
#define LOOP_YIELD_MS (TAKEOVER_MS + 20 + DMA_MS + 10)

static bool near(uint16_t peak, int expected) { return peak >= expected * 9 / 10 && peak <= expected * 11 / 10; }

static int64_t reply_done_us(void)
{
    s_expected_done = 1;
    if (!wait_for(reply_done, 3000)) return 0;
    pthread_mutex_lock(&g_lock);
    int64_t done = g.done_us[0];
    pthread_mutex_unlock(&g_lock);
    return done;
}

/* Called with g_lock held: the largest peak of the writes started in [from, to). */
static uint16_t peak_between(int64_t from_us, int64_t to_us)
{
    uint16_t peak = 0;
    for (size_t i = 0; i < g.write_count; i++) {
        if (g.writes[i].start_us >= from_us && g.writes[i].start_us < to_us && g.writes[i].peak > peak) {
            peak = g.writes[i].peak;
        }
    }
    return peak;
}

/* Called with g_lock held: the last write that carried sound. */
static const write_record_t *last_sound(void)
{
    for (size_t i = g.write_count; i > 0; i--) {
        if (!g.writes[i - 1].silent) return &g.writes[i - 1];
    }
    return NULL;
}

static int scenario_atmosphere_names(void)
{
    static const char *const names[] = {
        "calm", "forest", "water", "night", "cave", "storm", "danger", "battle", "magic", "victory",
    };
    for (int i = 0; i < 10; i++) {
        CHECK(pet_atmosphere_from_name(names[i]) == (pet_atmosphere_t)(PET_ATMOSPHERE_CALM + i), "%s", names[i]);
    }
    static const char *const unknown[] = { "", "none", "Storm", "storm ", "rain" };
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        CHECK(pet_atmosphere_from_name(unknown[i]) == PET_ATMOSPHERE_NONE, "\"%s\" names a loop", unknown[i]);
    }
    CHECK(pet_atmosphere_from_name(NULL) == PET_ATMOSPHERE_NONE, "NULL names a loop");
    return 0;
}

/* Speech plays 1 dB (two volume steps) above the setting on the codec, never
 * in the samples; muted stays muted, and the top of the scale is the limit. */
static int scenario_speech_volume_lift(void)
{
    static const struct {
        uint8_t setting;
        int speech;
    } cases[] = { { 55, 57 }, { 0, 0 }, { 99, 100 } };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pet_audio_set_volume(cases[i].setting);
        if (stream_reply((uint32_t)(301 + i), 24000, 4, 0, 0)) return 1;
        pthread_mutex_lock(&g_lock);
        int volume = g.speaker_volume;
        pthread_mutex_unlock(&g_lock);
        CHECK(volume == cases[i].speech, "volume %u played speech at %d, expected %d", cases[i].setting, volume,
              cases[i].speech);
    }
    return 0;
}

/* Cued before a reply, the loop fades in under it at a quarter of its level,
 * in every slice. The mouth follows the speech alone: this reply is silent,
 * so the mouth never opens. */
static int scenario_atmosphere_under_speech(void)
{
    pet_atmosphere_cue(PET_ATMOSPHERE_STORM);
    s_speech_level = 0;
    if (stream_reply(101, 24000, 12, 0, 0)) return 1;
    REQUIRE(reply_done_us(), "the reply was reported done");
    pthread_mutex_lock(&g_lock);
    /* 12 frames of 40 ms: the reply's first 24 writes, 20 ms each. */
    REQUIRE(g.write_count >= 24, "%zu writes", g.write_count);
    bool every = true;
    uint16_t faded_in = 0;
    for (size_t i = 0; i < 24; i++) {
        every = every && !g.writes[i].silent;
        if (i >= 18 && g.writes[i].peak > faded_in) faded_in = g.writes[i].peak;
    }
    uint16_t first = g.writes[0].peak;
    uint8_t mouth = 0;
    for (size_t i = 0; i < g.level_count; i++) {
        if (g.levels[i].level > mouth) mouth = g.levels[i].level;
    }
    pthread_mutex_unlock(&g_lock);
    CHECK(every, "a slice of the reply played without the loop");
    CHECK(first < UNDER_SPEECH_PEAK / 4, "the loop began at %u: no fade-in", first);
    CHECK(near(faded_in, UNDER_SPEECH_PEAK), "under speech the loop peaked at %u, expected %d", faded_in,
          UNDER_SPEECH_PEAK);
    CHECK(mouth == 0, "the loop opened the mouth to %u", mouth);
    return 0;
}

/* After the reply the loop holds the speaker at 40%, past the usual tail.
 * Tap to talk: one block fades it out and silence follows until the ring has
 * played the fade; only then does the speaker close and the microphone open. */
static int scenario_atmosphere_hold_microphone(void)
{
    pet_atmosphere_cue(PET_ATMOSPHERE_CALM);
    if (stream_reply(111, 24000, 6, 0, 0)) return 1;
    int64_t done = reply_done_us();
    REQUIRE(done, "the reply was reported done");
    sleep_until(done + (TAIL_MS + 500) * 1000);
    REQUIRE(speaker_opens_minus_closes() == 1, "the loop holds the speaker past the %lld ms tail", (long long)TAIL_MS);
    int64_t before = now_us();
    REQUIRE(pet_audio_capture_start(112) == ESP_OK, "the microphone may start in the hold");
    REQUIRE(wait_for(microphone_opened, 1000), "the microphone opened");
    vTaskDelay(60);
    pet_audio_capture_stop();
    REQUIRE(wait_for(capture_finished, 1000), "the capture finished");
    pthread_mutex_lock(&g_lock);
    int64_t microphone = g.microphone_open_us, closed = g.close_us[0];
    uint16_t held = peak_between(done + 500 * 1000, before);
    const write_record_t *fade = last_sound();
    REQUIRE(fade, "the loop never played");
    uint32_t silence = 0;
    for (const write_record_t *w = fade + 1; w < g.writes + g.write_count; w++) silence += w->frames;
    printf("the microphone waited %lld ms; the hold peaked at %u\n", (long long)((microphone - before) / 1000), held);
    CHECK(near(held, HOLD_PEAK), "the hold peaked at %u, expected %d", held, HOLD_PEAK);
    CHECK(g.closes == 1 && closed >= before && closed <= microphone, "the speaker closed before the microphone");
    CHECK(fade->start_us >= before && fade->last == 0, "the loop's last block ends at %u, not faded", fade->last);
    CHECK(fade->heard_end_us <= closed, "the speaker closed %.1f ms before the fade was heard",
          (fade->heard_end_us - closed) / 1000.0);
    CHECK(silence >= DMA_FRAMES + PET_AUDIO_DMA_FRAMES, "%u frames of silence after the fade", silence);
    CHECK(microphone - before <= LOOP_YIELD_MS * 1000, "the microphone waited %lld ms",
          (long long)((microphone - before) / 1000));
    CHECK(g.done_count == 1 && g.writes_while_closed == 0, "done %d, writes while closed %d", g.done_count,
          g.writes_while_closed);
    pthread_mutex_unlock(&g_lock);
    CHECK(speaker_writes_started_after(microphone) == 0, "the speaker was written after the microphone opened");
    return 0;
}

/* A cue for no loop while the loop holds the speaker after a 16 kHz reply: it
 * fades out over half a second, then silence until the speaker closes, long
 * before the hold would end. */
static int scenario_atmosphere_off(void)
{
    pet_atmosphere_cue(PET_ATMOSPHERE_NIGHT);
    if (stream_reply(121, 16000, 6, 0, 0)) return 1;
    int64_t done = reply_done_us();
    REQUIRE(done, "the reply was reported done");
    sleep_until(done + 600 * 1000);
    int64_t cued = now_us();
    pet_atmosphere_cue(PET_ATMOSPHERE_NONE);
    s_expected_closes = 1;
    REQUIRE(wait_for(speaker_closed, 2000), "the speaker closed");
    pthread_mutex_lock(&g_lock);
    int64_t closed = g.close_us[0];
    uint16_t early = peak_between(cued, cued + 100 * 1000), late = peak_between(cued + 350 * 1000, cued + 450 * 1000);
    const write_record_t *fade = last_sound();
    REQUIRE(fade, "the loop never played");
    int64_t faded_ms = (fade->start_us - cued) / 1000, closed_ms = (closed - cued) / 1000;
    pthread_mutex_unlock(&g_lock);
    printf("the loop faded from %u to %u and ended %lld ms after the cue; the speaker closed at %lld ms\n", early, late,
           (long long)faded_ms, (long long)closed_ms);
    CHECK(near(early, HOLD_PEAK) && late < early / 2, "the loop went from %u to %u: no fade", early, late);
    CHECK(faded_ms >= 450 && faded_ms <= 500 + LATE_MS, "the loop ended %lld ms after the cue", (long long)faded_ms);
    CHECK(fade->heard_end_us <= closed, "the speaker closed before the fade was heard");
    CHECK(closed_ms >= 500 && closed_ms <= 500 + DRAIN_FLOOR_MS + LATE_MS, "the speaker closed %lld ms after the cue",
          (long long)closed_ms);
    return 0;
}

/* While the loop holds the speaker after a reply, a sound cue is refused
 * without touching the speaker, and the loop plays on. */
static int scenario_atmosphere_refuses_cues(void)
{
    pet_atmosphere_cue(PET_ATMOSPHERE_MAGIC);
    if (stream_reply(131, 24000, 4, 0, 0)) return 1;
    int64_t done = reply_done_us();
    REQUIRE(done, "the reply was reported done");
    sleep_until(done + 600 * 1000);
    static int16_t cue[2400];
    for (size_t i = 0; i < sizeof(cue) / sizeof(cue[0]); i++) cue[i] = (i / 6u) % 2u ? 3000 : -3000;
    int64_t before = now_us();
    CHECK(pet_audio_play_local((const uint8_t *)cue, sizeof(cue), NULL, NULL) == ESP_ERR_INVALID_STATE,
          "the cue was not refused");
    vTaskDelay(200);
    pthread_mutex_lock(&g_lock);
    CHECK(g.opens == 1 && g.closes == 0, "the cue touched the speaker: opens %d closes %d", g.opens, g.closes);
    uint16_t after = peak_between(before, now_us());
    pthread_mutex_unlock(&g_lock);
    CHECK(near(after, HOLD_PEAK), "after the cue the loop peaked at %u", after);
    return 0;
}

/* Speech near full scale (94%) under a loop: the loop gives way to the
 * headroom the speech leaves, so nothing reaches the int16 limits and the
 * reply counts nothing limited. */
static int scenario_atmosphere_loud_speech(void)
{
    enum { LOUD = 30800 };
    pet_atmosphere_cue(PET_ATMOSPHERE_CALM);
    s_speech_level = LOUD;
    if (stream_reply(141, 24000, 12, 0, 0)) return 1;
    REQUIRE(reply_done_us(), "the reply was reported done");
    pthread_mutex_lock(&g_lock);
    uint16_t loudest = 0;
    int clipped = 0;
    for (size_t i = 0; i < 24 && i < g.write_count; i++) {
        if (g.writes[i].peak > loudest) loudest = g.writes[i].peak;
        clipped += g.writes[i].peak >= INT16_MAX;
    }
    pthread_mutex_unlock(&g_lock);
    long limited = stats_value("limited");
    CHECK(clipped == 0 && limited == 0, "%d slices reached full scale; limited=%ld", clipped, limited);
    CHECK(loudest > LOUD, "the loop vanished under the speech (peak %u)", loudest);
    return 0;
}

/* The blessing path: in a hold, a cue for another loop, then the next reply.
 * The codec closes only once its fade is heard, and the new loop plays under
 * the new reply. */
static int scenario_atmosphere_cue_then_reply(void)
{
    pet_atmosphere_cue(PET_ATMOSPHERE_CALM);
    if (stream_reply(151, 24000, 6, 0, 0)) return 1;
    int64_t done = reply_done_us();
    REQUIRE(done, "the reply was reported done");
    sleep_until(done + 600 * 1000);
    pet_atmosphere_cue(PET_ATMOSPHERE_VICTORY);
    vTaskDelay(40);
    s_speech_level = 0;
    if (stream_reply(152, 16000, 12, 0, 0)) return 1;
    s_expected_done = 2;
    REQUIRE(wait_for(reply_done, 3000), "the second reply was reported done");
    pthread_mutex_lock(&g_lock);
    int64_t closed = g.close_us[0], reopened = g.open_us[1];
    const write_record_t *fade = NULL;
    uint32_t crossings = 0, frames = 0;
    for (size_t i = 0; i < g.write_count; i++) {
        const write_record_t *w = &g.writes[i];
        if (w->start_us < closed && !w->silent) fade = w;
        if (w->start_us >= reopened + 150 * 1000 && w->start_us < reopened + 400 * 1000) {
            crossings += w->crossings;
            frames += w->frames;
        }
    }
    pthread_mutex_unlock(&g_lock);
    double hz = frames ? crossings / 2.0 * 16000.0 / frames : 0.0;
    CHECK(fade && fade->heard_end_us <= closed, "the codec closed before the hold's fade was heard");
    CHECK(hz > 700 && hz < 800, "under the new reply the loop is at %.0f Hz, not victory's 750", hz);
    return 0;
}

/* A stop between a cue and its reply drops the cue: the reply has no loop. */
static int scenario_atmosphere_stop_drops_cue(void)
{
    pet_atmosphere_cue(PET_ATMOSPHERE_STORM);
    pet_audio_playback_cancel();
    s_speech_level = 0;
    if (stream_reply(161, 24000, 6, 0, 0)) return 1;
    REQUIRE(reply_done_us(), "the reply was reported done");
    pthread_mutex_lock(&g_lock);
    bool sound = last_sound() != NULL;
    pthread_mutex_unlock(&g_lock);
    CHECK(!sound, "the reply played the loop the stop dropped");
    return 0;
}

/* Left alone, the hold ends ATMOSPHERE_HOLD_MS after the reply's last frame
 * (the host build shortens it): the loop fades out over a second, then the
 * speaker closes once the ring has played the fade. */
static int scenario_atmosphere_hold_expires(void)
{
    pet_atmosphere_cue(PET_ATMOSPHERE_WATER);
    if (stream_reply(171, 24000, 6, 0, 0)) return 1;
    int64_t done = reply_done_us();
    REQUIRE(done, "the reply was reported done");
    s_expected_closes = 1;
    REQUIRE(wait_for(speaker_closed, ATMOSPHERE_HOLD_MS + 3000), "the hold never ended");
    pthread_mutex_lock(&g_lock);
    int64_t closed = g.close_us[0], closed_ms = (closed - done) / 1000;
    uint16_t held = peak_between(done + (ATMOSPHERE_HOLD_MS - 1000) * 1000, done + (ATMOSPHERE_HOLD_MS - 600) * 1000);
    uint16_t fading = peak_between(done + (ATMOSPHERE_HOLD_MS + 400) * 1000, done + (ATMOSPHERE_HOLD_MS + 500) * 1000);
    const write_record_t *fade = last_sound();
    REQUIRE(fade, "the loop never played");
    bool heard = fade->heard_end_us <= closed;
    pthread_mutex_unlock(&g_lock);
    CHECK(near(held, HOLD_PEAK) && fading < held / 2, "the loop went from %u to %u: no fade", held, fading);
    CHECK(heard, "the speaker closed before the fade was heard");
    CHECK(closed_ms >= ATMOSPHERE_HOLD_MS + 600 && closed_ms <= ATMOSPHERE_HOLD_MS + 1000 + DRAIN_FLOOR_MS + LATE_MS,
          "the speaker closed %lld ms after the reply", (long long)closed_ms);
    return 0;
}

/* Called with g_lock held: the frequency of the writes started in [from,
 * to), from their sign changes. */
static double hz_between(int64_t from_us, int64_t to_us)
{
    uint32_t crossings = 0, frames = 0, rate = 0;
    for (size_t i = 0; i < g.write_count; i++) {
        const write_record_t *w = &g.writes[i];
        if (w->start_us < from_us || w->start_us >= to_us) continue;
        crossings += w->crossings;
        frames += w->frames;
        rate = w->rate;
    }
    return frames ? crossings / 2.0 * rate / frames : 0.0;
}

/* The frames of a reply already started, as stream_reply sends them. */
static int send_frames(uint32_t stream, uint32_t rate, int frames)
{
    int16_t pcm[1024];
    size_t samples = rate / 25u;
    for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)((i / 12u) % 2u ? s_speech_level : -s_speech_level);
    for (int i = 0; i < frames; i++) {
        REQUIRE(pet_audio_playback_enqueue(stream, (uint32_t)i, (const uint8_t *)pcm,
                                           samples * sizeof(int16_t)) == ESP_OK, "enqueue %d", i);
    }
    pet_audio_playback_finish(stream);
    return 0;
}

/* Three cues queued before a reply: the dice (1.008 s), the hit (0.504 s) and
 * the dice again play first, in order, in its held silence, then its speech,
 * and the played callback runs once between. A fourth cue, or a loop, is not
 * queued. */
static int scenario_sounds_before_speech(void)
{
    REQUIRE(pet_atmosphere_queue_sound("dice") && pet_atmosphere_queue_sound("hit") &&
            pet_atmosphere_queue_sound("dice"), "three cues queue");
    CHECK(!pet_atmosphere_queue_sound("hit"), "a fourth cue queued");
    CHECK(!pet_atmosphere_queue_sound("campfire"), "a loop queued as a cue");
    s_speech_level = 0;
    int64_t started = now_us();
    REQUIRE(pet_audio_playback_start(181, 16000, PET_REALTIME_BOOST_OFF,
                                     PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED) == ESP_OK, "start");
    REQUIRE(pet_audio_playback_hold_for_sounds(sounds_played) && pet_audio_playback_held(), "the reply waits");
    if (send_frames(181, 16000, 12)) return 1;
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 6000), "the reply was reported done");
    pthread_mutex_lock(&g_lock);
    int64_t done = g.done_us[0], played_ms = (g_played_us - started) / 1000;
    int played = g_played;
    uint32_t stream = g_played_stream;
    double first = hz_between(started + 150000, started + 800000);
    double second = hz_between(started + 1150000, started + 1400000);
    double third = hz_between(started + 1700000, started + 2300000);
    pthread_mutex_unlock(&g_lock);
    printf("the cues played for %lld ms at %.0f, %.0f and %.0f Hz\n", (long long)played_ms, first, second, third);
    CHECK(played == 1 && stream == 181, "the played callback ran %d times", played);
    CHECK(played_ms >= 2350 && played_ms <= 2520 + LATE_MS, "the cues played for %lld ms", (long long)played_ms);
    CHECK(done - g_played_us >= 400000, "the speech was done %lld ms after the cues",
          (long long)((done - g_played_us) / 1000));
    CHECK(first > 900 && first < 1100 && second > 1350 && second < 1650 && third > 900 && third < 1100,
          "the cues played out of order");
    CHECK(!pet_audio_playback_hold_for_sounds(sounds_played), "a reply still waits");
    return 0;
}

/* Playing on (the owner's tap) during the cues: the speech plays at once,
 * the cue playing goes on under it, the one waiting never plays and no
 * callback runs. */
static int scenario_sounds_play_on(void)
{
    REQUIRE(pet_atmosphere_queue_sound("dice") && pet_atmosphere_queue_sound("hit"), "two cues queue");
    s_speech_level = 0;
    int64_t started = now_us();
    REQUIRE(pet_audio_playback_start(191, 16000, PET_REALTIME_BOOST_OFF,
                                     PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED) == ESP_OK, "start");
    REQUIRE(pet_audio_playback_hold_for_sounds(sounds_played), "the reply waits");
    if (send_frames(191, 16000, 12)) return 1;
    sleep_until(started + 300000);
    REQUIRE(pet_audio_playback_hold(false) && !pet_audio_playback_held(), "the reply plays on");
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 3000), "the reply was reported done");
    sleep_until(started + 1500000);
    pthread_mutex_lock(&g_lock);
    int64_t done_ms = (g.done_us[0] - started) / 1000;
    double under = hz_between(started + 400000, started + 900000);
    double skipped = hz_between(started + 1150000, started + 1400000);
    int played = g_played;
    pthread_mutex_unlock(&g_lock);
    CHECK(done_ms < 1600, "the speech waited for the cues: done after %lld ms", (long long)done_ms);
    CHECK(under > 900 && under < 1100, "the dice went on under the speech at %.0f Hz", under);
    CHECK(skipped < 100 && !played && !pet_atmosphere_sounds_waiting(), "the hit played (%.0f Hz) or waits", skipped);
    return 0;
}

/* Held for the owner to talk over it (speech-pause-listen-v1). A plain hold
 * still refuses a cue and the microphone. Held to listen, the reply gives the
 * speaker up by itself at once and writes nothing more: written on, its
 * silence would hold the codec all but an instant of each block, and the
 * listening cue waiting for it could wait for ever. The cue then takes the
 * codec without waiting, and the microphone after it, while the reply's
 * frames wait; playing on, the reply opens the speaker again at its own rate,
 * plays every held frame and is reported done once. The listen is no gap in
 * its feed. */
static int scenario_hold_to_listen(void)
{
    static int16_t cue[2400];
    for (size_t i = 0; i < sizeof(cue) / sizeof(cue[0]); i++) cue[i] = (i / 6u) % 2u ? 3000 : -3000;
    REQUIRE(pet_audio_playback_start(221, 16000, PET_REALTIME_BOOST_OFF,
                                     PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED) == ESP_OK, "start");
    REQUIRE(pet_audio_playback_hold(true), "a plain hold");
    CHECK(pet_audio_play_local((const uint8_t *)cue, sizeof(cue), NULL, NULL) == ESP_ERR_INVALID_STATE,
          "a plain hold gave the speaker to a cue");
    CHECK(pet_audio_capture_start(222) == ESP_ERR_INVALID_STATE, "a plain hold gave the speaker to the microphone");
    REQUIRE(pet_audio_playback_hold(false), "a plain hold plays on");
    /* Half of the reply plays, then it is held to listen. */
    int16_t pcm[640];
    for (size_t i = 0; i < 640; i++) pcm[i] = (int16_t)((i / 12u) % 2u ? s_speech_level : -s_speech_level);
    for (uint32_t i = 0; i < 12; i++) {
        if (i == 6) {
            s_expected_speech_frames = 2 * 640;
            REQUIRE(wait_for(speech_written, 2000), "the reply began");
            REQUIRE(pet_audio_playback_hold_to_listen(), "held to listen");
        }
        REQUIRE(pet_audio_playback_enqueue(221, i, (const uint8_t *)pcm, sizeof(pcm)) == ESP_OK, "enqueue %u", i);
    }
    pet_audio_playback_finish(221);
    s_expected_closes = 1;
    REQUIRE(wait_for(speaker_closed, 500), "held to listen, the reply kept the speaker");
    int64_t asked = now_us();
    REQUIRE(pet_audio_play_local((const uint8_t *)cue, sizeof(cue), NULL, NULL) == ESP_OK, "the listening cue played");
    REQUIRE(pet_audio_capture_start(222) == ESP_OK, "the microphone may start over the held reply");
    REQUIRE(wait_for(microphone_opened, 500), "the microphone opened");
    vTaskDelay(400);
    pet_audio_capture_stop();
    REQUIRE(wait_for(capture_finished, 1000), "the capture finished");
    vTaskDelay(100);
    int64_t resumed = now_us();
    REQUIRE(pet_audio_playback_hold(false), "the reply plays on");
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 3000), "the reply was reported done");
    pthread_mutex_lock(&g_lock);
    int64_t yielded = g.close_us[0];
    size_t written_meanwhile = 0;
    for (size_t i = 0; i < g.write_count; i++) {
        written_meanwhile += g.writes[i].start_us >= yielded && g.writes[i].start_us < resumed &&
                             g.writes[i].rate != 24000;
    }
    CHECK(g.writes_while_closed == 0 && !written_meanwhile, "the speaker was written closed (%d), or by the held "
          "reply (%zu)", g.writes_while_closed, written_meanwhile);
    CHECK(g.open_us[1] - asked <= TAKEOVER_MS * 1000, "the listening cue waited %lld ms for the codec",
          (long long)((g.open_us[1] - asked) / 1000));
    CHECK(g.opens == 3 && g.open_rate[1] == 24000 && g.open_rate[2] == 16000 && g.open_us[2] >= resumed,
          "opens %d: the reply did not open the speaker again at its rate after playing on", g.opens);
    CHECK(g.speech_frames == 2400 + 12 * 640, "%llu frames of the cue and the speech were written",
          (unsigned long long)g.speech_frames);
    CHECK(g.done_count == 1 && g.done_stream[0] == 221, "done %d", g.done_count);
    pthread_mutex_unlock(&g_lock);
    long gap = stats_value("feed_gap_ms");
    CHECK(gap >= 0 && gap < LATE_MS, "the reply's feed gap was %ld ms", gap);
    return 0;
}

/* A stinger plays in a reply's tail and holds the speaker until it has
 * played, past the tail's end. With the speaker closed it opens it at 16 kHz
 * for the stinger alone and closes it after. An id the bank lacks opens
 * nothing. */
static int scenario_stinger(void)
{
    if (reply_then_wait_in_tail(201, TAIL_MS - 300)) return 1;
    int64_t asked = now_us();
    REQUIRE(pet_audio_play_story_sound("hit"), "the stinger plays in the tail");
    s_expected_closes = 1;
    REQUIRE(wait_for(speaker_closed, 3000), "the speaker closed");
    pthread_mutex_lock(&g_lock);
    int64_t closed_ms = (g.close_us[0] - asked) / 1000;
    double tail = hz_between(asked + 100000, asked + 400000);
    pthread_mutex_unlock(&g_lock);
    CHECK(tail > 1400 && tail < 1600, "in the tail the stinger played at %.0f Hz", tail);
    CHECK(closed_ms >= 600 && closed_ms <= 504 + DRAIN_FLOOR_MS + LATE_MS, "the tail closed %lld ms after it began",
          (long long)closed_ms);
    CHECK(!pet_audio_play_story_sound("nothing"), "an id the bank lacks played");
    vTaskDelay(60);
    CHECK(speaker_opens_minus_closes() == 0, "an id the bank lacks opened the speaker");
    int64_t alone = now_us();
    REQUIRE(pet_audio_play_story_sound("hit"), "the stinger plays alone");
    s_expected_closes = 2;
    REQUIRE(wait_for(speaker_closed, 3000), "the speaker closed again");
    pthread_mutex_lock(&g_lock);
    int opens = g.opens;
    uint32_t rate = g.open_rate[1];
    closed_ms = (g.close_us[1] - alone) / 1000;
    double heard = hz_between(alone + 100000, alone + 400000);
    pthread_mutex_unlock(&g_lock);
    CHECK(opens == 2 && rate == 16000, "the stinger opened the speaker at %u Hz (opens %d)", rate, opens);
    CHECK(heard > 1400 && heard < 1600, "alone the stinger played at %.0f Hz", heard);
    CHECK(closed_ms >= 500 && closed_ms <= 504 + DRAIN_FLOOR_MS + LATE_MS, "the speaker closed %lld ms after it began",
          (long long)closed_ms);
    return 0;
}

/* story.atmosphere naming a loop of the sound bank: it plays under a reply
 * as a built-in loop does (atmosphere-under-speech). A one-shot's id names no
 * loop. */
static int scenario_atmosphere_bank_loop(void)
{
    s_speech_level = 0;
    pet_atmosphere_cue_name("dice");
    if (stream_reply(211, 16000, 6, 0, 0)) return 1;
    REQUIRE(reply_done_us(), "the first reply was reported done");
    pthread_mutex_lock(&g_lock);
    bool sound = last_sound() != NULL;
    pthread_mutex_unlock(&g_lock);
    CHECK(!sound, "a one-shot's id played as a loop");
    pet_atmosphere_cue_name("campfire");
    int64_t second = now_us();
    if (stream_reply(212, 16000, 12, 0, 0)) return 1;
    s_expected_done = 2;
    REQUIRE(wait_for(reply_done, 3000), "the second reply was reported done");
    /* Its 19th to 24th 20 ms writes, after the loop's fade-in. */
    pthread_mutex_lock(&g_lock);
    uint16_t faded_in = 0;
    uint32_t crossings = 0, frames = 0;
    for (size_t i = 0, n = 0; i < g.write_count && n < 24; i++) {
        if (g.writes[i].start_us < second || n++ < 18) continue;
        if (g.writes[i].peak > faded_in) faded_in = g.writes[i].peak;
        crossings += g.writes[i].crossings;
        frames += g.writes[i].frames;
    }
    pthread_mutex_unlock(&g_lock);
    double hz = frames ? crossings / 2.0 * 16000.0 / frames : 0.0;
    CHECK(near(faded_in, UNDER_SPEECH_PEAK), "under speech the bank loop peaked at %u, expected %d", faded_in,
          UNDER_SPEECH_PEAK);
    CHECK(hz > 1900 && hz < 2100, "the bank loop played at %.0f Hz, not 2000", hz);
    return 0;
}

/* A blocked flash read must not hold the codec lock or delay the reply. */
static int scenario_stalled_sound_read(void)
{
    /* An index queued before replacement cannot identify a new-bank sound. */
    REQUIRE(pet_atmosphere_queue_sound("hit"), "queued the old-bank cue");
    atomic_fetch_add(&bank_generation, 1);
    CHECK(!pet_atmosphere_play_queued(), "an old-bank cue survived replacement");
    atomic_store(&bank_stalled, true);
    pet_atmosphere_cue_name("campfire");
    if (stream_reply(231, 16000, 12, 0, 0)) return 1;
    s_expected_done = 1;
    REQUIRE(wait_for(reply_done, 2000), "speech completed while the driver remained blocked");
    REQUIRE(atomic_load(&bank_entered), "the media task entered the flash read");
    REQUIRE(reply_done_us(), "speech drained while the sound read stayed blocked");
    long gap = stats_value("feed_gap_ms");
    CHECK(gap >= 0 && gap < LATE_MS, "blocked sound read left a %ld ms speech gap", gap);
    pet_audio_playback_cancel();
    atomic_fetch_add(&bank_generation, 1);
    atomic_store(&bank_stalled, false);
    vTaskDelay(100);
    CHECK(!pet_atmosphere_active(), "cancelled old-bank sound became audible after its read completed");
    return 0;
}

static void *media_worker(void *unused)
{
    (void)unused;
    media_thread = pthread_self();
    for (;;)
    {
        pet_atmosphere_prepare();
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t media;
    assert(!pthread_create(&media, NULL, media_worker, NULL));
    g_verbose = getenv("AUDIO_HOST_VERBOSE") != NULL;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        return 2;
    }
    if (pet_audio_init(capture_chunk, capture_done, playback_done) != ESP_OK) {
        fprintf(stderr, "pet_audio_init failed\n");
        return 2;
    }
    static const struct {
        const char *name;
        int (*run)(void);
    } scenarios[] = {
        {"complete", scenario_complete},
        {"stop-in-tail", scenario_stop_in_tail},
        {"stop-while-draining", scenario_stop_while_draining},
        {"new-reply-in-tail", scenario_new_reply_in_tail},
        {"capture-in-tail", scenario_capture_in_tail},
        {"cue-in-tail", scenario_cue_in_tail},
        {"dma-underflow", scenario_dma_underflow},
        {"slow-link", scenario_slow_link},
        {"mouth-default", scenario_mouth_default},
        {"mouth-later", scenario_mouth_later},
        {"mouth-earlier", scenario_mouth_earlier},
        {"mouth-earliest", scenario_mouth_earliest},
        {"mouth-clamp", scenario_mouth_clamp},
        {"mouth-drain", scenario_mouth_drain},
        {"mouth-cancel", scenario_mouth_cancel},
        {"mouth-skipped-frames", scenario_mouth_skipped_frames},
        {"speech-volume-lift", scenario_speech_volume_lift},
        {"atmosphere-names", scenario_atmosphere_names},
        {"atmosphere-under-speech", scenario_atmosphere_under_speech},
        {"atmosphere-hold-microphone", scenario_atmosphere_hold_microphone},
        {"atmosphere-off", scenario_atmosphere_off},
        {"atmosphere-refuses-cues", scenario_atmosphere_refuses_cues},
        {"atmosphere-loud-speech", scenario_atmosphere_loud_speech},
        {"atmosphere-cue-then-reply", scenario_atmosphere_cue_then_reply},
        {"atmosphere-stop-drops-cue", scenario_atmosphere_stop_drops_cue},
        {"atmosphere-hold-expires", scenario_atmosphere_hold_expires},
        {"sounds-before-speech", scenario_sounds_before_speech},
        {"sounds-play-on", scenario_sounds_play_on},
        {"hold-to-listen", scenario_hold_to_listen},
        {"stinger", scenario_stinger},
        {"atmosphere-bank-loop", scenario_atmosphere_bank_loop},
        {"stalled-sound-read", scenario_stalled_sound_read},
    };
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(argv[1], scenarios[i].name)) continue;
        int failed = scenarios[i].run() || g_failures;
        printf("%s: %s\n", argv[1], failed ? "FAILED" : "passed");
        fflush(stdout);
        /* Worker tasks still run; leave without tearing them down. */
        _Exit(failed ? 1 : 0);
    }
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}
