#include "pet_sfx.h"

#include <stddef.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_audio.h"
#include "pet_sfx_plan.h"

#define DECLARE_ASSET(name) \
    extern const uint8_t name##_start[] asm("_binary_" #name "_pcm_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_pcm_end")

DECLARE_ASSET(wake);
DECLARE_ASSET(connect);
DECLARE_ASSET(start);
DECLARE_ASSET(stop);
DECLARE_ASSET(complete);
DECLARE_ASSET(cancel);
DECLARE_ASSET(retry);
DECLARE_ASSET(error);
DECLARE_ASSET(no_voice);
DECLARE_ASSET(settings_open);
DECLARE_ASSET(settings_close);
DECLARE_ASSET(face_swipe_0);
DECLARE_ASSET(face_swipe_1);
DECLARE_ASSET(face_swipe_2);
DECLARE_ASSET(face_swipe_3);
DECLARE_ASSET(face_swipe_4);
DECLARE_ASSET(gesture_shake);
DECLARE_ASSET(gesture_pop);

/* Console waits use tokens with the top bit set; listening tokens never do. */
#define WAIT_TOKEN_FLAG 0x80000000u

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} embedded_audio_t;

static const char *TAG = "pet_sfx";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wait_lock;
static SemaphoreHandle_t s_wait_done;
static TaskHandle_t s_task;
static pet_sfx_listen_done_t s_listen_done;
/* Guarded by s_lock. */
static pet_sfx_plan_t s_plan;
static bool s_playing_decoration;
static uint32_t s_wait_token;
static uint32_t s_wait_serial;
static pet_sfx_outcome_t s_wait_outcome;
/* Changes whenever the playing cue must stop; read between 20 ms chunks. */
static volatile uint32_t s_generation;
static volatile bool s_effects_enabled = true;

static embedded_audio_t asset_for(pet_sfx_t effect)
{
    switch (effect) {
        case PET_SFX_WAKE: return (embedded_audio_t){ wake_start, wake_end };
        case PET_SFX_CONNECT: return (embedded_audio_t){ connect_start, connect_end };
        case PET_SFX_LISTEN: return (embedded_audio_t){ start_start, start_end };
        case PET_SFX_SUBMIT: return (embedded_audio_t){ stop_start, stop_end };
        case PET_SFX_DEPLOY_COMPLETE: return (embedded_audio_t){ complete_start, complete_end };
        case PET_SFX_CANCEL: return (embedded_audio_t){ cancel_start, cancel_end };
        case PET_SFX_RETRY: return (embedded_audio_t){ retry_start, retry_end };
        case PET_SFX_ERROR: return (embedded_audio_t){ error_start, error_end };
        case PET_SFX_NO_VOICE: return (embedded_audio_t){ no_voice_start, no_voice_end };
        case PET_SFX_SETTINGS_OPEN: return (embedded_audio_t){ settings_open_start, settings_open_end };
        case PET_SFX_SETTINGS_CLOSE: return (embedded_audio_t){ settings_close_start, settings_close_end };
        case PET_SFX_FACE_SWIPE_0: return (embedded_audio_t){ face_swipe_0_start, face_swipe_0_end };
        case PET_SFX_FACE_SWIPE_1: return (embedded_audio_t){ face_swipe_1_start, face_swipe_1_end };
        case PET_SFX_FACE_SWIPE_2: return (embedded_audio_t){ face_swipe_2_start, face_swipe_2_end };
        case PET_SFX_FACE_SWIPE_3: return (embedded_audio_t){ face_swipe_3_start, face_swipe_3_end };
        case PET_SFX_FACE_SWIPE_4: return (embedded_audio_t){ face_swipe_4_start, face_swipe_4_end };
        case PET_SFX_GESTURE_SHAKE: return (embedded_audio_t){ gesture_shake_start, gesture_shake_end };
        case PET_SFX_GESTURE_POP: return (embedded_audio_t){ gesture_pop_start, gesture_pop_end };
        default: return (embedded_audio_t){ NULL, NULL };
    }
}

static bool is_conversation(pet_sfx_t effect)
{
    switch (effect) {
        case PET_SFX_LISTEN:
        case PET_SFX_SUBMIT:
        case PET_SFX_CANCEL:
        case PET_SFX_RETRY:
        case PET_SFX_NO_VOICE:
        case PET_SFX_ERROR:
            return true;
        default:
            return false;
    }
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void report(const pet_sfx_plan_item_t *item, pet_sfx_outcome_t outcome)
{
    if (!item->token) return;
    if (item->token & WAIT_TOKEN_FLAG) {
        bool waiting = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (item->token == s_wait_token) {
            s_wait_token = 0;
            s_wait_outcome = outcome;
            waiting = true;
        }
        xSemaphoreGive(s_lock);
        if (waiting) xSemaphoreGive(s_wait_done);
    } else if (s_listen_done) {
        s_listen_done(item->token, outcome);
    }
}

static bool keep_playing(void *context)
{
    return s_generation == *(const uint32_t *)context;
}

static void cue_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            pet_sfx_plan_item_t item;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool next = pet_sfx_plan_next(&s_plan, now_ms(), &item);
            uint32_t generation = s_generation;
            s_playing_decoration = next && !item.conversation;
            xSemaphoreGive(s_lock);
            if (!next) break;

            pet_sfx_outcome_t outcome = PET_SFX_SKIPPED;
            embedded_audio_t asset = asset_for((pet_sfx_t)item.cue);
            if ((item.conversation || s_effects_enabled) && asset.start && asset.end > asset.start) {
                esp_err_t err = pet_audio_play_local(asset.start, (size_t)(asset.end - asset.start),
                                                     keep_playing, &generation);
                if (err == ESP_OK) outcome = PET_SFX_PLAYED;
                else if (s_generation != generation) outcome = PET_SFX_CANCELLED;
                else if (err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FINISHED)
                    ESP_LOGW(TAG, "sound cue %u failed: %s", item.cue, esp_err_to_name(err));
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_playing_decoration = false;
            xSemaphoreGive(s_lock);
            report(&item, outcome);
        }
    }
}

static esp_err_t request(pet_sfx_t effect, uint32_t token)
{
    if ((unsigned)effect >= PET_SFX_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_task) return ESP_ERR_INVALID_STATE;
    pet_sfx_plan_item_t item = {
        .cue = (uint8_t)effect,
        .conversation = is_conversation(effect),
        .token = token,
        .requested_ms = now_ms(),
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pet_sfx_plan_effect_t result = pet_sfx_plan_request(&s_plan, item, s_playing_decoration);
    if (result.stop_playing) s_generation++;
    xSemaphoreGive(s_lock);
    if (result.superseded) report(&result.superseded_item, PET_SFX_SKIPPED);
    if (result.refused) return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t pet_sfx_init(pet_sfx_listen_done_t listen_done)
{
    if (s_task) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_wait_lock = xSemaphoreCreateMutex();
    s_wait_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_wait_lock || !s_wait_done) return ESP_ERR_NO_MEM;
    s_listen_done = listen_done;
    pet_sfx_plan_reset(&s_plan);
    /* Below capture (7) and speech playback (6): cues never delay a voice turn. */
    if (xTaskCreate(cue_task, "pet_sfx", 4096, NULL, 4, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t pet_sfx_play(pet_sfx_t effect)
{
    return request(effect, 0);
}

esp_err_t pet_sfx_play_listen(uint32_t token)
{
    if (!token || (token & WAIT_TOKEN_FLAG)) return ESP_ERR_INVALID_ARG;
    return request(PET_SFX_LISTEN, token);
}

esp_err_t pet_sfx_play_and_wait(pet_sfx_t effect, uint32_t timeout_ms)
{
    if (!s_wait_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_wait_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wait_serial = (s_wait_serial + 1u) & ~WAIT_TOKEN_FLAG;
    uint32_t token = WAIT_TOKEN_FLAG | s_wait_serial;
    s_wait_token = token;
    xSemaphoreGive(s_lock);
    xSemaphoreTake(s_wait_done, 0);
    esp_err_t err = request(effect, token);
    if (err == ESP_OK) {
        err = xSemaphoreTake(s_wait_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE ? ESP_ERR_TIMEOUT :
              s_wait_outcome == PET_SFX_PLAYED ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wait_token = 0;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wait_lock);
    return err;
}

void pet_sfx_cancel(void)
{
    if (!s_lock) return;
    pet_sfx_plan_item_t cancelled;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool waiting = pet_sfx_plan_cancel(&s_plan, &cancelled);
    s_generation++;
    xSemaphoreGive(s_lock);
    if (waiting) report(&cancelled, PET_SFX_CANCELLED);
}

void pet_sfx_set_effects_enabled(bool enabled) { s_effects_enabled = enabled; }
bool pet_sfx_effects_enabled(void) { return s_effects_enabled; }

pet_sfx_t pet_sfx_for_face_swipe(uint8_t index)
{
    static const pet_sfx_t sequence[] = {
        PET_SFX_FACE_SWIPE_0,
        PET_SFX_FACE_SWIPE_1,
        PET_SFX_FACE_SWIPE_2,
        PET_SFX_FACE_SWIPE_3,
        PET_SFX_FACE_SWIPE_4,
    };
    return sequence[index % (sizeof(sequence) / sizeof(sequence[0]))];
}
