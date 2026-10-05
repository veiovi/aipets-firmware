#include "pet_audio.h"

#include <stdlib.h>
#include <string.h>
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_audio_board.h"
#include "pet_face.h"
#include "pet_pcm_gain.h"
#include "pet_speech_mouth.h"

#define CAPTURE_SAMPLES_PER_CHUNK 320
#define CAPTURE_SLOTS 2
#define CAPTURE_DEFAULT_MAX_SECONDS 30
#define PLAYBACK_MAX_CHUNK 2048
/* About a second of 40 ms frames, in PSRAM (malloc prefers it here): room for
 * the gateway's 600 ms lead and for a rebuffering reserve. */
#define PLAYBACK_QUEUE_DEPTH 25
#define PLAYBACK_PRIME_CHUNKS 6
/* After an underrun the reply pauses on silence until this many frames wait,
 * and the reserve grows with each further underrun in the reply: speech that
 * arrives slower than real time (a phone hotspot) then plays in long runs with
 * a few short pauses, instead of a 20 ms gap between every frame. */
/* A gap within this many frames of the previous one rebuffers, and so does
 * speech more than one wait (20 ms) late; one short, isolated gap is bridged
 * with silence as before. */
#define PLAYBACK_REBUFFER_WINDOW_FRAMES 8
#define PLAYBACK_REBUFFER_FIRST_CHUNKS 10
#define PLAYBACK_REBUFFER_STEP_CHUNKS 4
#define PLAYBACK_REBUFFER_MAX_CHUNKS 20
#define PLAYBACK_PRIME_POLL_MS 5
#define PLAYBACK_QUEUE_WAIT_MS 20
#define PLAYBACK_SILENCE_MAX_SAMPLES 960
/* After a reply's last frame the speaker stays open this long on silence, so
 * the audio still in the I2S DMA and the codec is heard to the end. The
 * microphone, a cue, a new reply or a stop ends the tail at once. */
#define PLAYBACK_TAIL_MS CONFIG_PET_PLAYBACK_TAIL_MS
/* Tail pacing: half a 20 ms silence block. The block then waits for DMA room,
 * which keeps the DMA full without drift, and the codec lock is free while
 * this task waits. */
#define PLAYBACK_TAIL_WAIT_MS 10
/* The face goes idle and the reply is reported done once its last sample has
 * crossed the TX DMA and the codec, but never sooner than this. */
#define PLAYBACK_DRAIN_FLOOR_MS 300
/* A small allowance for the codec's filters and the amplifier after the I2S
 * frame leaves the DMA. */
#define PLAYBACK_CODEC_LATENCY_MS 5
/* 20 ms of 24 kHz mono 16-bit PCM: the latency of stopping a local cue. */
#define LOCAL_CUE_CHUNK_BYTES 960
/* The mouth follows speech in 20 ms slices. */
#define PLAYBACK_SLICE_MS 20
/* From a mouth value to the screen: the face reads it on its next tick and
 * LVGL shows the change at its next refresh, on average half of each. */
#define MOUTH_DISPLAY_LATENCY_MS ((PET_FACE_TICK_MS + CONFIG_LV_DEF_REFR_PERIOD) / 2)
/* Mouth values are released after each speaker write, about every 20 ms; one
 * due within half of that goes out at once. */
#define MOUTH_RELEASE_EARLY_US 10000
/* Playback outranks the face's renderer. The LVGL task (priority 7 on either
 * core: pet_face.c, pet_onboarding.c) decodes and draws every face frame, and
 * the speaking mouth keeps it busy for the whole reply. Below it, playback
 * could be held off longer than the TX DMA ring lasts: the ring ran dry and
 * the speaker played silence inside the speech, a click that no queue counter
 * saw. Core 1 keeps playback clear of the Wi-Fi driver's core as well. The
 * task only copies 20 ms slices into the ring, a small fraction of a core,
 * and never runs at the same time as capture (the codec lock). */
#define PLAYBACK_TASK_PRIORITY 8
#define PLAYBACK_TASK_CORE 1
#define CAPTURE_TASK_PRIORITY 7
#define CAPTURE_TASK_CORE 1

typedef struct {
    uint32_t stream_id;
    uint32_t sequence;
    size_t length;
    bool final;
    int16_t pcm[PLAYBACK_MAX_CHUNK / sizeof(int16_t)];
} playback_chunk_t;

typedef enum {
    PLAYBACK_TAIL_NONE = 0,
    /* The final marker was reached; the reply's last audio is still in the
     * DMA and the codec. The reply still counts as playing. */
    PLAYBACK_TAIL_DRAINING,
    /* The reply has been heard and reported done; the speaker stays open on
     * silence until the tail ends. */
    PLAYBACK_TAIL_HOLDING,
} playback_tail_t;

static const char *TAG = "pet_audio";
static esp_codec_dev_handle_t s_microphone;
static esp_codec_dev_handle_t s_speaker;
static QueueHandle_t s_playback_queue;
static SemaphoreHandle_t s_codec_lock;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_playback_task;
static pet_audio_capture_chunk_cb_t s_chunk_callback;
static pet_audio_capture_done_cb_t s_done_callback;
static pet_audio_playback_done_cb_t s_playback_done_callback;
static volatile bool s_capture_requested;
static volatile bool s_capture_active;
static volatile bool s_playing;
static volatile bool s_local_playing;
/* Speech playback is waiting for the codec; a local cue yields to it. */
static volatile bool s_speech_waiting;
static volatile bool s_playback_primed;
/* An underrun unprimed the reply: it pauses on silence while the queue refills
 * to s_rebuffer_chunks. */
static volatile bool s_playback_rebuffering;
static unsigned s_rebuffer_chunks;
static uint32_t s_playback_rebuffers;
/* Frames played since the queue last ran empty; only the playback task uses it. */
static unsigned s_frames_since_gap;
static volatile bool s_playback_finishing;
static volatile uint32_t s_capture_stream;
static volatile uint32_t s_playback_stream;
static volatile uint8_t s_output_volume = 55;
static volatile uint16_t s_capture_timeout_seconds = CAPTURE_DEFAULT_MAX_SECONDS;
static pet_realtime_boost_t s_playback_boost = PET_REALTIME_BOOST_OFF;
static pet_pcm_gain_stats_t s_playback_stats;
static bool s_playback_stats_active;
static pet_speech_mouth_envelope_t s_mouth_envelope;
static pet_speech_mouth_mode_t s_playback_mouth_mode =
    PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED;
static uint32_t s_playback_sample_rate = 24000;
static uint8_t s_mouth_level_min = 255;
static uint8_t s_mouth_level_max;
static uint64_t s_mouth_level_total;
static uint32_t s_mouth_level_updates;
static uint32_t s_playback_underruns;
/* The TX DMA underflow count (pet_audio_board.h) at this reply's first speaker
 * write. Before that write the ring is meant to play silence. */
static uint32_t s_dma_underflow_base;
static bool s_dma_underflow_counting;
/* Feed lateness: the longest time between two speaker writes of this reply,
 * from its first write to its final marker. A gap longer than the DMA ring
 * played silence. */
static int64_t s_feed_last_us;
static uint32_t s_feed_gap_max_us;
/* Guarded by s_codec_lock. s_tail is also read without it, only to decide
 * whether to take the lock; the decision is made again under it. */
static volatile playback_tail_t s_tail;
/* Speech or its tail has the speaker open (a cue opens and closes it within
 * one locked section). */
static bool s_speaker_open;
static uint32_t s_tail_stream;
static int64_t s_tail_drained_at_us;
static int64_t s_tail_close_at_us;
/* Mouth values waiting for their slice to be heard. Guarded by s_codec_lock. */
static pet_speech_mouth_delay_t s_mouth_delay;
static volatile int16_t s_mouth_offset_ms = PET_SPEECH_MOUTH_OFFSET_DEFAULT_MS;

static void record_mouth_level(uint8_t level)
{
    if (level < s_mouth_level_min) s_mouth_level_min = level;
    if (level > s_mouth_level_max) s_mouth_level_max = level;
    s_mouth_level_total += level;
    s_mouth_level_updates++;
}

static uint8_t mouth_frame_max(void)
{
    return 6u;
}

static void log_playback_stats(const char *reason, uint32_t stream_id)
{
    if (!s_playback_stats_active) return;
    ESP_LOGI(TAG,
             "playback audio stream=%lu reason=%s boost=%s samples=%llu input_rms=%lu "
             "output_rms=%lu input_peak=%lu output_peak=%lu limited=%llu "
             "mouth_mode=%s mouth_rms=%lu mouth_peak=%lu envelope_min=%u "
             "envelope_max=%u envelope_avg=%u frame_min=%u frame_max=%u "
             "articulation=%u underruns=%lu rebuffers=%lu dma_underflows=%lu dma_ms=%lu feed_gap_ms=%lu",
             (unsigned long)stream_id, reason, pet_realtime_boost_name(s_playback_boost),
             (unsigned long long)s_playback_stats.sample_count,
             (unsigned long)pet_pcm_gain_rms(s_playback_stats.input_square_sum,
                                             s_playback_stats.sample_count),
             (unsigned long)pet_pcm_gain_rms(s_playback_stats.output_square_sum,
                                             s_playback_stats.sample_count),
             (unsigned long)s_playback_stats.input_peak,
             (unsigned long)s_playback_stats.output_peak,
             (unsigned long long)s_playback_stats.limited_samples,
             pet_speech_mouth_mode_name(s_playback_mouth_mode),
             (unsigned long)pet_pcm_gain_rms(s_playback_stats.output_square_sum,
                                             s_playback_stats.sample_count),
             (unsigned long)s_playback_stats.output_peak,
             s_mouth_level_updates ? s_mouth_level_min : 0,
             s_mouth_level_max,
             s_mouth_level_updates ? (unsigned)(s_mouth_level_total / s_mouth_level_updates) : 0,
             s_mouth_level_updates ? (unsigned)((uint32_t)s_mouth_level_min * mouth_frame_max() / 255u) : 0,
             s_mouth_level_updates ? (unsigned)(((uint32_t)s_mouth_level_max * mouth_frame_max() + 127u) / 255u) : 0,
             s_mouth_envelope.articulation,
             (unsigned long)s_playback_underruns, (unsigned long)s_playback_rebuffers,
             (unsigned long)(s_dma_underflow_counting ?
                             pet_audio_board_tx_underflows() - s_dma_underflow_base : 0u),
             (unsigned long)(s_playback_sample_rate ?
                             (uint64_t)pet_audio_board_tx_ring_frames() * 1000u / s_playback_sample_rate : 0u),
             (unsigned long)(s_feed_gap_max_us / 1000u));
    s_playback_stats_active = false;
}

/* Time from a speaker write returning to the listener hearing its last
 * sample: the TX DMA ring the board created (pet_audio_board.h), 6 x 240
 * frames by default, 60 ms at 24 kHz, then the codec. */
static uint32_t playback_output_latency_ms(uint32_t sample_rate)
{
    const uint64_t frames = pet_audio_board_tx_ring_frames();
    if (!sample_rate) return PLAYBACK_CODEC_LATENCY_MS;
    return (uint32_t)((frames * 1000u + sample_rate - 1u) / sample_rate) + PLAYBACK_CODEC_LATENCY_MS;
}

static uint32_t playback_drain_ms(uint32_t sample_rate)
{
    uint32_t latency = playback_output_latency_ms(sample_rate);
    return latency > PLAYBACK_DRAIN_FLOOR_MS ? latency : PLAYBACK_DRAIN_FLOOR_MS;
}

/* A slice's level is computed just before its write. The write returns once
 * the ring has room for it (one slice later), and the ring then plays the
 * DMA's worth of audio ahead of its end: the middle of the slice is heard a
 * ring plus half a slice after its level is computed. */
static uint32_t mouth_audio_latency_ms(void)
{
    return playback_output_latency_ms(s_playback_sample_rate) + PLAYBACK_SLICE_MS / 2u;
}

/* Called with s_codec_lock held after each speaker write of a reply, speech or
 * the silence that bridges a late frame. The first starts the reply's DMA
 * underflow count: from then on the ring holds the reply. */
static void note_reply_write(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_dma_underflow_counting) {
        s_dma_underflow_base = pet_audio_board_tx_underflows();
        s_dma_underflow_counting = true;
    } else if (now - s_feed_last_us > (int64_t)s_feed_gap_max_us) {
        s_feed_gap_max_us = (uint32_t)(now - s_feed_last_us);
    }
    s_feed_last_us = now;
}

/* Called with s_codec_lock held: shows the newest mouth value whose slice is
 * being heard now. */
static void release_speech_mouth(void)
{
    pet_speech_mouth_sample_t sample;
    if (pet_speech_mouth_delay_pop_due(&s_mouth_delay, esp_timer_get_time() + MOUTH_RELEASE_EARLY_US,
                                       &sample)) {
        pet_face_set_speech_articulation(sample.articulation);
        pet_face_set_audio_level(sample.level);
    }
}

/* Called with s_codec_lock held, just before the slice the value came from is
 * written: the mouth shows it when the listener hears that slice. Only what
 * the speaker plays gets here, each value timed from its own write, so a
 * frame the queue refused (pet_vnext.c skips it after its wait) never moves
 * the mouth or shifts a later value. */
static void queue_speech_mouth(uint8_t level, uint8_t articulation)
{
    uint32_t delay_ms = pet_speech_mouth_delay_ms(mouth_audio_latency_ms(), MOUTH_DISPLAY_LATENCY_MS,
                                                  s_mouth_offset_ms);
    record_mouth_level(level);
    pet_speech_mouth_delay_push(&s_mouth_delay, esp_timer_get_time() + (int64_t)delay_ms * 1000,
                                level, articulation);
    release_speech_mouth();
}

static esp_err_t write_speech_chunk(playback_chunk_t *chunk)
{
    size_t samples = chunk->length / sizeof(int16_t);
    pet_pcm_gain_apply(chunk->pcm, samples, s_playback_boost, &s_playback_stats);
    /* The full-frame runtime selects the pack's authored stages. Feed it one
     * calibrated 20 ms energy stream regardless of any legacy persisted
     * renderer value; it then applies its own attack/release, hysteresis, and
     * authored minimum-hold policy. */
    size_t slice_samples = s_playback_sample_rate / 50u;
    if (!slice_samples) slice_samples = samples;
    for (size_t offset = 0; offset < samples; offset += slice_samples) {
        size_t count = samples - offset;
        if (count > slice_samples) count = slice_samples;
        uint8_t articulation = 0;
        uint8_t level = pet_speech_mouth_envelope_process(
            &s_mouth_envelope, chunk->pcm + offset, count, &articulation);
        queue_speech_mouth(level, articulation);
        esp_err_t err = esp_codec_dev_write(s_speaker, chunk->pcm + offset,
                                            count * sizeof(int16_t));
        release_speech_mouth();
        if (err != ESP_OK) return err;
        note_reply_write();
    }
    return ESP_OK;
}

/* While silence plays, the mouth closes along its release. */
static void decay_speech_mouth(void)
{
    uint8_t articulation = 0;
    uint8_t level = pet_speech_mouth_envelope_decay(&s_mouth_envelope, &articulation);
    queue_speech_mouth(level, articulation);
}

/* Called with s_codec_lock held: the mouth closes now and nothing queued
 * reaches it later. */
static void clear_speech_mouth(void)
{
    pet_speech_mouth_delay_reset(&s_mouth_delay);
    pet_face_set_audio_level(0);
    pet_face_set_speech_articulation(0);
    pet_speech_mouth_envelope_reset(&s_mouth_envelope, s_playback_mouth_mode,
                                    s_playback_sample_rate);
}

/* Called with s_codec_lock held: the speaker speech opened closes, with any
 * tail. */
static void close_speech_speaker_locked(void)
{
    if (s_speaker_open) esp_codec_dev_close(s_speaker);
    s_speaker_open = false;
    s_tail = PLAYBACK_TAIL_NONE;
}

/* Called with s_codec_lock held after the final marker: the speaker stays
 * open while the reply drains and for the tail after it. */
static void begin_playback_tail_locked(uint32_t stream_id)
{
    int64_t now = esp_timer_get_time();
    int64_t drained_at = now + (int64_t)playback_drain_ms(s_playback_sample_rate) * 1000;
    /* The mouth shows the last slices after its delay; the reply ends after them. */
    int64_t last_mouth_us;
    if (pet_speech_mouth_delay_last_due(&s_mouth_delay, &last_mouth_us) &&
        last_mouth_us + PLAYBACK_SLICE_MS * 1000 > drained_at) {
        drained_at = last_mouth_us + PLAYBACK_SLICE_MS * 1000;
    }
    int64_t close_at = now + (int64_t)PLAYBACK_TAIL_MS * 1000;
    log_playback_stats("complete", stream_id);
    s_tail_stream = stream_id;
    s_tail_drained_at_us = drained_at;
    s_tail_close_at_us = close_at > drained_at ? close_at : drained_at;
    s_tail = PLAYBACK_TAIL_DRAINING;
}

/* Called with s_codec_lock held by anything that needs the codec for itself:
 * the tail yields and its speaker closes. A reply still draining is complete
 * from here, unless a stop is already on its way; returns true when the
 * caller must report it. */
static bool end_playback_tail_locked(const char *reason, uint32_t *stream_id)
{
    if (s_tail == PLAYBACK_TAIL_NONE) return false;
    bool report = s_tail == PLAYBACK_TAIL_DRAINING && s_playing;
    *stream_id = s_tail_stream;
    ESP_LOGI(TAG, "playback tail stream=%lu yields to %s", (unsigned long)s_tail_stream, reason);
    close_speech_speaker_locked();
    if (report) {
        s_playing = false;
        clear_speech_mouth();
        pet_face_set_state(PET_FACE_IDLE);
    }
    return report;
}

/* One step of a finished reply's tail: a block of silence keeps the DMA fed
 * (the silence bridge's anti-crackle rule), the reply is reported done once it
 * has drained, and the speaker closes when the tail ends. The lock is held
 * for one block only. */
static void service_playback_tail(int16_t *silence)
{
    bool drained = false;
    uint32_t stream_id = 0;
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    if (s_tail == PLAYBACK_TAIL_NONE) {
        /* Something else ended it while this task waited. */
    } else if (s_tail == PLAYBACK_TAIL_DRAINING && !s_playing) {
        /* A stop is on its way to the lock; it wins, and nothing is reported. */
        close_speech_speaker_locked();
    } else if (s_tail == PLAYBACK_TAIL_HOLDING && (s_capture_requested || s_speech_waiting)) {
        ESP_LOGI(TAG, "playback tail stream=%lu yields to %s", (unsigned long)s_tail_stream,
                 s_capture_requested ? "microphone" : "speech");
        close_speech_speaker_locked();
    } else {
        /* While the reply drains, the mouth closes along with its silence. */
        if (s_tail == PLAYBACK_TAIL_DRAINING && esp_timer_get_time() < s_tail_drained_at_us) {
            decay_speech_mouth();
        }
        size_t samples = s_playback_sample_rate / 50u;
        if (!samples) samples = 1;
        if (samples > PLAYBACK_SILENCE_MAX_SAMPLES) samples = PLAYBACK_SILENCE_MAX_SAMPLES;
        bool written = esp_codec_dev_write(s_speaker, silence, samples * sizeof(int16_t)) == ESP_OK;
        if (!written) ESP_LOGE(TAG, "speaker silence tail failed");
        int64_t now = esp_timer_get_time();
        if (s_tail == PLAYBACK_TAIL_DRAINING) {
            if (written && now < s_tail_drained_at_us) {
                release_speech_mouth();
            } else {
                s_tail = PLAYBACK_TAIL_HOLDING;
                s_playing = false;
                clear_speech_mouth();
                pet_face_set_state(PET_FACE_IDLE);
                drained = true;
                stream_id = s_tail_stream;
            }
        }
        if (s_tail == PLAYBACK_TAIL_HOLDING && (!written || now >= s_tail_close_at_us)) {
            ESP_LOGI(TAG, "playback tail stream=%lu closed", (unsigned long)s_tail_stream);
            close_speech_speaker_locked();
        }
    }
    xSemaphoreGive(s_codec_lock);
    if (drained && s_playback_done_callback) s_playback_done_callback(stream_id);
}

static void capture_task(void *arg)
{
    (void)arg;
    int16_t raw[CAPTURE_SAMPLES_PER_CHUNK * CAPTURE_SLOTS];
    int16_t mono[CAPTURE_SAMPLES_PER_CHUNK];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t stream_id = s_capture_stream;
        uint32_t sequence = 0;
        uint16_t timeout_seconds = s_capture_timeout_seconds;
        int64_t started = esp_timer_get_time();
        bool first_chunk = true;
        /* The wire carries PCM16. Capture at that precision too: 32-bit stereo
         * doubles both duplex DMA rings and can exhaust internal RAM at Listen. */
        esp_codec_dev_sample_info_t format = { .bits_per_sample = 16, .channel = 2, .sample_rate = 16000 };
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
        /* The duplex I2S cannot run the speaker and the microphone at their
         * different formats: a reply's tail yields here. The done callback
         * only posts an event, so it may run under the lock. */
        uint32_t finished_stream = 0;
        if (end_playback_tail_locked("microphone", &finished_stream) && s_playback_done_callback) {
            s_playback_done_callback(finished_stream);
        }
        esp_err_t result = esp_codec_dev_set_in_gain(s_microphone, 30.0f);
        if (result == ESP_OK) result = esp_codec_dev_open(s_microphone, &format);
        if (result != ESP_OK) {
            s_capture_requested = false;
            first_chunk = false;
        }
        /* A stop can arrive before this task gets its first timeslice. Still
         * read one frame so input.audio.end never advertises sequence zero
         * without a matching PCM frame. Cancellation paths discard it. */
        while ((s_capture_requested || first_chunk) && s_capture_stream == stream_id) {
            first_chunk = false;
            result = esp_codec_dev_read(s_microphone, raw, sizeof(raw));
            if (result != ESP_OK) break;
            for (size_t i = 0; i < CAPTURE_SAMPLES_PER_CHUNK; ++i) {
                int32_t mixed = (int32_t)raw[i * CAPTURE_SLOTS] + raw[i * CAPTURE_SLOTS + 1];
                mixed /= 2;
                if (mixed > INT16_MAX) mixed = INT16_MAX;
                if (mixed < INT16_MIN) mixed = INT16_MIN;
                mono[i] = (int16_t)mixed;
            }
            if (s_chunk_callback && s_chunk_callback(stream_id, sequence, mono, CAPTURE_SAMPLES_PER_CHUNK) != ESP_OK) {
                result = ESP_ERR_NO_MEM;
                break;
            }
            sequence++;
            if (esp_timer_get_time() - started >= (int64_t)timeout_seconds * 1000LL * 1000) {
                s_capture_requested = false;
                break;
            }
        }
        esp_codec_dev_close(s_microphone);
        xSemaphoreGive(s_codec_lock);
        s_capture_requested = false;
        uint32_t duration_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
        if (s_done_callback) s_done_callback(stream_id, sequence ? sequence - 1 : 0, duration_ms, result);
        s_capture_active = false;
    }
}

/* One silent 20 ms block inside a reply, the silence bridge: it keeps the I2S
 * DMA fed while speech is late or the queue refills, so the ring never runs
 * dry (dma_underflows) and the mouth closes along its release. Counted in the
 * reply's underruns. */
static void bridge_silence(int16_t *silence)
{
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    if (s_playing) {
        decay_speech_mouth();
        if (s_playback_primed || s_playback_rebuffering) {
            s_playback_underruns++;
            if (s_playback_underruns <= 3u) {
                ESP_LOGW(TAG, "playback underrun stream=%lu count=%lu", (unsigned long)s_playback_stream,
                         (unsigned long)s_playback_underruns);
            }
            size_t samples = s_playback_sample_rate / 50u;
            if (samples > PLAYBACK_SILENCE_MAX_SAMPLES) samples = PLAYBACK_SILENCE_MAX_SAMPLES;
            if (esp_codec_dev_write(s_speaker, silence, samples * sizeof(int16_t)) != ESP_OK) {
                ESP_LOGE(TAG, "speaker silence bridge failed");
            } else {
                note_reply_write();
            }
        }
        release_speech_mouth();
    }
    xSemaphoreGive(s_codec_lock);
}

static void playback_task(void *arg)
{
    (void)arg;
    playback_chunk_t *chunk = NULL;
    int16_t silence[PLAYBACK_SILENCE_MAX_SAMPLES] = {0};
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(s_tail != PLAYBACK_TAIL_NONE ?
                                        PLAYBACK_TAIL_WAIT_MS : PLAYBACK_QUEUE_WAIT_MS);
        if (xQueueReceive(s_playback_queue, &chunk, wait) != pdTRUE || !chunk) {
            if (s_tail != PLAYBACK_TAIL_NONE) {
                service_playback_tail(silence);
            } else if (s_playing) {
                /* Once a reply has rebuffered its link is known to be slow:
                 * a later gap pauses at once instead of after one late frame. */
                bool repeated = s_playback_rebuffers > 0 ||
                                s_frames_since_gap < PLAYBACK_REBUFFER_WINDOW_FRAMES;
                s_frames_since_gap = 0;
                if (s_playback_primed && !s_playback_finishing && repeated) {
                    /* Speech is late: pause until a deeper reserve waits. */
                    s_playback_primed = false;
                    s_playback_rebuffering = true;
                    unsigned reserve = PLAYBACK_REBUFFER_FIRST_CHUNKS +
                                       PLAYBACK_REBUFFER_STEP_CHUNKS * s_playback_rebuffers;
                    s_rebuffer_chunks = reserve < PLAYBACK_REBUFFER_MAX_CHUNKS ? reserve
                                                                               : PLAYBACK_REBUFFER_MAX_CHUNKS;
                    s_playback_rebuffers++;
                }
                bridge_silence(silence);
            }
            continue;
        }
        if (!chunk->final && !s_playback_primed &&
            chunk->stream_id == s_playback_stream && s_playing) {
            /* The gateway intentionally paces 40 ms PCM frames in real time.
             * Starting on its first frame leaves no reserve for early network
             * jitter and produced a repeatable audible pause about one second
             * into replies. Hold the first frame while five more arrive, then
             * begin with a bounded ~240 ms reserve. A short completed reply or
             * cancellation releases the hold immediately. */
            bool rebuffer = s_playback_rebuffering;
            unsigned target = rebuffer ? s_rebuffer_chunks : PLAYBACK_PRIME_CHUNKS;
            int64_t prime_started_us = esp_timer_get_time();
            while (s_playing && chunk->stream_id == s_playback_stream &&
                   !s_playback_finishing &&
                   uxQueueMessagesWaiting(s_playback_queue) + 1u < target) {
                /* Inside a reply the wait plays silence, which also paces it. */
                if (rebuffer) bridge_silence(silence);
                else vTaskDelay(pdMS_TO_TICKS(PLAYBACK_PRIME_POLL_MS));
            }
            if (s_playing && chunk->stream_id == s_playback_stream) {
                /* A reply's first gap is isolated until proven otherwise. */
                if (!rebuffer) s_frames_since_gap = PLAYBACK_REBUFFER_WINDOW_FRAMES;
                s_playback_primed = true;
                s_playback_rebuffering = false;
                ESP_LOGI(TAG,
                         "playback %s stream=%lu chunks=%u waitMs=%lld",
                         rebuffer ? "rebuffered" : "primed", (unsigned long)chunk->stream_id,
                         (unsigned)(uxQueueMessagesWaiting(s_playback_queue) + 1u),
                         (long long)((esp_timer_get_time() - prime_started_us) / 1000));
            }
        }
        bool playback_done = false;
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
        /* Frames after the final marker belong to no reply and are dropped. */
        if (chunk->stream_id == s_playback_stream && s_playing && s_tail == PLAYBACK_TAIL_NONE) {
            if (chunk->final) {
                /* Not closed here: that cut the end of every reply still in
                 * the DMA and the codec. The tail reports it once drained. */
                begin_playback_tail_locked(chunk->stream_id);
            } else {
                if (s_frames_since_gap < PLAYBACK_REBUFFER_WINDOW_FRAMES) s_frames_since_gap++;
                if (write_speech_chunk(chunk) != ESP_OK) {
                    ESP_LOGE(TAG, "speaker write failed");
                    close_speech_speaker_locked();
                    s_playing = false;
                    log_playback_stats("write-error", chunk->stream_id);
                    clear_speech_mouth();
                    pet_face_set_state(PET_FACE_IDLE);
                    playback_done = true;
                }
            }
        }
        xSemaphoreGive(s_codec_lock);
        uint32_t completed_stream = chunk->stream_id;
        free(chunk);
        if (playback_done && s_playback_done_callback) s_playback_done_callback(completed_stream);
    }
}

esp_err_t pet_audio_init(pet_audio_capture_chunk_cb_t chunk_callback,
                         pet_audio_capture_done_cb_t done_callback,
                         pet_audio_playback_done_cb_t playback_done_callback)
{
    s_chunk_callback = chunk_callback;
    s_done_callback = done_callback;
    s_playback_done_callback = playback_done_callback;
    if (pet_audio_board_init(&s_speaker, &s_microphone) != ESP_OK) return ESP_FAIL;
    s_playback_queue = xQueueCreate(PLAYBACK_QUEUE_DEPTH, sizeof(playback_chunk_t *));
    if (!s_playback_queue) return ESP_ERR_NO_MEM;
    s_codec_lock = xSemaphoreCreateMutex();
    if (!s_codec_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(capture_task, "pet_capture", 6144, NULL, CAPTURE_TASK_PRIORITY, &s_capture_task,
                                CAPTURE_TASK_CORE) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(playback_task, "pet_playback", 5120, NULL, PLAYBACK_TASK_PRIORITY, &s_playback_task,
                                PLAYBACK_TASK_CORE) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t pet_audio_capture_start(uint32_t stream_id)
{
    /* active stays true through codec close and done-callback dispatch. This
     * prevents a new request from being overwritten by the prior task's
     * unconditional cleanup. A playing cue stops within one 20 ms chunk once
     * the request is visible, so it never refuses the microphone. */
    if (s_capture_active || s_playing) return ESP_ERR_INVALID_STATE;
    s_capture_stream = stream_id;
    s_capture_requested = true;
    s_capture_active = true;
    xTaskNotifyGive(s_capture_task);
    return ESP_OK;
}

void pet_audio_capture_stop(void) { s_capture_requested = false; }
bool pet_audio_is_capturing(void) { return s_capture_active; }

esp_err_t pet_audio_set_capture_timeout(uint16_t seconds)
{
    if (seconds < 1 || seconds > 600) return ESP_ERR_INVALID_ARG;
    s_capture_timeout_seconds = seconds;
    return ESP_OK;
}

uint16_t pet_audio_get_capture_timeout(void) { return s_capture_timeout_seconds; }

esp_err_t pet_audio_playback_start(uint32_t stream_id, uint32_t sample_rate,
                                   pet_realtime_boost_t boost,
                                   pet_speech_mouth_mode_t mouth_mode)
{
    if (boost < PET_REALTIME_BOOST_OFF || boost >= PET_REALTIME_BOOST_COUNT) return ESP_ERR_INVALID_ARG;
    if (!pet_speech_mouth_mode_valid(mouth_mode)) return ESP_ERR_INVALID_ARG;
    /* Also ends the previous reply's tail: an open codec ignores a new format. */
    pet_audio_playback_cancel();
    esp_codec_dev_sample_info_t format = { .bits_per_sample = 16, .channel = 1, .sample_rate = sample_rate };
    s_speech_waiting = true;
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    s_speech_waiting = false;
    close_speech_speaker_locked();
    esp_err_t err = esp_codec_dev_open(s_speaker, &format);
    if (err == ESP_OK) {
        s_speaker_open = true;
        esp_codec_dev_set_out_vol(s_speaker, s_output_volume);
        s_playback_stream = stream_id;
        s_playback_boost = boost;
        s_playback_mouth_mode = pet_speech_mouth_full_frame_normalize(mouth_mode);
        s_playback_sample_rate = sample_rate;
        pet_speech_mouth_envelope_reset(&s_mouth_envelope,
                                        s_playback_mouth_mode, sample_rate);
        pet_speech_mouth_delay_reset(&s_mouth_delay);
        s_mouth_level_min = 255;
        s_mouth_level_max = 0;
        s_mouth_level_total = 0;
        s_mouth_level_updates = 0;
        s_playback_underruns = 0;
        s_playback_rebuffers = 0;
        s_playback_rebuffering = false;
        s_dma_underflow_counting = false;
        s_feed_gap_max_us = 0;
        s_playback_primed = false;
        s_playback_finishing = false;
        pet_face_begin_speech_stream(s_playback_mouth_mode);
        pet_pcm_gain_stats_reset(&s_playback_stats);
        s_playback_stats_active = true;
        s_playing = true;
    }
    xSemaphoreGive(s_codec_lock);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

esp_err_t pet_audio_playback_enqueue(uint32_t stream_id, uint32_t sequence,
                                     const uint8_t *pcm, size_t length)
{
    if (!s_playing || stream_id != s_playback_stream) return ESP_ERR_INVALID_STATE;
    if (!pcm || !length || length > PLAYBACK_MAX_CHUNK || (length & 1)) return ESP_ERR_INVALID_ARG;
    playback_chunk_t *chunk = malloc(sizeof(*chunk));
    if (!chunk) return ESP_ERR_NO_MEM;
    chunk->stream_id = stream_id;
    chunk->sequence = sequence;
    chunk->length = length;
    chunk->final = false;
    memcpy(chunk->pcm, pcm, length);
    if (xQueueSend(s_playback_queue, &chunk, 0) != pdTRUE) {
        free(chunk);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pet_audio_playback_finish(uint32_t stream_id)
{
    if (!s_playing || stream_id != s_playback_stream) return;
    s_playback_finishing = true;
    playback_chunk_t *chunk = calloc(1, sizeof(*chunk));
    if (!chunk) {
        ESP_LOGE(TAG, "could not allocate playback completion marker");
        pet_audio_playback_cancel();
        if (s_playback_done_callback) s_playback_done_callback(stream_id);
        return;
    }
    chunk->stream_id = stream_id;
    chunk->final = true;
    if (xQueueSend(s_playback_queue, &chunk, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "playback queue did not accept completion marker");
        free(chunk);
        pet_audio_playback_cancel();
        if (s_playback_done_callback) s_playback_done_callback(stream_id);
    }
}

void pet_audio_playback_cancel(void)
{
    /* Cleared before the lock: the playback task then neither writes another
     * frame nor reports a draining reply, whichever of us takes the lock. */
    bool close_speaker = s_playing;
    s_playing = false;
    s_playback_finishing = true;
    s_playback_primed = false;
    s_playback_rebuffering = false;
    /* A reply's tail also holds the speaker after s_playing is false. Only
     * then is the lock taken, so a stop never waits for a cue. */
    if ((close_speaker || s_tail != PLAYBACK_TAIL_NONE) && s_speaker && s_codec_lock) {
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
        close_speech_speaker_locked();
        log_playback_stats("cancelled", s_playback_stream);
        clear_speech_mouth();
        xSemaphoreGive(s_codec_lock);
    } else {
        pet_face_set_audio_level(0);
        pet_face_set_speech_articulation(0);
    }
    if (s_playback_queue) {
        playback_chunk_t *chunk;
        while (xQueueReceive(s_playback_queue, &chunk, 0) == pdTRUE) free(chunk);
    }
}

bool pet_audio_is_playing(void) { return s_playing; }

bool pet_audio_playback_queue_healthy(void)
{
    if (!s_playing || !s_playback_queue) return true;
    return uxQueueSpacesAvailable(s_playback_queue) > 1u;
}

esp_err_t pet_audio_play_local(const uint8_t *pcm, size_t length,
                               bool (*keep_playing)(void *context), void *context)
{
    if (!pcm || !length || (length & 1)) return ESP_ERR_INVALID_ARG;
    if (s_capture_requested || s_playing || s_speech_waiting || !s_codec_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    if (s_capture_requested || s_playing || s_speech_waiting) {
        xSemaphoreGive(s_codec_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* A reply's tail only holds the speaker on silence now (s_playing is
     * false, so there is nothing to report): the cue takes it. Closing first
     * lets the cue set its own format. */
    uint32_t tail_stream;
    (void)end_playback_tail_locked("a cue", &tail_stream);

    s_local_playing = true;
    esp_codec_dev_sample_info_t format = { .bits_per_sample = 16, .channel = 1, .sample_rate = 24000 };
    esp_err_t err = esp_codec_dev_open(s_speaker, &format);
    uint8_t effect_volume = (uint8_t)((s_output_volume * 70U + 50U) / 100U);
    if (err == ESP_OK) err = esp_codec_dev_set_out_vol(s_speaker, effect_volume);
    for (size_t offset = 0; err == ESP_OK && offset < length; offset += LOCAL_CUE_CHUNK_BYTES) {
        /* The microphone and speech always win; so does an explicit stop. */
        if (s_capture_requested || s_speech_waiting || (keep_playing && !keep_playing(context))) {
            err = ESP_ERR_NOT_FINISHED;
            break;
        }
        size_t chunk = length - offset;
        if (chunk > LOCAL_CUE_CHUNK_BYTES) chunk = LOCAL_CUE_CHUNK_BYTES;
        err = esp_codec_dev_write(s_speaker, (void *)(pcm + offset), chunk);
    }
    esp_codec_dev_close(s_speaker);
    s_local_playing = false;
    xSemaphoreGive(s_codec_lock);
    return err;
}

void pet_audio_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_output_volume = volume;
    if ((s_playing || s_local_playing) && s_codec_lock && s_speaker) {
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
        esp_codec_dev_set_out_vol(s_speaker, volume);
        xSemaphoreGive(s_codec_lock);
    }
}

uint8_t pet_audio_get_volume(void) { return s_output_volume; }

void pet_audio_set_speech_mouth_offset(int16_t offset_ms)
{
    s_mouth_offset_ms = pet_speech_mouth_offset_clamp(offset_ms);
}

int16_t pet_audio_get_speech_mouth_offset(void) { return s_mouth_offset_ms; }
