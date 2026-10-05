#include "pet_vnext.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "pet_display.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "frame_player.h"
#include "pet_audio.h"
#include "pet_battery.h"
#include "pet_diagnostics.h"
#include "pet_face.h"
#include "pet_face_pack.h"
#include "pet_face_id.h"
#include "pet_motion.h"
#include "pet_network.h"
#include "pet_onboarding.h"
#include "pet_setup.h"
#include "pet_sync.h"
#include "pet_runtime_gate.h"
#include "pet_firmware_control.h"
#include "pet_firmware_receipt_nvs.h"
#include "pet_single_store.h"
#include "pet_replace_control.h"
#include "pet_release_v2_pack.h"
#include "pet_release_v2_protection.h"
#include "pet_tap_policy.h"
#include "pet_session_wire.h"
#include "cJSON.h"
#if CONFIG_PET_POCKET_TERMINAL
#include "pet_sfx.h"
#include "face_core.h"
#include "pet_slot_store.h"
#include "pet_slot_protection.h"
#endif

typedef enum
{
    CONTROL_LIBRARY,
    CONTROL_SELECT,
    CONTROL_RETURN,
    CONTROL_RETRY,
    CONTROL_RESTART,
    CONTROL_OTA,
    CONTROL_SWITCH,
    CONTROL_KEEP_VOLUME,
    CONTROL_KEEP_BRIGHTNESS
} control_kind_t;
typedef struct
{
    control_kind_t kind;
    bool next;
    uint8_t value;
    char build_id[37];
    pet_ota_request_t *ota;
} control_command_t;
typedef enum
{
    AUDIO_TAP,
    AUDIO_CAPTURE_DONE,
    AUDIO_PLAYBACK_DONE,
    AUDIO_CONFIG,
    AUDIO_SPEECH,
    AUDIO_ERROR,
    AUDIO_GESTURE,
    AUDIO_STATE,
    AUDIO_LISTEN_CUE_DONE,
    AUDIO_TAP_RETRY,
    AUDIO_PET,
    AUDIO_VOLUME
} audio_kind_t;
typedef struct
{
    audio_kind_t kind;
    uint32_t stream, sequence, duration, generation;
    esp_err_t result;
    pet_synced_config_v2_t config;
    pet_speech_preference_t speech;
    /* AUDIO_ERROR: the reason typed on the face, or NULL. */
    const char *line;
} audio_event_t;
typedef struct
{
    const void *bytes;
    size_t length;
    pet_pack_verified_manifest_t manifest;
    pet_face_gender_t gender;
} prepared_t;
static QueueHandle_t s_commands, s_audio_events;
static SemaphoreHandle_t s_audio_lock;
static pet_asset_store_t s_store;
static pet_sync_t *s_sync;
static pet_control_library_t s_library;
static pet_config_t s_wifi;
static pet_control_context_t s_applied;
static pet_control_context_t s_brain_context;
static char s_brain_token[PET_BRAIN_TOKEN_MAX];
static atomic_uint s_brain_expires_ms;
static uint64_t s_brain_retry_ms, s_management_poll_ms;
static atomic_bool s_brain_reconnect;
static atomic_bool s_brain_disconnected;
static prepared_t s_prepared[2];
static int s_active_slot = -1;
#if CONFIG_PET_POCKET_TERMINAL
static const char *TAG = "pet_pocket";
/* The three installed pets and the slot on screen (-1: none). */
static pet_slot_store_t s_slots;
static int s_shown_slot = -1;
/* The store failed to reopen after a journal write failed; retried every 15 s. */
static bool s_store_lost;
static char s_shown_name[17];
/* Why the pet on screen cannot talk, for a tap that asks (publish_status). */
typedef enum
{
    VOICE_BLOCK_CONNECTING,
    VOICE_BLOCK_NOT_PAIRED,
    VOICE_BLOCK_NOT_LINKED
} voice_block_t;
static atomic_int s_voice_block;
static int64_t s_no_voice_cue_at = -1500000;
/* Voice held for the pet on screen while its session stays open: set when
 * another pet comes on screen (a swipe) or a selection is about to move the
 * session, cleared when the pet on screen is admitted. A late answer for the
 * previous binding never enables it. */
static atomic_bool s_voice_paused;
/* A rebind (session-rebind-v1) was sent and waits for its answer. */
static atomic_bool s_rebinding;
/* The waiting tap was made while the session connects or switches pets
 * (TAP_CONNECTING_US, TAP_SWITCH_WAIT_US), not held for a control step.
 * Guarded by s_audio_lock. */
static bool s_tap_connecting;
static bool pocket_activate(void *unused, const pet_control_context_t *cloud);
static bool pocket_unlinked(unsigned slot);
/* A tap while the voice session connects waits this long. */
#define TAP_CONNECTING_US 5000000
/* A tap while the open session moves to the pet on screen waits for the whole
 * switch: the swipe settling, the cloud's selection and the rebind. */
#define TAP_SWITCH_WAIT_US 15000000
/* A rebind's answer is due well before a new socket's hello would be. */
#define REBIND_TIMEOUT_US 8000000
/* The swipe the control task has not taken yet: 1 next, -1 previous, 0 none.
 * A swipe made meanwhile replaces it, so swipes made while control is busy
 * move one pet once it is free, never one pet per swipe. */
static atomic_int s_swipe;
/* When the control task's current pass began (ms, never 0); 0 while it waits
 * for a command. A cloud step can hold it for seconds on a slow link. */
static atomic_uint s_control_busy_since;
/* A swipe after control has been busy this long says it can't switch yet. */
#define CONTROL_BUSY_NOTICE_MS 1000u
/* The levels the owner hears and sees now (the menu's volume and brightness):
 * applied at every activation and reported as they are, before they are kept. */
static atomic_uint s_volume_now, s_brightness_now;
#endif
static atomic_bool s_face_started, s_audio_started;
static bool s_face_attempted, s_audio_attempted;
static atomic_bool s_voice_allowed, s_revalidate, s_turn_active, s_runtime_fault;
/* The session is checked again from scratch: control re-admits the pet or
 * reconnects. The first reason is logged, since a silent "idle -> offline"
 * left pet switches that reconnected unexplained (1 Oct). */
static void revalidate(const char *why)
{
    (void)why; /* Host builds compile the log away. */
    if (!atomic_exchange(&s_revalidate, true))
        ESP_LOGW("pet_vnext", "revalidate: %s", why);
}
static atomic_bool s_control_healthy;
static atomic_bool s_gateway_connected, s_identity_ready;
static atomic_bool s_handshake_pending;
static int64_t s_handshake_deadline;
static bool s_ota_reserved;
static atomic_uint s_input_stream, s_output_stream, s_generation;
/* An input stream the gateway refused: its remaining mic frames are dropped. */
static atomic_uint s_input_refused;
static uint32_t s_output_sequence;
static uint32_t s_next_input_stream;
static pet_runtime_gate_t s_resources;
static atomic_bool s_battery_valid, s_battery_charging;
static atomic_uint s_battery_percent;
static bool s_battery_started;
static char s_id[37], s_credential[44];
static char s_local_error[33];
static const char *s_origin;
static pet_pack_trust_key_t s_trust;
static size_t s_trust_count;
static bool s_fw_lifecycle, s_lifecycle_selected;
static atomic_bool s_fw_reserved;
static pet_single_store_t s_single;
static pet_firmware_receipt_store_t s_fw_receipt;
static pet_firmware_control_t *s_firmware;
static pet_replace_control_t *s_pet;
static pet_firmware_bootloader_t s_boot_profile;
static bool s_boot_health_decided;
static bool s_single_boot_healthy;
static int64_t s_boot_guard_next;
static bool single_ready(void);
static bool single_readers_detached(void *unused);
/* A tap that arrived during a control step; guarded by s_audio_lock. */
static int64_t s_tap_pending_until;
static atomic_bool s_tap_pending;
/* The waiting tap waits for a control step, not for the session: control
 * yields to it after its current cloud call (independent_updates). */
static atomic_bool s_tap_for_control;
/* When the last turn ended (esp_timer ms), and whether control still leaves
 * the microphone free after it (conversation_grace). */
static atomic_uint s_turn_ended_ms;
static atomic_bool s_turn_grace;
/* The answer a tap stopped. The gateway ends it on input.cancel; its frames
 * already in flight are dropped quietly instead of forcing revalidation.
 * Guarded by s_audio_lock. */
static uint32_t s_stopped_output;
#if CONFIG_PET_POCKET_TERMINAL
/* Nonzero while the listening cue plays; the microphone opens after it.
 * Guarded by s_audio_lock. */
static uint32_t s_listen_token;
static esp_timer_handle_t s_wake_timer;
#endif

bool pet_vnext_independent_control(void)
{
    if (!s_lifecycle_selected)
    {
        pet_flash_layout_t layout;
        char sha[65];
        s_fw_lifecycle = pet_flash_layout_read(&layout, sha) != ESP_OK || !pet_flash_layout_allows_legacy_ota(&layout);
        s_lifecycle_selected = true;
    }
    return s_fw_lifecycle;
}

static bool queue_audio(const audio_event_t *event)
{
    audio_event_t queued = *event;
    queued.generation = atomic_load(&s_generation);
    bool ok = s_audio_events && xQueueSend(s_audio_events, &queued, 0) == pdTRUE;
    if (!ok)
        pet_diagnostics_note_dropped_app_event();
    return ok;
}
static void tapped(void)
{
    const audio_event_t event = {.kind = AUDIO_TAP};
    queue_audio(&event);
}
#if CONFIG_PET_POCKET_TERMINAL
static void listen_cue_done(uint32_t token, pet_sfx_outcome_t outcome)
{
    const audio_event_t event = {.kind = AUDIO_LISTEN_CUE_DONE,
                                 .stream = token,
                                 .result = outcome == PET_SFX_CANCELLED ? ESP_ERR_NOT_FINISHED : ESP_OK};
    queue_audio(&event);
}
/* The codec and amplifier settle after audio start before the first cue. */
static void wake_cue(void *unused)
{
    (void)unused;
    pet_sfx_play(PET_SFX_WAKE);
}
#endif
static void settings_opened(void)
{
    pet_battery_set_enabled(true);
    pet_onboarding_open_from_ui();
#if CONFIG_PET_POCKET_TERMINAL
    pet_sfx_play(PET_SFX_SETTINGS_OPEN);
#endif
}
static bool library_requested(bool next)
{
    const control_command_t command = {.kind = CONTROL_LIBRARY, .next = next};
    return xQueueSend(s_commands, &command, 0) == pdTRUE;
}
static bool selection_requested(const char *build)
{
    if (!build || strlen(build) != 36)
        return false;
    control_command_t command = {.kind = CONTROL_SELECT};
    strcpy(command.build_id, build);
    return xQueueSend(s_commands, &command, 0) == pdTRUE;
}
#if CONFIG_PET_POCKET_TERMINAL
/* LVGL context: the pet shows at once, not after the control task's next
 * network step. The shown pet is a PSRAM copy, so the screen is always safe. */
static void return_requested(void)
{
    pet_battery_set_enabled(false);
    if (atomic_load(&s_face_started))
    {
        pet_face_show();
        pet_sfx_play(PET_SFX_SETTINGS_CLOSE);
    }
}
/* LVGL context: the level is heard at once (audio task); kept once settled. */
static bool volume_requested(uint8_t volume, bool save)
{
    if (volume > 100)
        return false;
    atomic_store(&s_volume_now, volume);
    if (save)
    {
        const control_command_t command = {.kind = CONTROL_KEEP_VOLUME, .value = volume};
        return xQueueSend(s_commands, &command, 0) == pdTRUE;
    }
    const audio_event_t event = {.kind = AUDIO_VOLUME, .stream = volume};
    return queue_audio(&event);
}
static bool brightness_requested(uint8_t brightness, bool save)
{
    if (brightness < PET_BRIGHTNESS_MIN || brightness > 100)
        return false;
    atomic_store(&s_brightness_now, brightness);
    if (save)
    {
        const control_command_t command = {.kind = CONTROL_KEEP_BRIGHTNESS, .value = brightness};
        return xQueueSend(s_commands, &command, 0) == pdTRUE;
    }
    if (pet_face_set_brightness(brightness) != ESP_OK)
        bsp_display_brightness_set(brightness);
    return true;
}
#else
static void return_requested(void)
{
    const control_command_t command = {.kind = CONTROL_RETURN};
    xQueueSend(s_commands, &command, 0);
}
#endif
static bool retry_requested(void)
{
    const control_command_t command = {.kind = CONTROL_RETRY};
    return xQueueSend(s_commands, &command, 0) == pdTRUE;
}
static bool restart_requested(void)
{
    const control_command_t command = {.kind = CONTROL_RESTART};
    return xQueueSend(s_commands, &command, 0) == pdTRUE;
}
#if CONFIG_PET_POCKET_TERMINAL
/* LVGL context: hand the swipe to the control task and never block. While a
 * cloud step holds that task, say so at once instead of looking stuck. */
static void pet_swiped(int direction)
{
    unsigned busy_since = atomic_load(&s_control_busy_since);
    if (busy_since && (unsigned)(esp_timer_get_time() / 1000) - busy_since >= CONTROL_BUSY_NOTICE_MS)
        pet_face_announce("> busy, can't switch yet");
    if (atomic_exchange(&s_swipe, direction > 0 ? 1 : -1))
        return; /* The waiting swipe's command is already queued. */
    const control_command_t command = {.kind = CONTROL_SWITCH};
    if (xQueueSend(s_commands, &command, 0) != pdTRUE)
        atomic_store(&s_swipe, 0);
}
/* LVGL context: a long press pets the pet, and with a gateway that tells
 * stories the pet then tells one (begin_story). It never opens the microphone. */
static void face_gesture(uint8_t gesture)
{
    if (gesture == FC_GESTURE_SPIN_CW)
    {
        const audio_event_t event = {.kind = AUDIO_PET};
        queue_audio(&event);
    }
}
#endif

static void quiesce_audio(void)
{
    atomic_store(&s_voice_allowed, false);
    atomic_store(&s_gateway_connected, false);
    atomic_store(&s_handshake_pending, false);
#if CONFIG_PET_POCKET_TERMINAL
    atomic_store(&s_rebinding, false);
#endif
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    if (s_audio_started)
    {
        pet_audio_capture_stop();
        pet_audio_playback_cancel();
    }
#if CONFIG_PET_POCKET_TERMINAL
    if (s_listen_token)
    {
        s_listen_token = 0;
        pet_sfx_cancel();
    }
#endif
#if CONFIG_PET_POCKET_TERMINAL
    /* A tap waiting for the session survives a reconnect; it has its own deadline. */
    if (!s_tap_connecting)
#endif
    {
        atomic_store(&s_tap_pending, false);
        atomic_store(&s_tap_for_control, false);
        s_tap_pending_until = 0;
    }
    atomic_store(&s_turn_active, false);
    pet_runtime_release(&s_resources, PET_RESOURCE_VOICE);
    /* Reconnecting needs control at once. */
    atomic_store(&s_turn_grace, false);
    s_input_stream = s_output_stream = 0;
    s_stopped_output = 0;
#if CONFIG_PET_POCKET_TERMINAL
    if (s_face_started && !atomic_load(&s_tap_pending))
#else
    if (s_face_started)
#endif
        pet_face_set_state(PET_FACE_OFFLINE);
    xSemaphoreGive(s_audio_lock);
}
static void freeze(void *unused)
{
    (void)unused;
    quiesce_audio();
    pet_network_freeze_bound();
    /* A hello/audio callback already in flight can run during socket stop.
     * Revoke it again after the network worker has drained, before detachment
     * or unmapping is allowed. A final DISCONNECTED callback is not guaranteed. */
    quiesce_audio();
    atomic_fetch_add(&s_generation, 1); /* Old socket callbacks have now drained. */
}
static bool busy(void *unused)
{
    (void)unused;
    return pet_ota_busy() || pet_runtime_owns(&s_resources, PET_RESOURCE_VOICE) || pet_audio_is_capturing();
}
static void confirm_ota_health_if_ready(void)
{
    if (s_fw_lifecycle)
        return; /* No legacy callback may validate a v2 image. */
    if (!s_store.initialized || !atomic_load(&s_control_healthy))
        return;
    const bool has_pet = s_store.state.active_slot >= 0;
    const pet_ota_health_evidence_t evidence = {
        .native_ui_ready = true,
        .setup_service_ready = true,
        .storage_ready = true,
        .authenticated_control = true,
        .pet_runtime_ready = has_pet && atomic_load(&s_gateway_connected) && atomic_load(&s_face_started) &&
                             atomic_load(&s_audio_started),
    };
    if (!has_pet)
        pet_ota_confirm_health_stage(PET_OTA_HEALTH_ENROLLED_NO_PET, &evidence);
    else if (evidence.pet_runtime_ready)
        pet_ota_confirm_health_stage(PET_OTA_HEALTH_INSTALLED_PET, &evidence);
}
static void healthy(void *unused)
{
    (void)unused;
    atomic_store(&s_control_healthy, true);
    /* Control reconciliation and the conversation socket can become ready in
     * either order. Re-evaluate here and from connected() so an OTA image does
     * not miss its health confirmation because of callback ordering. */
    confirm_ota_health_if_ready();
}
static bool release_slot(void *unused, unsigned slot)
{
    (void)unused;
    if (slot > 1 || s_active_slot == (int)slot)
        return false;
    memset(&s_prepared[slot], 0, sizeof(s_prepared[slot]));
    return true;
}
/* `proven` bytes were validated in full just before; they are only inspected.
 * Binding a pack for display never validates it again (pet_face_pack.h).
 * `imported`: the manifest comes from a signed imported release, whose face ID
 * may be the derived `<pack ID>-<8 hex>` (pet_face_id.h). */
static bool prepare_pack(unsigned slot, const void *pack, size_t bytes, const pet_pack_verified_manifest_t *manifest,
                         bool proven, bool imported)
{
    fp_pack_info_t info = {0};
    if (slot > 1 || !manifest || strlen(manifest->face_id) >= PET_FACE_ID_MAX || bytes > UINT32_MAX ||
        (proven ? fp_inspect_prevalidated(pack, (uint32_t)bytes, &info)
                : pet_face_pack_validate(pack, (uint32_t)bytes, &info)) != FP_OK ||
        !info.approved || !pet_face_id_matches(manifest->face_id, info.id, info.id_len, imported) ||
        info.version_len != strlen(manifest->version) || memcmp(info.version, manifest->version, info.version_len))
        return false;
    s_prepared[slot] =
        (prepared_t){pack, bytes, *manifest,
                     info.gender < PET_FACE_GENDER_COUNT ? (pet_face_gender_t)info.gender : PET_FACE_GENDER_NEUTRAL};
    return true;
}
static bool prepare(void *unused, unsigned slot, const void *pack, size_t bytes,
                    const pet_pack_verified_manifest_t *manifest)
{
    (void)unused;
    return prepare_pack(slot, pack, bytes, manifest, false, false);
}

static esp_err_t capture_chunk(uint32_t stream, uint32_t sequence, const int16_t *pcm, size_t samples)
{
    if (stream == atomic_load(&s_input_refused))
        return ESP_OK;
    return stream == s_input_stream && atomic_load(&s_voice_allowed)
               ? pet_network_send_microphone(stream, sequence, pcm, samples)
               : ESP_ERR_INVALID_STATE;
}
static void capture_done(uint32_t stream, uint32_t sequence, uint32_t duration, esp_err_t result)
{
    const audio_event_t event = {
        .kind = AUDIO_CAPTURE_DONE, .stream = stream, .sequence = sequence, .duration = duration, .result = result};
    if (!queue_audio(&event))
        revalidate("audio queue full (capture)");
}
static void playback_done(uint32_t stream)
{
    const audio_event_t event = {.kind = AUDIO_PLAYBACK_DONE, .stream = stream};
    if (!queue_audio(&event))
        revalidate("audio queue full (playback)");
}
static void connected(bool ready)
{
    if (ready)
        atomic_store(&s_handshake_pending, false);
#if CONFIG_PET_POCKET_TERMINAL
    atomic_store(&s_rebinding, false);
    /* A session answering for a pet no longer on screen: its voice stays off. */
    if (ready && atomic_load(&s_voice_paused))
        return;
#endif
    if (ready)
    {
        atomic_store(&s_brain_disconnected, false);
        atomic_store(&s_brain_reconnect, false);
    }
    bool was_connected = atomic_exchange(&s_gateway_connected, ready);
#if CONFIG_PET_POCKET_TERMINAL
    if (ready && !was_connected)
        pet_sfx_play(PET_SFX_CONNECT);
#else
    (void)was_connected;
#endif
    atomic_store(&s_voice_allowed, ready && !pet_ota_busy() && !atomic_load(&s_fw_reserved));
    if (ready)
        confirm_ota_health_if_ready();
    if (!ready)
    {
        atomic_store(&s_voice_allowed, false);
        atomic_store(&s_brain_disconnected, true);
        atomic_store(&s_brain_reconnect, true);
    }
#if CONFIG_PET_POCKET_TERMINAL
    /* A waiting tap keeps its face until it listens or gives up, through a reconnect too. */
    if (s_face_started && !atomic_load(&s_tap_pending))
        pet_face_set_state(ready ? PET_FACE_IDLE : PET_FACE_OFFLINE);
#else
    if (s_face_started)
        pet_face_set_state(ready ? PET_FACE_IDLE : PET_FACE_OFFLINE);
#endif
}
static void state_changed(const char *state)
{
    if (!s_face_started || !atomic_load(&s_voice_allowed))
        return;
    if (strcmp(state, "thinking") && strcmp(state, "idle"))
        return;
    const audio_event_t event = {.kind = AUDIO_STATE, .stream = !strcmp(state, "thinking")};
    if (!queue_audio(&event))
        revalidate("audio queue full (state)");
}
static void expression_changed(const char *expression)
{
    if (!s_face_started || !atomic_load(&s_voice_allowed))
        return;
    pet_expression_t value;
    if (pet_expression_from_wire(expression, &value))
        pet_face_set_expression(value);
}
static void gesture_requested(uint8_t gesture)
{
    const audio_event_t event = {.kind = AUDIO_GESTURE, .stream = gesture};
    queue_audio(&event);
}
static void motion_gesture(uint8_t gesture, pet_motion_trigger_t trigger)
{
    const audio_event_t event = {
        .kind = AUDIO_GESTURE, .stream = gesture, .sequence = trigger == PET_MOTION_TRIGGER_SHAKE};
    queue_audio(&event);
}
static bool motion_enabled(void)
{
    return s_face_started && !atomic_load(&s_turn_active) && pet_face_shake_detection_enabled();
}
static void battery_updated(const pet_battery_snapshot_t *snapshot)
{
    if (!snapshot)
        return;
    atomic_store(&s_battery_valid, snapshot->valid);
    atomic_store(&s_battery_percent, snapshot->percent);
    atomic_store(&s_battery_charging, snapshot->state == PET_BATTERY_CHARGING || snapshot->state == PET_BATTERY_FULL);
    if (s_face_started)
        pet_face_set_battery(snapshot);
    pet_network_set_battery_snapshot(snapshot);
}
/* The audio task never holds s_audio_lock across network I/O, so speech from
 * the network task can wait briefly for it. Dropping a chunk instead would
 * desynchronize the stream and tear the session down. */
#define AUDIO_LOCK_WAIT pdMS_TO_TICKS(100)
/* The speaker's queue holds ten 40 ms frames (pet_audio.c) and never waits.
 * After a network stall the frames the gateway paced in real time arrive at
 * once, more than it holds. A frame waits for room while the socket holds the
 * rest: playback backpressure is never a broken session. A frame the speaker
 * still cannot take is skipped but counted, so the reply's end marker matches
 * and the conversation goes on. */
/* Speech plays without the +6 dB realtime boost. Cloud replies already peak at
 * 74-94% of full scale, so the boost drove the per-sample soft limiter on 1.4-2.6%
 * of all samples (captured 26 Sep), audible as crackle on loud syllables. The
 * Pocket has no console to change rt_boost, so it is fixed here; loudness stays
 * with the volume setting. */
#define VNEXT_SPEECH_BOOST PET_REALTIME_BOOST_OFF
#define SPEAKER_ROOM_WAIT_MS 600
#define SPEAKER_ROOM_POLL_MS 10
static unsigned s_output_skipped;
static esp_err_t audio_start(uint32_t stream, uint32_t rate)
{
    if (xSemaphoreTake(s_audio_lock, AUDIO_LOCK_WAIT) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_audio_started && atomic_load(&s_voice_allowed) && pet_network_is_ready() &&
        (pet_runtime_owns(&s_resources, PET_RESOURCE_VOICE) || pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE)))
    {
        s_output_stream = stream;
        s_output_sequence = 0;
        s_stopped_output = 0;
        s_output_skipped = 0;
        atomic_store(&s_turn_active, true);
        pet_face_set_state(PET_FACE_SPEAKING);
        result = pet_audio_playback_start(stream, rate, VNEXT_SPEECH_BOOST, PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED);
    }
#if CONFIG_PET_POCKET_TERMINAL
    /* An answer still arriving for the previous pet, or before a rebind is
     * accepted, is dropped quietly: the open session stays. */
    if (result != ESP_OK && (atomic_load(&s_voice_paused) || atomic_load(&s_handshake_pending)))
        s_stopped_output = stream;
    else
#endif
        if (result != ESP_OK)
        revalidate("speaker refused a reply");
    xSemaphoreGive(s_audio_lock);
    return result;
}
static esp_err_t audio_chunk(uint32_t stream, uint32_t sequence, const uint8_t *pcm, size_t bytes)
{
    if (xSemaphoreTake(s_audio_lock, AUDIO_LOCK_WAIT) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (atomic_load(&s_voice_allowed) && stream == s_output_stream && sequence == s_output_sequence)
    {
        /* Room for this frame and the end marker (pet_audio_playback_finish). */
        for (unsigned waited = 0;
             pet_audio_is_playing() && !pet_audio_playback_queue_healthy() && waited < SPEAKER_ROOM_WAIT_MS;
             waited += SPEAKER_ROOM_POLL_MS)
            vTaskDelay(pdMS_TO_TICKS(SPEAKER_ROOM_POLL_MS));
        result = pet_audio_playback_enqueue(stream, sequence, pcm, bytes);
        if (result == ESP_ERR_NO_MEM)
        {
            ++s_output_skipped;
            result = ESP_OK;
        }
        if (result == ESP_OK)
            ++s_output_sequence;
    }
    else if (stream && stream == s_stopped_output)
        result = ESP_OK;
    if (result != ESP_OK)
        revalidate("speech frame refused");
    xSemaphoreGive(s_audio_lock);
    return result;
}
static void audio_end(uint32_t stream, uint32_t final_sequence)
{
    if (xSemaphoreTake(s_audio_lock, AUDIO_LOCK_WAIT) != pdTRUE)
    {
        revalidate("audio lock busy at speech end");
        return;
    }
    if (atomic_load(&s_voice_allowed) && stream == s_output_stream &&
        (!s_output_sequence || final_sequence == s_output_sequence - 1))
    {
        if (s_output_skipped)
            ESP_LOGW("pet_vnext", "speech stream %lu: speaker skipped %u of %lu frames", (unsigned long)stream,
                     s_output_skipped, (unsigned long)s_output_sequence);
        pet_audio_playback_finish(stream);
    }
    else if (!stream || stream != s_stopped_output)
        revalidate("speech end for an unknown stream");
    xSemaphoreGive(s_audio_lock);
}
/* Why a turn was refused, in the face's terminal line (PET_FACE_TERMINAL_MAX),
 * for the refusals an owner can fix at aipets.com. */
static const char *refusal_line(const char *code)
{
    static const struct
    {
        const char *code, *line;
    } lines[] = {
        {"OWNER_PET_UNAVAILABLE", "> update pet at aipets.com"},
        {"OWNER_KEY_REQUIRED", "> add key at aipets.com"},
        {"OWNER_KEY_UNAVAILABLE", "> check key at aipets.com"},
        {"OWNER_CARTESIA_KEY_REQUIRED", "> add cartesia key"},
        {"OWNER_AUDIO_MODEL_UNAVAILABLE", "> pick another voice model"},
        {"OWNER_DEVICE_CONVERSATION_DISABLED", "> talking isn't on yet"},
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        if (!strcmp(code, lines[i].code))
            return lines[i].line;
    return "> can't talk right now";
}
static void session_error(const char *code, const char *message, bool recoverable)
{
    (void)message;
    /* The frames of a refused turn still in flight are refused too; the turn
     * already ended with its reason. */
    if (!strcmp(code, "AUDIO_STREAM") && atomic_load(&s_input_refused) == atomic_load(&s_input_stream))
        return;
    const bool reconnect = !strcmp(code, "BINDING_MISMATCH") || !strcmp(code, "PAIRING_REJECTED") ||
                           !strcmp(code, "BRAIN_AUTH_REQUIRED") || !strcmp(code, PET_SESSION_REBIND_REFUSED);
    if (reconnect)
    {
        atomic_store(&s_brain_expires_ms, 0);
        revalidate(code);
    }
#if CONFIG_PET_POCKET_TERMINAL
    /* A refused rebind: reconnect with a new hello, as before, with no error cue. */
    if (!strcmp(code, PET_SESSION_REBIND_REFUSED))
    {
        revalidate("rebind refused");
        return;
    }
#endif
    const bool no_voice =
        !strcmp(code, "NO_SPEECH") || !strcmp(code, "STT_EMPTY") || !strcmp(code, "CARTESIA_STT_EMPTY");
    /* A refused turn stops sending the microphone now and says why; the
     * gateway would refuse every further frame of it. */
    const bool refused = !recoverable && !reconnect;
    if (refused)
        atomic_store(&s_input_refused, atomic_load(&s_input_stream));
    const audio_event_t event = {.kind = AUDIO_ERROR, .stream = no_voice, .line = refused ? refusal_line(code) : NULL};
    queue_audio(&event);
}
static bool config_received(const pet_synced_config_v2_t *config)
{
    const audio_event_t event = {.kind = AUDIO_CONFIG, .config = *config};
    return queue_audio(&event);
}
static bool speech_received(const pet_speech_preference_t *speech)
{
    const audio_event_t event = {.kind = AUDIO_SPEECH, .speech = *speech};
    return queue_audio(&event);
}
static esp_err_t ota_received(const pet_ota_request_t *request)
{
    if (s_fw_lifecycle)
        return ESP_ERR_NOT_SUPPORTED;
    pet_ota_request_t *copy = malloc(sizeof(*copy));
    if (!copy)
        return ESP_ERR_NO_MEM;
    *copy = *request;
    control_command_t command = {.kind = CONTROL_OTA, .ota = copy};
    if (xQueueSend(s_commands, &command, 0) == pdTRUE)
        return ESP_OK;
    pet_enrollment_clear(copy, sizeof(*copy));
    free(copy);
    return ESP_ERR_TIMEOUT;
}

static bool connect_brain(const pet_control_context_t *cloud);

static bool activate(void *unused, unsigned slot, const pet_control_context_t *cloud, bool online)
{
    (void)unused;
    if (slot > 1 || !s_prepared[slot].bytes || strcmp(s_prepared[slot].manifest.face_id, cloud->config.face_id))
        return false;
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    const pet_control_config_t *c = &cloud->config;
    bool ok = false;
    uint8_t volume = c->volume, brightness = c->brightness;
#if CONFIG_PET_POCKET_TERMINAL
    /* On the Pocket Terminal the owner sets these in the menu: one level for
     * every pet, whichever the cloud binds, including one still being chosen. */
    volume = (uint8_t)atomic_load(&s_volume_now);
    brightness = (uint8_t)atomic_load(&s_brightness_now);
#endif
    if (pet_display_lock(-1) != ESP_OK)
        goto done;
    esp_err_t result = pet_face_pack_use_external(c->face_id, s_prepared[slot].bytes, s_prepared[slot].length);
    pet_display_unlock();
    if (result != ESP_OK)
        goto done;
    if (!s_face_started)
    {
        if (s_face_attempted)
            goto done;
        s_face_attempted = true;
        const pet_face_callbacks_t callbacks = {
            .tapped = tapped,
            .settings_opened = settings_opened,
#if CONFIG_PET_POCKET_TERMINAL
            .pet_swiped = pet_swiped,
            .gesture_requested = face_gesture,
#endif
        };
        result =
            pet_face_init(&callbacks, c->volume, c->brightness, c->shake_sensitivity, s_wifi.wifi_ssid, c->face_id,
                          PET_VOICE_PUCK, c->recording_timeout, (pet_animation_profile_t)c->animation_profile,
                          PET_AI_MODE_OPENROUTER, PET_REALTIME_MODEL_2_1_MINI, (pet_speech_profile_t)c->speech_profile,
                          PET_REALTIME_VOICE_CEDAR, VNEXT_SPEECH_BOOST, PET_CARTESIA_VOICE_NEUTRAL,
                          s_prepared[slot].gender, PET_SPEECH_MOUTH_FULL_FRAME_AUTHORED);
        if (result != ESP_OK)
            goto done;
        s_face_started = true;
    }
    else if (pet_face_set_id(c->face_id) != ESP_OK)
        goto done;
    if (!s_audio_started)
    {
        if (s_audio_attempted)
            goto done;
        s_audio_attempted = true;
        if (pet_audio_init(capture_chunk, capture_done, playback_done) != ESP_OK)
            goto done;
        s_audio_started = true;
#if CONFIG_PET_POCKET_TERMINAL
        /* Sounds are optional: the pet still works if the cue worker cannot start. */
        if (pet_sfx_init(listen_cue_done) == ESP_OK)
        {
            const esp_timer_create_args_t wake = {.callback = wake_cue, .name = "pet_wake_cue"};
            if (esp_timer_create(&wake, &s_wake_timer) == ESP_OK)
                esp_timer_start_once(s_wake_timer, 400000);
        }
#endif
        if (!s_battery_started)
            s_battery_started = pet_battery_start(battery_updated) == ESP_OK;
        pet_motion_start(motion_gesture, motion_enabled, c->shake_sensitivity);
    }
    if (pet_audio_set_capture_timeout(c->recording_timeout) != ESP_OK ||
        pet_face_set_brightness(brightness) != ESP_OK ||
        pet_face_set_shake_sensitivity(c->shake_sensitivity) != ESP_OK ||
        pet_face_set_recording_timeout(c->recording_timeout) != ESP_OK ||
        pet_face_set_animation_profile((pet_animation_profile_t)c->animation_profile) != ESP_OK ||
        pet_face_set_speech_profile((pet_speech_profile_t)c->speech_profile, true, true, false) != ESP_OK)
        goto done;
    pet_audio_set_volume(volume);
    pet_motion_set_sensitivity(c->shake_sensitivity);
    s_applied = *cloud;
    s_active_slot = (int)slot;
#if CONFIG_PET_POCKET_TERMINAL
    /* Moving the open session to this pet (session-rebind-v1) is no
     * reconnect: the pet stays idle on screen, and a waiting tap says it is
     * switching. Only a new socket shows the connecting face. */
    if (!online)
        pet_face_set_state(PET_FACE_OFFLINE);
    else if (!pet_network_bound_to(cloud) && !pet_network_can_rebind())
        pet_face_set_state(PET_FACE_CONNECTING);
    else if (!atomic_load(&s_tap_pending))
        pet_face_set_state(PET_FACE_IDLE);
#else
    pet_face_set_state(online ? PET_FACE_CONNECTING : PET_FACE_OFFLINE);
#endif
    ok = true;
done:
    xSemaphoreGive(s_audio_lock);
    if (!ok)
    {
        atomic_store(&s_runtime_fault, true);
        return false;
    }
    if (!online)
        return true;
    atomic_store(&s_revalidate, false);
    return connect_brain(cloud);
}

static int32_t brain_remaining(uint64_t now)
{
    uint32_t expiry = atomic_load(&s_brain_expires_ms);
    return expiry ? (int32_t)(expiry - (uint32_t)now) : 0;
}

static bool connect_brain(const pet_control_context_t *cloud)
{
    const pet_control_config_t *c = &cloud->config;
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
    bool same = pet_control_same_binding(&s_brain_context.binding, &cloud->binding) &&
        !strcmp(s_brain_context.config.version, cloud->config.version);
    bool renewed = !same || !s_brain_token[0] ||
        brain_remaining(now) <= 60000;
    if (renewed)
    {
        char request[PET_SESSION_WIRE_REBIND_MAX], response[1024];
        pet_control_http_result_t reply = {0};
        const pet_control_http_t http = {.origin = s_origin, .device_id = s_id, .credential = s_credential};
        bool received = pet_session_wire_brain_request(cloud, request, sizeof(request)) &&
            pet_control_http_json(&http, "/v1/device/brain", request, response, sizeof(response), &reply) &&
            reply.status == 200;
        cJSON *root = received ? pet_control_json(response, reply.bytes, sizeof(response)) : NULL;
        bool valid = root && pet_session_wire_brain_grant(root, cloud, s_origin, s_brain_token);
        cJSON_Delete(root);
        pet_enrollment_clear(response, sizeof(response));
        if (!valid)
        {
            /* An answer for another endpoint, binding or token format. */
            if (received)
                ESP_LOGW("pet_vnext", "BRAIN_GRANT_INVALID");
            s_brain_retry_ms = now + 15000;
            atomic_store(&s_brain_reconnect, true);
            if (!same || brain_remaining(now) <= 0 ||
                reply.status == 401 || reply.status == 403)
            {
                pet_network_freeze_bound();
                atomic_store(&s_voice_allowed, false);
                pet_face_set_state(PET_FACE_OFFLINE);
            }
            return true; /* Art/control remain admitted when only voice is unavailable. */
        }
        s_brain_context = *cloud;
        atomic_store(&s_brain_expires_ms, (uint32_t)now + 900000);
    }
    pet_config_t config = s_wifi;
    if (!pet_session_wire_origin(s_origin, config.gateway_host, sizeof(config.gateway_host), &config.gateway_port))
    {
        pet_enrollment_clear(&config, sizeof(config));
        return false;
    }
    config.gateway_secure = true;
    strlcpy(config.pairing_token, s_brain_token, sizeof(config.pairing_token));
    config.credential_version = 0;
    /* Enrollment is a separate identity. Never execute an old legacy pending
     * Wi-Fi/credential operation when the first bound session connects. */
    config.pending_wifi_operation_id[0] = config.pending_wifi_ssid[0] = 0;
    pet_enrollment_clear(config.pending_credential_token, sizeof(config.pending_credential_token));
    config.pending_credential_operation_id[0] = 0;
    config.pending_credential_version = 0;
    config.volume = c->volume;
    config.brightness = c->brightness;
    config.shake_sensitivity = c->shake_sensitivity;
    config.recording_timeout_seconds = c->recording_timeout;
    config.animation_profile = (pet_animation_profile_t)c->animation_profile;
    config.speech_profile = (pet_speech_profile_t)c->speech_profile;
    strlcpy(config.face_id, c->face_id, sizeof(config.face_id));
    strlcpy(config.ai_pet_id, c->ai_pet_id, sizeof(config.ai_pet_id));
    const pet_network_callbacks_t callbacks = {.connected = connected,
                                               .state = state_changed,
                                               .expression = expression_changed,
                                               .gesture = gesture_requested,
                                               .audio_start = audio_start,
                                               .audio_chunk = audio_chunk,
                                               .audio_end = audio_end,
                                               .session_error = session_error,
                                               .config_v2_received = config_received,
                                               .speech_preference_received = speech_received,
                                               .ota_update = ota_received};
    atomic_store(&s_brain_reconnect, false);
    pet_network_set_brain_token(s_brain_token);
#if CONFIG_PET_POCKET_TERMINAL
    /* The pet on screen is being admitted: its session's answer may enable voice. */
    atomic_store(&s_voice_paused, false);
    /* Over the open session: already this binding's, or moved to it without a
     * new socket (session-rebind-v1). Otherwise a new socket, as before. */
    if (pet_network_bound_to(cloud) && !renewed)
    {
        pet_enrollment_clear(&config, sizeof(config));
        connected(true);
        return true;
    }
    s_handshake_deadline = esp_timer_get_time() + REBIND_TIMEOUT_US;
    atomic_store(&s_handshake_pending, true);
    /* Set first: the acceptance can arrive before the send returns. */
    atomic_store(&s_rebinding, true);
    if (pet_network_rebind(cloud) == ESP_OK)
    {
        pet_enrollment_clear(&config, sizeof(config));
        return true;
    }
    atomic_store(&s_rebinding, false);
    /* No rebind after all: a new socket, which is a reconnect and says so. */
    pet_face_set_state(PET_FACE_CONNECTING);
#endif
    s_handshake_deadline = esp_timer_get_time() + 15000000;
    atomic_store(&s_handshake_pending, true);
    esp_err_t result = pet_network_start_bound(&config, &callbacks, s_id, cloud);
    pet_enrollment_clear(&config, sizeof(config));
    if (result != ESP_OK)
    {
        atomic_store(&s_handshake_pending, false);
        return false;
    }
    return true; /* Only acknowledged gateway.hello enables voice. */
}

static bool same_config(const pet_synced_config_v2_t *c)
{
    char version[16];
    snprintf(version, sizeof(version), "%lu", (unsigned long)c->version);
    const pet_control_config_t *a = &s_applied.config;
    return !strcmp(version, a->version) && c->volume == a->volume && c->brightness == a->brightness &&
           c->shake_sensitivity == a->shake_sensitivity && c->recording_timeout_seconds == a->recording_timeout &&
           c->animation_profile == a->animation_profile && c->speech_profile == a->speech_profile &&
           !strcmp(c->face_id, a->face_id) && !strcmp(c->ai_pet_id, a->ai_pet_id);
}
/* Websocket sends stay outside s_audio_lock. The network task delivers speech
 * under that lock, so holding it across a send (up to 500 ms) would stall or
 * drop an answer and tear the session down. handle_audio() decides and updates
 * state under the lock; finish_send() then talks to the gateway and re-checks
 * that the attempt is still current before opening the microphone. */
typedef enum
{
    SEND_NONE,
    SEND_CANCEL,
    SEND_INPUT_START,
    SEND_INPUT_END,
    SEND_STORY,
    SEND_CONFIG,
    SEND_SPEECH
} send_kind_t;
typedef struct
{
    send_kind_t kind;
    uint32_t stream, sequence, duration, generation;
    bool same;
    char ai_pet_id[sizeof(((pet_control_config_t *)0)->ai_pet_id)];
} audio_send_t;
/* A tap during a control step waits for it, then listens (wait_for_control).
 * A step is one cloud call, usually a few seconds; one that keeps control
 * longer than this gives up with a friendly retry. */
#define TAP_CONTROL_WAIT_US 10000000
/* After a turn the control task leaves the microphone free this long before
 * its next cloud step: polls wait during a turn, so one is always due right
 * after a reply, just when a follow-up tap comes. */
#define CONVERSATION_GRACE_MS 8000u

/* A turn ended: the reply was heard, stopped or failed, or the listen ended
 * without one. Voice is free, and control waits CONVERSATION_GRACE_MS. */
static void close_turn(void)
{
    atomic_store(&s_turn_active, false);
    pet_runtime_release(&s_resources, PET_RESOURCE_VOICE);
    atomic_store(&s_turn_ended_ms, (unsigned)(esp_timer_get_time() / 1000));
    atomic_store(&s_turn_grace, true);
}
/* Voice is free again; only the end of an actual turn starts the grace. */
static void free_voice(void)
{
    if (atomic_load(&s_turn_active))
        close_turn();
    else
        pet_runtime_release(&s_resources, PET_RESOURCE_VOICE);
}
static void abandon_turn(void)
{
    s_input_stream = 0;
    close_turn();
    pet_face_set_state(PET_FACE_IDLE);
}
/* A listen that ended without being sent (the microphone's send queue
 * overflowed while the socket stalled, or the codec failed) says so instead
 * of going quietly idle. Called with s_audio_lock held. */
static void listen_failed(esp_err_t result)
{
    (void)result; /* Only logged and shown on the Pocket's terminal line. */
    ESP_LOGW("pet_vnext", "listening stopped: %s", esp_err_to_name(result));
    pet_face_set_state(PET_FACE_ERROR);
#if CONFIG_PET_POCKET_TERMINAL
    pet_sfx_play(PET_SFX_ERROR);
    pet_face_announce(result == ESP_ERR_NO_MEM || result == ESP_ERR_TIMEOUT ? "> network slow, try again"
                      : result == ESP_ERR_INVALID_STATE                     ? "> offline, try again"
                                                                            : "> mic error, try again");
#endif
}
/* The gateway message that starts the current turn, for finish_send(). */
static void request_turn(audio_send_t *send, send_kind_t kind)
{
    *send = (audio_send_t){.kind = kind, .stream = s_input_stream, .generation = atomic_load(&s_generation)};
    strlcpy(send->ai_pet_id, s_applied.config.ai_pet_id, sizeof(send->ai_pet_id));
}
/* Voice is claimed: a turn on a new input stream, never 0. */
static void start_turn(void)
{
    if (!s_next_input_stream)
        s_next_input_stream = esp_random();
    if (!++s_next_input_stream)
        ++s_next_input_stream;
    s_input_stream = s_next_input_stream;
    s_output_stream = 0;
    atomic_store(&s_turn_active, true);
}
/* Voice is claimed. Show listening at once; the microphone follows. */
static void begin_listening(audio_send_t *send)
{
    start_turn();
    pet_face_set_state(PET_FACE_LISTENING);
#if CONFIG_PET_POCKET_TERMINAL
    /* The listening cue plays first; its completion opens the microphone. */
    uint32_t token = (esp_random() & 0x7fffffffu) | 1u;
    if (pet_sfx_play_listen(token) == ESP_OK)
    {
        s_listen_token = token;
        return;
    }
#endif
    request_turn(send, SEND_INPUT_START);
}
#if CONFIG_PET_POCKET_TERMINAL
/* Voice is claimed. The pet tells what is happening in its own world: a turn
 * without the microphone, answered like a spoken one (thinking, speech, idle).
 * The face stays as it is meanwhile, so the touch reaction plays. */
static void begin_story(audio_send_t *send)
{
    start_turn();
    request_turn(send, SEND_STORY);
}
#endif
static void clear_pending_tap(void)
{
    atomic_store(&s_tap_pending, false);
    atomic_store(&s_tap_for_control, false);
    s_tap_pending_until = 0;
#if CONFIG_PET_POCKET_TERMINAL
    s_tap_connecting = false;
#endif
}
/* A listening attempt that has not opened the microphone yet. */
static bool listening_soon(void)
{
#if CONFIG_PET_POCKET_TERMINAL
    if (s_listen_token)
        return true;
#endif
    return atomic_load(&s_tap_pending);
}

#if CONFIG_PET_POCKET_TERMINAL
/* The open session is moving to the pet on screen: a swipe or a selection
 * paused voice on a session that can rebind, or a rebind waits for its
 * answer. Without such a session, the pet reconnects instead. */
static bool switching_pets(void)
{
    return (atomic_load(&s_voice_paused) && pet_network_can_rebind()) || atomic_load(&s_rebinding);
}
/* Voice stops for the pet on screen while the session stays open: another pet
 * comes on screen, or a selection is about to move the session. The
 * microphone closes, playback and a starting listen end, the gateway cancels
 * the turn, and the previous pet's late conversation events are dropped. */
/* `same_pet`: the selection or rebind of the pet already on screen, which a
 * tap waiting for the switch keeps waiting through; a swipe withdraws it. */
static void pause_voice(bool same_pet)
{
    atomic_store(&s_voice_paused, true);
    atomic_store(&s_voice_allowed, false);
    atomic_store(&s_gateway_connected, false);
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    uint32_t cancel = atomic_load(&s_turn_active) ? s_input_stream : 0;
    if (s_audio_started)
    {
        pet_audio_capture_stop();
        pet_audio_playback_cancel();
    }
    /* With its audio stopped, the pet is idle. A face left SPEAKING or
     * LISTENING without playback or capture reads to pet_diagnostics as a
     * stall, and its recovery replaces the socket the switch keeps. */
    pet_face_state_t face = pet_face_get_state();
    if (s_face_started && (face == PET_FACE_LISTENING || face == PET_FACE_THINKING || face == PET_FACE_SPEAKING))
        pet_face_set_state(PET_FACE_IDLE);
    if (s_listen_token)
    {
        s_listen_token = 0;
        pet_sfx_cancel();
    }
    if (!same_pet || !s_tap_connecting)
        clear_pending_tap();
    atomic_store(&s_turn_active, false);
    pet_runtime_release(&s_resources, PET_RESOURCE_VOICE);
    /* The next pet's binding needs control at once. */
    atomic_store(&s_turn_grace, false);
    if (s_output_stream)
        s_stopped_output = s_output_stream;
    s_input_stream = s_output_stream = 0;
    atomic_fetch_add(&s_generation, 1);
    xSemaphoreGive(s_audio_lock);
    if (cancel)
        pet_network_cancel(cancel);
}
#endif

/* A cloud step holds control (voice and control never overlap): the tap
 * waits for it and listens as soon as it ends, with the listening cue.
 * Control yields to it after its current cloud call (independent_updates).
 * The face keeps its state until then: LISTENING means an open microphone,
 * and pet_diagnostics recovers from LISTENING without capture. */
static void wait_for_control(void)
{
    s_tap_pending_until = esp_timer_get_time() + TAP_CONTROL_WAIT_US;
    atomic_store(&s_tap_for_control, true);
    atomic_store(&s_tap_pending, true);
    ESP_LOGI("pet_vnext", "tap waits for a control step");
#if CONFIG_PET_POCKET_TERMINAL
    pet_face_announce("> one moment...");
#endif
}

static void handle_tap(audio_send_t *send)
{
    if (atomic_load(&s_tap_pending))
    {
        /* A second tap while waiting for a control step withdraws the first. */
        clear_pending_tap();
        if (!atomic_load(&s_turn_active))
            pet_face_set_state(PET_FACE_IDLE);
#if CONFIG_PET_POCKET_TERMINAL
        pet_face_announce("> cancelled");
#endif
        return;
    }
    pet_face_state_t face = pet_face_get_state();
    bool listen_pending = false;
#if CONFIG_PET_POCKET_TERMINAL
    listen_pending = s_listen_token != 0;
#endif
    switch (pet_tap_action(pet_audio_is_capturing(), listen_pending, pet_audio_is_playing(),
                           atomic_load(&s_turn_active) && face == PET_FACE_THINKING, face == PET_FACE_SPEAKING))
    {
    case PET_TAP_STOP_CAPTURE:
        pet_audio_capture_stop();
        break;
    case PET_TAP_CANCEL_LISTENING:
#if CONFIG_PET_POCKET_TERMINAL
        /* A second tap during the listening cue: the microphone never opens. */
        s_listen_token = 0;
        pet_sfx_cancel();
        abandon_turn();
#endif
        break;
    case PET_TAP_IGNORE_WAITING:
        /* A submitted turn keeps waiting for its answer. */
        break;
    case PET_TAP_STOP_ANSWER:
    {
        /* The gateway stops whatever it is producing on any cancel. */
        uint32_t stream = s_input_stream;
        pet_audio_playback_cancel();
        s_stopped_output = s_output_stream;
        s_output_stream = 0;
        abandon_turn();
        *send = (audio_send_t){.kind = SEND_CANCEL, .stream = stream};
        break;
    }
    case PET_TAP_START_LISTENING:
        if (pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE))
            begin_listening(send);
        else if (pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL))
            wait_for_control();
        break;
    }
}

/* Called with s_audio_lock held. */
static void handle_audio(const audio_event_t *event, audio_send_t *send)
{
    pet_diagnostics_note_app_event(event->kind, "vnext", 0);
    pet_diagnostics_note_streams(s_input_stream, s_output_stream);
    bool allowed = atomic_load(&s_voice_allowed) && pet_network_is_ready() && s_audio_started;
    if (!atomic_load(&s_turn_active) &&
        brain_remaining((uint64_t)esp_timer_get_time() / 1000) <= 0)
    {
        allowed = false;
        atomic_store(&s_brain_reconnect, true);
    }
    if (event->kind == AUDIO_STATE && allowed)
    {
        if (event->stream)
        {
            if (pet_runtime_owns(&s_resources, PET_RESOURCE_VOICE) ||
                pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE))
            {
                atomic_store(&s_turn_active, true);
                pet_face_set_state(PET_FACE_THINKING);
            }
            else
                revalidate("voice resource busy");
        }
        else if (!pet_audio_is_playing() && !pet_audio_is_capturing() && !listening_soon())
        {
            /* The gateway's idle after a cancel must not end the next attempt. */
            free_voice();
            pet_face_set_state(PET_FACE_IDLE);
        }
    }
    else if (event->kind == AUDIO_TAP && allowed)
    {
        handle_tap(send);
#if CONFIG_PET_POCKET_TERMINAL
    }
    else if (event->kind == AUDIO_TAP && s_face_started)
    {
        voice_block_t block = (voice_block_t)atomic_load(&s_voice_block);
        if (pet_network_wifi_is_ready() && block == VOICE_BLOCK_CONNECTING)
        {
            /* The session is connecting or moving to this pet: wait calmly and
             * listen once it is ready. The microphone stays closed until then. */
            if (atomic_load(&s_tap_pending))
            {
                /* Tapping again during a switch means the owner wants to talk:
                 * keep waiting and listen once the pet is ready. Only a slow
                 * reconnect's wait is withdrawn by a second tap. */
                if (switching_pets())
                {
                    pet_face_announce("> hold on, almost ready");
                    return;
                }
                clear_pending_tap();
                pet_face_set_state(PET_FACE_IDLE);
                pet_face_announce("> cancelled");
                return;
            }
            bool switching = switching_pets();
            s_tap_pending_until = esp_timer_get_time() + (switching ? TAP_SWITCH_WAIT_US : TAP_CONNECTING_US);
            s_tap_connecting = true;
            atomic_store(&s_tap_pending, true);
            if (switching)
            {
                /* The open session is moving to this pet: never a reconnect's face. */
                char line[PET_FACE_TERMINAL_MAX + 1];
                strlcpy(line, "> switching to ", sizeof(line));
                strlcat(line, s_shown_name, sizeof(line));
                pet_face_announce(line);
            }
            else
            {
                pet_face_set_state(PET_FACE_CONNECTING);
                pet_face_announce("> connecting...");
            }
            return;
        }
        /* Tap to talk with no voice: say why, instead of only the touch reaction. */
        pet_face_announce(!pet_network_wifi_is_ready()      ? "> no wifi: can't talk"
                          : block == VOICE_BLOCK_NOT_PAIRED ? "> not paired: no voice"
                                                            : "> not linked: no voice");
        int64_t now = esp_timer_get_time();
        if (now - s_no_voice_cue_at >= 1500000)
        {
            s_no_voice_cue_at = now;
            pet_sfx_play(PET_SFX_NO_VOICE);
        }
    }
    else if (event->kind == AUDIO_VOLUME)
    {
        /* A playing cue stops within one chunk, so the new level is heard at
         * once in the pop; the listening cue of a starting turn is kept. */
        if (s_audio_started && !s_listen_token)
            pet_sfx_cancel();
        pet_audio_set_volume((uint8_t)event->stream);
        if (s_audio_started)
            pet_sfx_play(PET_SFX_GESTURE_POP);
#endif
    }
    else if (event->kind == AUDIO_TAP_RETRY)
    {
        if (!atomic_load(&s_tap_pending))
            return;
#if CONFIG_PET_POCKET_TERMINAL
        if (s_tap_connecting)
        {
            /* Listen as soon as the session is ready; after the wait, a friendly retry. */
            voice_block_t block = (voice_block_t)atomic_load(&s_voice_block);
            if (!allowed && block != VOICE_BLOCK_CONNECTING)
            {
                /* The switch ended without voice (the cloud refused this pet, or
                 * it has no cloud release): say why at once, as a new tap would. */
                clear_pending_tap();
                if (!atomic_load(&s_turn_active))
                    pet_face_set_state(PET_FACE_IDLE);
                pet_face_announce(block == VOICE_BLOCK_NOT_PAIRED ? "> not paired: no voice"
                                                                  : "> not linked: no voice");
                pet_sfx_play(PET_SFX_NO_VOICE);
            }
            else if (esp_timer_get_time() >= s_tap_pending_until)
            {
                clear_pending_tap();
                if (!atomic_load(&s_turn_active))
                    pet_face_set_state(PET_FACE_IDLE);
                pet_face_announce("> not ready, try again");
            }
            else if (allowed && pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE))
            {
                clear_pending_tap();
                begin_listening(send);
            }
            return;
        }
#endif
        if (!allowed || esp_timer_get_time() >= s_tap_pending_until)
        {
            ESP_LOGW("pet_vnext", "waiting tap given up: %s", allowed ? "control kept" : "session closed");
            clear_pending_tap();
            if (!atomic_load(&s_turn_active))
                pet_face_set_state(PET_FACE_IDLE);
#if CONFIG_PET_POCKET_TERMINAL
            pet_face_announce("> not ready, try again");
#endif
        }
        else if (pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE))
        {
            clear_pending_tap();
            begin_listening(send);
        }
#if CONFIG_PET_POCKET_TERMINAL
    }
    else if (event->kind == AUDIO_LISTEN_CUE_DONE && allowed)
    {
        /* Stale when a tap, an error or a disconnect ended the attempt first. */
        if (s_listen_token && event->stream == s_listen_token)
        {
            s_listen_token = 0;
            if (event->result == ESP_OK)
                request_turn(send, SEND_INPUT_START);
            else
                abandon_turn();
        }
#endif
    }
    else if (event->kind == AUDIO_CAPTURE_DONE && allowed && event->stream == s_input_stream)
    {
        if (event->result == ESP_OK)
        {
#if CONFIG_PET_POCKET_TERMINAL
            pet_sfx_play(PET_SFX_SUBMIT);
#endif
            pet_face_set_state(PET_FACE_THINKING);
            *send = (audio_send_t){.kind = SEND_INPUT_END,
                                   .stream = event->stream,
                                   .sequence = event->sequence,
                                   .duration = event->duration,
                                   .generation = atomic_load(&s_generation)};
        }
        else
        {
            abandon_turn();
            listen_failed(event->result);
            *send = (audio_send_t){.kind = SEND_CANCEL, .stream = event->stream};
        }
    }
    else if (event->kind == AUDIO_PLAYBACK_DONE && event->stream == s_output_stream && allowed)
    {
        close_turn();
        pet_face_set_state(PET_FACE_IDLE);
    }
#if CONFIG_PET_POCKET_TERMINAL
    else if ((event->kind == AUDIO_CONFIG || event->kind == AUDIO_SPEECH) && s_audio_started &&
             !atomic_load(&s_gateway_connected))
    {
        /* No session answers for the pet yet: a boot, a join or a switch still
         * under way. A management answer then differs from the offline or the
         * previous pet's settings; revalidating dropped a switch and added 6.6 s
         * to every first connection (1 Oct). The admission brings the cloud's
         * settings, and the next answer compares against them. */
    }
#endif
    else if (event->kind == AUDIO_CONFIG && s_audio_started)
    {
        bool same = same_config(&event->config);
        *send = (audio_send_t){.kind = SEND_CONFIG, .same = same};
        /* HTTPS management supplies mouth timing independently of voice.
         * The next speech uses it; finish_send() keeps it for an offline boot. */
        if (same)
            pet_audio_set_speech_mouth_offset(event->config.speech_mouth_offset_ms);
        if (!same)
            revalidate("management config differs");
    }
    else if (event->kind == AUDIO_SPEECH && s_audio_started)
    {
        bool same = event->speech.profile == s_applied.config.speech_profile &&
                    !strcmp(event->speech.ai_pet_id, s_applied.config.ai_pet_id);
        *send = (audio_send_t){.kind = SEND_SPEECH, .same = same};
        if (!same)
            revalidate("management speech differs");
#if CONFIG_PET_POCKET_TERMINAL
    }
    else if (event->kind == AUDIO_ERROR && s_audio_started && !allowed)
    {
        /* No live session (connecting, switching or closed): nothing failed
         * that the owner started, so no error cue or face. */
#endif
    }
    else if (event->kind == AUDIO_ERROR && s_audio_started)
    {
        clear_pending_tap();
#if CONFIG_PET_POCKET_TERMINAL
        if (s_listen_token)
        {
            s_listen_token = 0;
            pet_sfx_cancel();
        }
        pet_sfx_play(event->stream ? PET_SFX_NO_VOICE : PET_SFX_ERROR);
#endif
        pet_audio_capture_stop();
        pet_audio_playback_cancel();
        free_voice();
        pet_face_set_state(PET_FACE_ERROR);
#if CONFIG_PET_POCKET_TERMINAL
        if (event->line)
            pet_face_announce(event->line);
#endif
    }
    else if (event->kind == AUDIO_GESTURE && s_face_started && !atomic_load(&s_turn_active))
    {
        uint8_t active = pet_face_trigger_gesture((uint8_t)event->stream);
#if CONFIG_PET_POCKET_TERMINAL
        if (active && event->sequence)
            pet_sfx_play(PET_SFX_GESTURE_SHAKE);
#else
        (void)active;
#endif
    }
#if CONFIG_PET_POCKET_TERMINAL
    else if (event->kind == AUDIO_PET && s_face_started && !atomic_load(&s_turn_active))
    {
        bool petted = pet_face_touch_reaction();
        bool story = allowed && pet_network_can_story();
        if (story && !pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE))
        {
            story = false;
            pet_face_announce("> busy, try again");
        }
        if (story)
            begin_story(send);
        if (petted || story)
            pet_sfx_play(PET_SFX_GESTURE_POP);
    }
#endif
}

/* Conversation events from an earlier session are stale; a volume change is
 * not conversation and always applies. */
static bool audio_event_current(const audio_event_t *event)
{
    return event->kind == AUDIO_VOLUME || event->generation == atomic_load(&s_generation);
}

/* Mouth timing as NVS holds it. Audio task only, after start. */
static bool s_mouth_offset_kept;
static int16_t s_mouth_offset_kept_ms;
/* Keeps an accepted mouth timing for an offline boot. Called without
 * s_audio_lock: the NVS write may take a moment. */
static void keep_mouth_offset(const pet_synced_config_v2_t *c)
{
    if (c->has_speech_mouth_offset == s_mouth_offset_kept && c->speech_mouth_offset_ms == s_mouth_offset_kept_ms)
        return;
    if (pet_config_store_speech_mouth_offset(c->has_speech_mouth_offset, c->speech_mouth_offset_ms) != ESP_OK)
    {
        ESP_LOGW("pet_vnext", "mouth timing not saved");
        return;
    }
    s_mouth_offset_kept = c->has_speech_mouth_offset;
    s_mouth_offset_kept_ms = c->speech_mouth_offset_ms;
}

/* Called without s_audio_lock. */
static void finish_send(const audio_send_t *send, const audio_event_t *event)
{
    if (send->kind == SEND_CANCEL)
        pet_network_cancel(send->stream);
    else if (send->kind == SEND_CONFIG)
    {
        if (send->same)
            keep_mouth_offset(&event->config);
        pet_network_config_v2_result(&event->config, send->same, send->same ? NULL : "BINDING_MISMATCH", NULL);
    }
    else if (send->kind == SEND_SPEECH)
        pet_network_speech_profile_result(&event->speech, send->same);
    else if (send->kind == SEND_INPUT_START)
    {
        esp_err_t announced = pet_network_input_start(send->stream, send->ai_pet_id);
        bool cancel = false;
        xSemaphoreTake(s_audio_lock, portMAX_DELAY);
        if (send->generation == atomic_load(&s_generation) && s_input_stream == send->stream &&
            atomic_load(&s_turn_active) && !pet_audio_is_capturing())
        {
            if (announced != ESP_OK)
            {
                /* The socket did not take the turn's start (a stalled uplink):
                 * say so, then reconnect. */
                abandon_turn();
                listen_failed(announced == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT);
                revalidate("listen start failed");
            }
            /* Speech that started meanwhile wins; this listening attempt ends. */
            else if (pet_audio_capture_start(send->stream) != ESP_OK)
            {
                abandon_turn();
                cancel = true;
            }
        }
        xSemaphoreGive(s_audio_lock);
        if (cancel)
            pet_network_cancel(send->stream);
    }
#if CONFIG_PET_POCKET_TERMINAL
    else if (send->kind == SEND_STORY)
    {
        esp_err_t asked = pet_network_story(send->stream, send->ai_pet_id);
        if (asked == ESP_OK)
            return;
        xSemaphoreTake(s_audio_lock, portMAX_DELAY);
        if (send->generation == atomic_load(&s_generation) && s_input_stream == send->stream &&
            atomic_load(&s_turn_active))
        {
            /* The socket did not take the request: say so, then reconnect. */
            abandon_turn();
            listen_failed(asked == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT);
            revalidate("story request failed");
        }
        xSemaphoreGive(s_audio_lock);
    }
#endif
    else if (send->kind == SEND_INPUT_END)
    {
        esp_err_t ended = pet_network_input_end(send->stream, send->sequence, send->duration);
        if (ended == ESP_OK)
            return;
        bool cancel = false;
        xSemaphoreTake(s_audio_lock, portMAX_DELAY);
        if (send->generation == atomic_load(&s_generation) && s_input_stream == send->stream &&
            atomic_load(&s_turn_active))
        {
            abandon_turn();
            listen_failed(ended);
            cancel = true;
        }
        xSemaphoreGive(s_audio_lock);
        if (cancel)
            pet_network_cancel(send->stream);
    }
}

static void audio_task(void *unused)
{
    (void)unused;
    audio_event_t event;
    for (;;)
    {
        /* A waiting tap is retried when control releases and expires on its own. */
        TickType_t wait = atomic_load(&s_tap_pending) ? pdMS_TO_TICKS(100) : portMAX_DELAY;
        if (xQueueReceive(s_audio_events, &event, wait) != pdTRUE)
            event = (audio_event_t){.kind = AUDIO_TAP_RETRY, .generation = atomic_load(&s_generation)};
        audio_send_t send = {.kind = SEND_NONE};
        xSemaphoreTake(s_audio_lock, portMAX_DELAY);
        if (!audio_event_current(&event))
        {
            xSemaphoreGive(s_audio_lock);
            continue;
        }
        handle_audio(&event, &send);
        xSemaphoreGive(s_audio_lock);
        finish_send(&send, &event);
    }
}

static void random_uuid(char id[37])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    snprintf(id, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1],
             bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
}
static void sanitize_support_code(char output[33], const char *input)
{
    size_t written = 0;
    if (input)
        for (size_t i = 0; input[i] && written < 32; ++i)
        {
            unsigned char value = (unsigned char)input[i];
            if (isalnum(value))
                output[written++] = (char)toupper(value);
            else if (value == '_' || value == '-')
                output[written++] = (char)value;
            else
                output[written++] = '_';
        }
    output[written] = 0;
}

static pet_onboarding_install_state_t installation_state(void)
{
    if (s_local_error[0] || atomic_load(&s_runtime_fault))
        return PET_ONBOARDING_INSTALL_FAILED;
#if CONFIG_PET_POCKET_TERMINAL
    if (s_slots.open || s_store_lost)
    {
        if (!s_slots.open)
            return PET_ONBOARDING_INSTALL_FAILED;
        /* Installed pets show offline before any worker runs. */
        if (!s_pet)
            return PET_ONBOARDING_INSTALL_IDLE;
        if (s_pet->status == PET_REPLACE_CONTROL_RECOVERY)
            return PET_ONBOARDING_INSTALL_FAILED;
        const pet_replace_t *s = &s_slots.machine.state;
        switch (s->phase)
        {
        case PET_REPLACE_REQUESTED:
        case PET_REPLACE_FENCED:
            return PET_ONBOARDING_INSTALL_REQUESTED;
        case PET_REPLACE_INVALIDATED:
            return PET_ONBOARDING_INSTALL_DOWNLOADING;
        case PET_REPLACE_DOWNLOADING:
            return s->downloaded_bytes == s->target.bytes ? PET_ONBOARDING_INSTALL_VERIFYING
                                                          : PET_ONBOARDING_INSTALL_DOWNLOADING;
        case PET_REPLACE_VERIFIED:
        case PET_REPLACE_ACTIVATING:
            return PET_ONBOARDING_INSTALL_ACTIVATING;
        case PET_REPLACE_ACTIVE:
            return PET_ONBOARDING_INSTALL_READY;
        case PET_REPLACE_RECOVERY:
            return PET_ONBOARDING_INSTALL_FAILED;
        default:
            return PET_ONBOARDING_INSTALL_IDLE;
        }
    }
#endif
    if (s_fw_lifecycle)
    {
        if (!single_ready() || !s_pet || s_pet->status == PET_REPLACE_CONTROL_RECOVERY)
            return PET_ONBOARDING_INSTALL_FAILED;
        const pet_replace_t *s = &s_single.state.state;
        switch (s->phase)
        {
        case PET_REPLACE_REQUESTED:
        case PET_REPLACE_FENCED:
            return PET_ONBOARDING_INSTALL_REQUESTED;
        case PET_REPLACE_INVALIDATED:
            return PET_ONBOARDING_INSTALL_DOWNLOADING;
        case PET_REPLACE_DOWNLOADING:
            return s->downloaded_bytes == s->target.bytes ? PET_ONBOARDING_INSTALL_VERIFYING
                                                          : PET_ONBOARDING_INSTALL_DOWNLOADING;
        case PET_REPLACE_VERIFIED:
        case PET_REPLACE_ACTIVATING:
            return PET_ONBOARDING_INSTALL_ACTIVATING;
        case PET_REPLACE_ACTIVE:
            return s_active_slot == 0 ? PET_ONBOARDING_INSTALL_READY : PET_ONBOARDING_INSTALL_VERIFYING;
        case PET_REPLACE_RECOVERY:
            return PET_ONBOARDING_INSTALL_FAILED;
        default:
            return PET_ONBOARDING_INSTALL_IDLE;
        }
    }
    if (s_sync && (s_sync->status == PET_SYNC_AUTH || s_sync->status == PET_SYNC_STORAGE ||
                   s_sync->status == PET_SYNC_UNTRUSTED || s_sync->status == PET_SYNC_RECONCILE))
        return PET_ONBOARDING_INSTALL_FAILED;
    if (!s_store.initialized)
        return PET_ONBOARDING_INSTALL_IDLE;
    if (s_sync && s_sync->status == PET_SYNC_VERIFY)
        return PET_ONBOARDING_INSTALL_VERIFYING;
    switch (s_store.state.phase)
    {
    case PET_INSTALL_REQUESTED:
        return PET_ONBOARDING_INSTALL_REQUESTED;
    case PET_INSTALL_DOWNLOADING:
        return PET_ONBOARDING_INSTALL_DOWNLOADING;
    case PET_INSTALL_VERIFIED:
    case PET_INSTALL_ACTIVATING:
        return PET_ONBOARDING_INSTALL_ACTIVATING;
    default:
        return s_store.state.active_slot >= 0 ? PET_ONBOARDING_INSTALL_READY : PET_ONBOARDING_INSTALL_IDLE;
    }
}

static void publish_status(int64_t retry_not_before)
{
    pet_onboarding_status_t status = {0};
    bool online = pet_network_wifi_is_ready();
    status.wifi = online ? PET_ONBOARDING_WIFI_CONNECTED
                         : (s_wifi.wifi_ssid[0] ? PET_ONBOARDING_WIFI_CONNECTING : PET_ONBOARDING_WIFI_DISCONNECTED);
    strlcpy(status.ssid, s_wifi.wifi_ssid, sizeof(status.ssid));
    status.installation = installation_state();
    status.conversation = !atomic_load(&s_face_started)       ? PET_ONBOARDING_CONVERSATION_UNAVAILABLE
                          : atomic_load(&s_runtime_fault)     ? PET_ONBOARDING_CONVERSATION_ERROR
                          : pet_audio_is_capturing()          ? PET_ONBOARDING_CONVERSATION_LISTENING
                          : pet_audio_is_playing()            ? PET_ONBOARDING_CONVERSATION_SPEAKING
                          : atomic_load(&s_turn_active)       ? PET_ONBOARDING_CONVERSATION_THINKING
                          : atomic_load(&s_gateway_connected) ? PET_ONBOARDING_CONVERSATION_IDLE
                                                              : PET_ONBOARDING_CONVERSATION_UNAVAILABLE;
    bool attention = status.installation == PET_ONBOARDING_INSTALL_FAILED;
    status.cloud = !atomic_load(&s_identity_ready)   ? PET_ONBOARDING_CLOUD_PAIRING
                   : !online                         ? PET_ONBOARDING_CLOUD_OFFLINE
                   : attention                       ? PET_ONBOARDING_CLOUD_ATTENTION
                   : atomic_load(&s_control_healthy) ? PET_ONBOARDING_CLOUD_CONNECTED
                                                     : PET_ONBOARDING_CLOUD_SYNCING;
    status.battery_valid = atomic_load(&s_battery_valid);
    status.battery_percent = (uint8_t)atomic_load(&s_battery_percent);
    status.battery_charging = atomic_load(&s_battery_charging);
    strlcpy(status.firmware_version, esp_app_get_description()->version, sizeof(status.firmware_version));
    status.single_pet_slot = s_fw_lifecycle;
#if CONFIG_PET_POCKET_TERMINAL
    status.volume = (uint8_t)atomic_load(&s_volume_now);
    status.volume_valid = true;
    status.brightness = (uint8_t)atomic_load(&s_brightness_now);
    status.brightness_valid = true;
    if (s_slots.open || s_store_lost)
    {
        status.single_pet_slot = false;
        status.has_pet = s_shown_slot >= 0 && s_prepared[0].bytes;
        const pet_slot_inventory_t *inventory = &s_slots.inventory.state;
        status.pet_capacity = PET_SLOT_COUNT;
        for (unsigned slot = 0; slot < PET_SLOT_COUNT; ++slot)
            if (inventory->slots[slot].state == PET_SLOT_READY)
            {
                ++status.pet_count;
                if (status.has_pet && (int)slot == s_shown_slot)
                    status.pet_index = status.pet_count;
            }
        /* NOT LINKED only for a pet with no cloud release for this account (a
         * USB-only copy) or one the cloud refused. Any other pet on screen shows
         * CONNECTING until its session is ready, including while it switches. */
        status.pet_linked = status.has_pet && !pocket_unlinked((unsigned)s_shown_slot);
        if (status.has_pet)
        {
            strlcpy(status.build_id, s_slots.inventory.state.slots[s_shown_slot].pack.build_id,
                    sizeof(status.build_id));
            strlcpy(status.pack_name, s_prepared[0].manifest.face_id, sizeof(status.pack_name));
            strlcpy(status.pack_version, s_prepared[0].manifest.version, sizeof(status.pack_version));
        }
        const pet_replace_t *install = &s_slots.machine.state;
        if (s_slots.inventory.state.target >= 0 && install->target.bytes)
            status.installation_percent = (unsigned)((uint64_t)install->downloaded_bytes * 100 / install->target.bytes);
    }
    else
#endif
        if (s_fw_lifecycle && single_ready())
    {
        const pet_replace_t *s = &s_single.state.state;
        status.has_pet = pet_replace_has_active(s) && s_active_slot == 0 && s_prepared[0].bytes;
        status.slots[0].bytes = pet_replace_has_active(s) ? s->active.bytes : s->target.bytes;
        status.slots[0].state = status.has_pet                  ? PET_SLOT_ACTIVE
                                : s->phase == PET_REPLACE_EMPTY ? PET_SLOT_EMPTY
                                                                : PET_SLOT_PARTIAL;
        if (status.has_pet)
        {
            strlcpy(status.build_id, s->active.build_id, sizeof(status.build_id));
            strlcpy(status.pack_name, s_prepared[0].manifest.face_id, sizeof(status.pack_name));
            strlcpy(status.pack_version, s_prepared[0].manifest.version, sizeof(status.pack_version));
        }
        status.installation_percent = s->target.bytes
                                          ? (unsigned)((uint64_t)s->downloaded_bytes * 100 / s->target.bytes)
                                      : status.installation == PET_ONBOARDING_INSTALL_READY ? 100
                                                                                            : 0;
    }
    else if (s_store.initialized)
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            status.slots[i].state = s_store.state.slots[i].state;
            status.slots[i].bytes = s_store.state.slots[i].bytes;
        }
        int active = s_store.state.active_slot;
        status.has_pet = active >= 0 && active < 2 && s_store.state.slots[active].state == PET_SLOT_ACTIVE;
        if (status.has_pet)
        {
            strlcpy(status.build_id, s_store.state.slots[active].build_id, sizeof(status.build_id));
            if (s_prepared[active].bytes)
            {
                strlcpy(status.pack_name, s_prepared[active].manifest.face_id, sizeof(status.pack_name));
                strlcpy(status.pack_version, s_prepared[active].manifest.version, sizeof(status.pack_version));
            }
        }
        int candidate = s_store.state.candidate_slot;
        if (candidate >= 0 && candidate < 2 && s_store.state.slots[candidate].bytes)
        {
            status.installation_percent =
                (unsigned)((uint64_t)s_store.state.downloaded_bytes * 100 / s_store.state.slots[candidate].bytes);
        }
        else if (status.installation == PET_ONBOARDING_INSTALL_READY)
            status.installation_percent = 100;
    }
#if CONFIG_PET_POCKET_TERMINAL
    atomic_store(&s_voice_block, !atomic_load(&s_identity_ready) ? VOICE_BLOCK_NOT_PAIRED
                                 : !status.pet_linked            ? VOICE_BLOCK_NOT_LINKED
                                                                 : VOICE_BLOCK_CONNECTING);
#else
    status.pet_linked = status.has_pet;
#endif
    int64_t now = esp_timer_get_time();
    if (retry_not_before > now)
        status.retry_seconds = (unsigned)((retry_not_before - now + 999999) / 1000000);
    const char *error = s_local_error;
    if (s_firmware && s_firmware->error_code[0])
        error = s_firmware->error_code;
    if (!error[0] && s_pet && s_pet->error_code[0])
        error = s_pet->error_code;
    if (!error[0] && s_sync && s_sync->error_code[0])
        error = s_sync->error_code;
    if (!error[0] && atomic_load(&s_runtime_fault))
        error = "RUNTIME_START_FAILED";
    if (!error[0] && s_sync)
    {
        if (s_sync->status == PET_SYNC_AUTH)
            error = "DEVICE_AUTH";
        else if (s_sync->status == PET_SYNC_STORAGE)
            error = "ASSET_STORAGE";
        else if (s_sync->status == PET_SYNC_UNTRUSTED)
            error = "PACK_UNTRUSTED";
        else if (s_sync->status == PET_SYNC_RECONCILE)
            error = "PACK_RECONCILE";
    }
    sanitize_support_code(status.support_error_code, error);
    pet_onboarding_update_status(&status);
}
static bool single_ready(void)
{
    return s_single.initialized && s_single.layout_valid && s_single.state.ready && s_single.state.journal.loaded &&
           s_single.state.journal.generation && pet_replace_valid(&s_single.state.state);
}
static bool single_empty(void)
{
    return single_ready() && s_single.state.state.phase == PET_REPLACE_EMPTY;
}
static pet_fw_storage_t firmware_health(void *unused);
static bool single_compatible(void *unused, const pet_release_v2_t *release)
{
    (void)unused;
    /* The IDF bootloader initializes erased USB otadata directly to VALID.
     * That is not UI/control health, and a first install has no OTA receipt to
     * establish it. Require actual service/UI evidence once on this boot, not
     * a fabricated OTA-success record. Keep this proof volatile: a reset must
     * establish it again. REQUESTED/FENCED deliberately revoke the old pet's
     * admission before switching to recovery UI, so that UI transition must
     * not retroactively revoke a health gate already passed on this boot.
     * Control stays live: a poll is fresh only until its next deadline. */
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
    if (!s_firmware || !s_firmware->authenticated || !s_firmware->next_poll_ms || now >= s_firmware->next_poll_ms ||
        !pet_onboarding_setup_ready() || !pet_setup_control_ready() || !pet_network_wifi_is_ready() ||
        !atomic_load(&s_identity_ready))
        return false;
    pet_firmware_boot_state_t boot;
    if (!single_ready() || !CONFIG_PET_VNEXT_FIRMWARE_EPOCH ||
        !pet_firmware_image_bootloader_matches(&s_boot_profile) || !pet_firmware_image_boot_state(&boot) ||
        !pet_ota_selection_is(&boot.selection, boot.running_slot, PET_OTA_STATE_VALID) ||
        boot.layout.id != s_single.layout.id || strcmp(boot.partition_sha256, s_single.partition_sha256))
        return false;
    pet_firmware_requirements_t observed = {.layout = boot.layout,
                                            .firmware_epoch = CONFIG_PET_VNEXT_FIRMWARE_EPOCH,
                                            .formats = 6,
                                            .codecs = 0xff,
                                            .imported_release = true,
                                            .bootloader = s_boot_profile};
    strcpy(observed.partition_sha256, boot.partition_sha256);
    if (!pet_release_v2_compatible(release, &observed))
        return false;
    if (!s_single_boot_healthy)
    {
        if (firmware_health(NULL) == PET_FW_STORAGE_NONE)
            return false;
        s_single_boot_healthy = true;
    }
    return true;
}
/* The last release whose pack passed full validation on this boot, and the
 * store's write count at the time. Reactivating the same pet reuses the proof
 * instead of inflating every frame again. */
static pet_release_v2_t s_proven;
static uint32_t s_proven_writes;
static bool s_proven_valid;
static bool single_validate_pack(const pet_release_v2_t *release, const void **pack, size_t *bytes,
                                 fp_pack_info_t *info)
{
    if (!single_compatible(NULL, release) || pet_single_store_map(&s_single, pack, bytes) != ESP_OK)
        return false;
    uint32_t writes = pet_single_store_writes();
    if (!info && s_proven_valid && s_proven_writes == writes && !memcmp(&s_proven, release, sizeof(s_proven)))
        return true;
    uint32_t size = fp_validation_workspace_size();
    void *workspace = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!workspace)
        return false;
    bool ok = pet_release_v2_validate_pack(release, *pack, *bytes, workspace, size, info);
    heap_caps_free(workspace);
    s_proven_valid = ok;
    if (ok)
    {
        s_proven = *release;
        s_proven_writes = writes;
    }
    return ok;
}
static bool single_verify(void *unused, const pet_release_v2_t *release)
{
    (void)unused;
    const void *pack = NULL;
    size_t bytes = 0;
    return single_validate_pack(release, &pack, &bytes, NULL);
}
static bool single_prepare_active(const pet_control_context_t *cloud)
{
    const pet_replace_t *state = &s_single.state.state;
    if (!cloud || !single_ready() || state->phase != PET_REPLACE_ACTIVE || !s_pet || !s_pet->has_context ||
        !s_pet->authenticated || strcmp(cloud->device_id, s_id) ||
        strcmp(cloud->account_id, s_pet->cloud.context.account_id) || !cloud->binding.assigned ||
        strcmp(cloud->binding.revision, state->binding_revision) ||
        strcmp(cloud->binding.relationship_id, state->relationship_id) ||
        strcmp(cloud->binding.build_id, state->active.build_id) || strcmp(cloud->binding.sha256, state->active.sha256))
        return false;
    /* The shared worker scratch is safe only on this serialized control task,
     * between HTTP operations. No signed bytes or approval are reconstructed. */
    pet_release_v2_t release;
    if (pet_single_store_read_manifest(&s_single, s_pet->chunk, PET_REPLACE_MANIFEST_BYTES) != ESP_OK ||
        !pet_release_v2_record_verify(s_pet->chunk, PET_REPLACE_MANIFEST_BYTES, cloud->account_id, &state->active,
                                      &s_trust, s_trust_count, &release) ||
        strcmp(release.face_id, cloud->config.face_id))
        return false;
    const void *pack = NULL;
    size_t bytes = 0;
    if (!single_validate_pack(&release, &pack, &bytes, NULL))
        return false;
    pet_pack_verified_manifest_t manifest = {.bytes = release.pack.bytes};
    strcpy(manifest.account_id, release.account_id);
    strcpy(manifest.project_id, release.project_id);
    strcpy(manifest.face_id, release.face_id);
    strcpy(manifest.version, release.version);
    /* Reuse the existing bounded player-init proof and UI/audio preparation.
     * Current settings may be newer than the immutable activation receipt. */
    return prepare_pack(0, pack, bytes, &manifest, true, release.imported);
}
static bool single_activate(void *unused, const pet_control_context_t *cloud)
{
    (void)unused;
    if (!single_prepare_active(cloud) || !activate(NULL, 0, cloud, true))
    {
        single_readers_detached(NULL);
        return false;
    }
    atomic_store(&s_runtime_fault, false);
    pet_battery_set_enabled(false);
    pet_face_show();
    return true;
}
static void firmware_protection(void *unused, bool fresh, pet_firmware_protection_t *out)
{
    (void)unused;
    (void)fresh;
    memset(out, 0, sizeof(*out)); /* One record: always proved. */
    pet_flash_layout_t physical;
    char hash[65];
    if (!single_ready() || pet_flash_layout_read(&physical, hash) != ESP_OK || physical.id != s_single.layout.id ||
        strcmp(hash, s_single.partition_sha256))
        return;
    out->known = single_empty();
    if (out->known || !single_ready() || !s_pet || !s_pet->has_context)
        return;
    /* Unknown/torn metadata stays unknown. Protecting a signed interrupted
     * target is not evidence that its incomplete bytes can be rendered. */
    if (pet_single_store_read_manifest(&s_single, s_pet->chunk, PET_REPLACE_MANIFEST_BYTES) == ESP_OK)
        out->known = pet_release_v2_protection(
            &s_single.state, &s_single.layout, s_single.partition_sha256, s_pet->chunk, PET_REPLACE_MANIFEST_BYTES,
            s_pet->cloud.context.account_id, &s_trust, s_trust_count, &out->active, &out->interrupted);
}
static pet_fw_storage_t firmware_health(void *unused)
{
    (void)unused;
    if (!pet_onboarding_setup_ready() || !pet_setup_control_ready() || !pet_network_wifi_is_ready() ||
        !atomic_load(&s_identity_ready) || !s_firmware || !s_pet)
        return PET_FW_STORAGE_NONE;
    if (single_ready() && s_single.state.state.phase == PET_REPLACE_ACTIVE && s_pet && s_pet->admitted &&
        s_active_slot == 0 && s_audio_started && pet_face_pack_ready())
        return PET_FW_STORAGE_PACK;
    if (!pet_onboarding_ui_ready())
        return PET_FW_STORAGE_NONE;
    return single_empty() ? PET_FW_STORAGE_SETUP : PET_FW_STORAGE_RECOVERY;
}
static bool single_readers_detached(void *unused)
{
    (void)unused;
    /* The store unmaps only after this returns true. Drain the bound socket
     * (including its catalog callbacks) and invalidate queued audio first. */
    freeze(NULL);
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    if (pet_display_lock(-1) != ESP_OK)
    {
        xSemaphoreGive(s_audio_lock);
        return false;
    }
    if (!pet_onboarding_setup_ready())
    {
        pet_display_unlock();
        xSemaphoreGive(s_audio_lock);
        return false;
    }
    pet_face_pack_release_external();
    memset(s_prepared, 0, sizeof(s_prepared));
    s_active_slot = -1;
    memset(&s_applied, 0, sizeof(s_applied));
    /* Keep the initialized UI/audio services: subsequent activation reuses
     * them. The renderer's now-empty player makes timer/gesture calls inert. */
    pet_onboarding_open_from_ui();
    pet_display_unlock();
    xSemaphoreGive(s_audio_lock);
    return true;
}
static void firmware_restart(void *unused)
{
    (void)unused;
    esp_restart();
}
static uint64_t firmware_now_ms(void *unused)
{
    (void)unused;
    return (uint64_t)esp_timer_get_time() / 1000;
}
static void independent_health_guard(void)
{
    /* Even missing identity, failed receipt NVS or a failed PSRAM allocation
     * must not make an unproven PENDING app immortal. This reads only qualified
     * boot evidence; a normal reset delegates rollback to the bootloader. */
    int64_t now = esp_timer_get_time();
    if (!s_fw_lifecycle || s_boot_health_decided || now < 90000000 || now < s_boot_guard_next)
        return;
    s_boot_guard_next = now + 1000000;
    if (!pet_firmware_image_bootloader_matches(&s_boot_profile))
        return;
    pet_firmware_boot_state_t boot;
    if (pet_firmware_image_boot_state(&boot))
    {
        if (pet_ota_selection_is(&boot.selection, boot.running_slot, PET_OTA_STATE_VALID))
            s_boot_health_decided = true;
        else if (pet_ota_selection_is(&boot.selection, boot.running_slot, PET_OTA_STATE_PENDING))
        {
            freeze(NULL);
            esp_restart();
        }
    }
}
/* The single-slot machine the pet worker runs: the single-pet store's, or on a
 * Pocket Terminal the three-pet inventory's `operation`. */
static const pet_replace_journal_t *pet_machine(void)
{
#if CONFIG_PET_POCKET_TERMINAL
    if (s_slots.open)
        return &s_slots.machine;
#endif
    return &s_single.state;
}
/* Conversation first: control leaves the microphone free for
 * CONVERSATION_GRACE_MS after a turn, so a follow-up tap listens at once
 * instead of waiting for the cloud poll the turn postponed. Never while new
 * firmware still has to prove itself: its health check runs under control. */
static bool conversation_grace(uint64_t now_ms)
{
    if (!atomic_load(&s_turn_grace))
        return false;
    if ((s_fw_lifecycle && !s_boot_health_decided) ||
        (uint32_t)now_ms - (uint32_t)atomic_load(&s_turn_ended_ms) >= CONVERSATION_GRACE_MS)
    {
        atomic_store(&s_turn_grace, false);
        return false;
    }
    return true;
}
static void management_step(uint64_t now)
{
    if (now < s_management_poll_ms) return;
    s_management_poll_ms = now + 30000;
    char *buffer = heap_caps_calloc(1, 16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return;
    const pet_control_http_t http = {.origin = s_origin, .device_id = s_id, .credential = s_credential};
    pet_control_http_result_t result = {0};
    if (pet_network_management_request(buffer, 8192) &&
        pet_control_http_json(&http, "/v1/device/management", buffer, buffer + 8192, 8192, &result) &&
        result.status == 200)
        pet_network_management_response(buffer + 8192, result.bytes);
    if (result.status == 401 || result.status == 403)
        revalidate("management credential rejected");
    free(buffer);
}

static void brain_disconnect_step(void)
{
    if (atomic_exchange(&s_brain_disconnected, false))
    {
        quiesce_audio();
        atomic_fetch_add(&s_generation, 1);
    }
}

static void brain_maintenance(const pet_control_context_t *context, uint64_t now)
{
    if (now >= s_brain_retry_ms &&
        (atomic_load(&s_brain_reconnect) ||
         brain_remaining(now) <= 60000))
    {
        if (!connect_brain(context))
        {
            atomic_store(&s_brain_reconnect, true);
            s_brain_retry_ms = now + 15000;
        }
    }
    management_step(now);
}

static void independent_updates(uint64_t now)
{
    brain_disconnect_step();
    if (atomic_load(&s_handshake_pending) && (uint64_t)s_handshake_deadline / 1000 <= now)
        revalidate("handshake or rebind timed out");
    if (atomic_exchange(&s_revalidate, false))
    {
        freeze(NULL);
        pet_replace_control_disconnect(s_pet);
    }
    bool online = pet_setup_control_ready() && pet_network_wifi_is_ready();
    if (!online)
    {
        pet_replace_control_disconnect(s_pet);
        pet_control_http_close_kept();
        if (s_firmware)
        {
            s_firmware->authenticated = false;
            s_firmware->next_poll_ms = 0;
        }
        atomic_store(&s_control_healthy, false);
    }
    bool reserved = pet_firmware_control_blocks_pet(s_firmware);
    if (reserved && !pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL))
        freeze(NULL);
    if (!reserved && !pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL) && conversation_grace(now))
        return;
    if (!pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL) && !pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL))
        return;
    atomic_store(&s_fw_reserved, reserved);
    if (s_firmware)
    {
        pet_firmware_control_health_deadline(s_firmware, now);
        if (online)
            pet_firmware_control_step(s_firmware, now);
    }
    reserved = pet_firmware_control_blocks_pet(s_firmware);
    atomic_store(&s_fw_reserved, reserved);
    /* A tap waits for this step: it listens first, the pet worker's call
     * comes on a later pass. */
    if (s_pet && online && !atomic_load(&s_tap_for_control))
    {
        bool had_context = s_pet->has_context, manifest = s_pet->manifest_ready;
        bool had_operation = s_pet->cloud.has_operation;
        pet_replace_phase_t local_phase = pet_machine()->state.phase;
        pet_replace_cloud_phase_t cloud_phase = s_pet->cloud.operation.phase;
        /* Firmware always goes first; its newly observed operation revokes pet
         * admission before any following pet step can write or activate. */
        pet_replace_control_step(s_pet, firmware_now_ms(NULL), !reserved);
        if (s_firmware && (had_context != s_pet->has_context || manifest != s_pet->manifest_ready ||
                           had_operation != s_pet->cloud.has_operation || local_phase != pet_machine()->state.phase ||
                           cloud_phase != s_pet->cloud.operation.phase))
            s_firmware->next_poll_ms = 0;
    }
    atomic_store(&s_control_healthy,
                 online && ((s_firmware && s_firmware->authenticated) || (s_pet && s_pet->authenticated)));
    if (online && !reserved && !atomic_load(&s_tap_for_control))
    {
        if (s_pet && s_pet->admitted && s_pet->authenticated)
            brain_maintenance(&s_pet->cloud.context, now);
        else
            management_step(now);
    }
    if (!reserved)
        pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
    if (!reserved && atomic_load(&s_tap_pending))
    {
        const audio_event_t retry = {.kind = AUDIO_TAP_RETRY};
        queue_audio(&retry);
    }
}
static bool single_reopen(void)
{
    if (!pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL))
        return false;
    pet_replace_control_disconnect(s_pet);
    /* An outstanding firmware receipt is not an active flash write. It must
     * not prevent reopening the journal needed to prove pack compatibility.
     * Quiesce any actual/uncertain OTA handle first, retaining its receipt and
     * reservation: its immutable download can subsequently restart from zero. */
    if (s_firmware && (s_firmware->image.active || s_firmware->writer_fault))
    {
        s_firmware->writer_fault = !pet_firmware_image_abort(&s_firmware->image);
        if (s_firmware->writer_fault)
            return false;
    }
    if (s_pet)
    {
        s_pet->manifest_ready = s_pet->verified = false;
        s_pet->next_poll_ms = 0;
    }
    const pet_single_readers_t readers = {freeze, single_readers_detached, NULL};
    if (!pet_single_store_close(&s_single))
        return false;
    bool ok = pet_single_store_open(&s_single, &readers) == ESP_OK;
    if (s_firmware)
        s_firmware->next_poll_ms = 0;
    return ok;
}
static void independent_control_loop(void)
{
    /* This is the existing control task, not a second flash writer. */
    pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL);
    atomic_store(&s_fw_reserved, true);
    const pet_single_readers_t readers = {freeze, single_readers_detached, NULL};
    if (pet_single_store_open(&s_single, &readers) != ESP_OK)
        strlcpy(s_local_error, "PET_STORAGE_RECOVERY", sizeof(s_local_error));
    pet_firmware_receipt_open_nvs(&s_fw_receipt); // Failure must not stop independent networking.
    s_firmware = heap_caps_calloc(1, sizeof(*s_firmware), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pet_firmware_control_config_t config = {.http = {s_origin, s_id, s_credential},
                                            .keys = &s_trust,
                                            .key_count = s_trust_count,
                                            .firmware_epoch = CONFIG_PET_VNEXT_FIRMWARE_EPOCH,
                                            .bootloader = s_boot_profile,
                                            .protection = firmware_protection,
                                            .health = firmware_health,
                                            .freeze = freeze,
                                            .restart = firmware_restart,
                                            .now_ms = firmware_now_ms};
    random_uuid(config.boot_id);
    const pet_replace_control_config_t pet_config = {.http = {s_origin, s_id, s_credential},
                                                     .keys = &s_trust,
                                                     .key_count = s_trust_count,
                                                     .compatible_healthy = single_compatible,
                                                     .verify = single_verify,
                                                     .activate = single_activate,
                                                     .freeze = freeze,
                                                     .now_ms = firmware_now_ms};
    pet_replace_control_config_t pet_init = pet_config;
    strcpy(pet_init.boot_id, config.boot_id);
    if (!s_firmware || !pet_firmware_control_init(s_firmware, &config, &s_fw_receipt))
    {
        free(s_firmware);
        s_firmware = NULL;
        strlcpy(s_local_error, "FIRMWARE_WORKER_START", sizeof(s_local_error));
    }
    s_pet = heap_caps_calloc(1, sizeof(*s_pet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pet && !pet_replace_control_init(s_pet, &pet_init, &s_single.state, &s_single.writer))
    {
        free(s_pet);
        s_pet = NULL;
    }
    int64_t next_status = 0, next_reopen = 0, next_pet_reopen = 15000000, next_allocate = 15000000;
    control_command_t command;
    for (;;)
    {
        int64_t now = esp_timer_get_time();
        independent_health_guard();
        if (now >= next_allocate)
        {
            if (!s_firmware)
            {
                s_firmware = heap_caps_calloc(1, sizeof(*s_firmware), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_firmware && !pet_firmware_control_init(s_firmware, &config, &s_fw_receipt))
                {
                    free(s_firmware);
                    s_firmware = NULL;
                }
            }
            if (!s_pet)
            {
                s_pet = heap_caps_calloc(1, sizeof(*s_pet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_pet && !pet_replace_control_init(s_pet, &pet_init, &s_single.state, &s_single.writer))
                {
                    free(s_pet);
                    s_pet = NULL;
                }
            }
            next_allocate = now + 15000000;
        }
        independent_updates((uint64_t)now / 1000);
        if (pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL) ||
            pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL))
        {
            if (!s_fw_receipt.loaded && now >= next_reopen)
            {
                pet_firmware_receipt_open_nvs(&s_fw_receipt);
                next_reopen = now + 15000000;
            }
            if (!single_ready() && now >= next_pet_reopen)
            {
                single_reopen();
                next_pet_reopen = now + 15000000;
            }
            if (!pet_firmware_control_blocks_pet(s_firmware))
                pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
        }
        strlcpy(s_local_error,
                !s_firmware       ? "FIRMWARE_WORKER_START"
                : !s_pet          ? "PET_WORKER_START"
                : !single_ready() ? "PET_STORAGE_RECOVERY"
                                  : "",
                sizeof(s_local_error));
        if (now >= next_status)
        {
            uint64_t retry = s_firmware ? s_firmware->retry_at_ms : 0;
            if (s_pet && s_pet->retry_at_ms > retry)
                retry = s_pet->retry_at_ms;
            publish_status((int64_t)retry * 1000);
            next_status = now + 1000000;
        }
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (command.kind == CONTROL_RESTART)
        {
            freeze(NULL);
            esp_restart();
        }
        else if (command.kind == CONTROL_RETURN)
        {
            if (single_ready() && pet_replace_has_active(&s_single.state.state) && s_active_slot == 0)
            {
                pet_battery_set_enabled(false);
                pet_face_show();
            }
        }
        else if (command.kind == CONTROL_RETRY)
        {
            uint64_t current = firmware_now_ms(NULL);
            if (s_firmware && current >= s_firmware->retry_at_ms)
                s_firmware->next_poll_ms = 0;
            if (s_pet && current >= s_pet->retry_at_ms)
                s_pet->next_poll_ms = 0;
        }
        else if (command.kind == CONTROL_OTA)
        {
            pet_network_ota_status(command.ota->release_id, "failed", 0, "OTA_V2_REQUIRED");
            pet_enrollment_clear(command.ota, sizeof(*command.ota));
            free(command.ota);
        }
        /* Legacy library/select never accesses a single-slot partition. Pet-v2
         * selection/retry authority belongs to the authenticated website. */
    }
}
#if CONFIG_PET_POCKET_TERMINAL
/* ---- Pocket Terminal: three installed pets --------------------------------
 * The pet on screen comes from local flash at boot, before Wi-Fi, enrollment
 * or cloud, and a swipe shows the next installed pet. A pet is shown from a
 * RAM copy of its slot, hashed against its record as it is copied
 * (pet_slot_store.h); its frames are validated again only when this
 * firmware's validator is newer than the one that proved them.
 *
 * Once paired, the v2 pet worker runs unchanged on the inventory's
 * `operation` (the three-slot contract): a website installation writes the
 * inventory's target, a slot neither on screen nor bound, and its activation
 * shows and binds the new pet. Only the bound pet talks, and only while it is
 * on screen. Another pet the owner swipes to stays on screen offline, and once
 * the swipe settles the device asks the cloud to select it: a signed cloud
 * pet the cloud accepts becomes the bound pet and talks with its own voice
 * and memory. The firmware worker polls and reports this device, but reports
 * its installed pets as unprotected, so no firmware update is accepted until
 * OTA protection covers every slot. */
#define SELECTION_SETTLE_US 1000000
static int64_t s_selection_save_at;
static uint8_t s_swipe_sound;
/* A legal moment to ask the cloud to talk as the pet on screen (the three-slot
 * contract's Selection): a swipe that settled, or control authenticating
 * again. A moment ends with an answer; with none, or a 429 or 5xx, it stays
 * and asks again after the worker's backoff. */
typedef enum
{
    SELECT_NONE,
    SELECT_AFTER_RECONNECT,
    SELECT_AFTER_SWIPE
} select_moment_t;
static select_moment_t s_select_moment;
static bool s_select_authenticated;
/* A final refusal, or a pet with no signed cloud release: a reconnect asks
 * again only once the pet on screen or the cloud's binding has changed. A
 * swipe always asks. */
static char s_select_refused_build[37], s_select_refused_revision[81];
/* The copy the pet on screen is drawn from: one slot's capacity in PSRAM,
 * allocated once, so showing any installed pet never needs a new allocation. */
static uint8_t *s_shown_pack;
/* The slot whose verified bytes the copy holds; -1 while it holds anything else. */
static int s_copy_slot = -1;
static bool s_pocket_boot_healthy;

/* Offline settings, until the cloud confirms this pet's configuration. */
static void local_context(const pet_pack_verified_manifest_t *manifest, pet_control_context_t *out)
{
    memset(out, 0, sizeof(*out));
    pet_control_config_t *c = &out->config;
    c->volume = s_wifi.volume;
    c->brightness = s_wifi.brightness;
    c->shake_sensitivity = s_wifi.shake_sensitivity;
    c->recording_timeout = s_wifi.recording_timeout_seconds;
    c->animation_profile = (uint8_t)s_wifi.animation_profile;
    c->speech_profile = (uint8_t)s_wifi.speech_profile;
    strlcpy(c->face_id, manifest->face_id, sizeof(c->face_id));
    strlcpy(c->ai_pet_id, manifest->face_id, sizeof(c->ai_pet_id));
}
/* Every inventory commit advances the journal generation that the v2
 * machine's receipts carry (pet_slot_store.c sync_machine). A selection or a
 * validator record is therefore written only between installations: never
 * while the machine is mid-operation or in recovery, nor while the cloud
 * reports an operation past its queue, or the device's next report would
 * contradict one the cloud already acknowledged. */
static bool pocket_inventory_writable(void)
{
    pet_replace_phase_t phase = s_slots.inventory.state.operation.phase;
    if (phase != PET_REPLACE_EMPTY && phase != PET_REPLACE_ACTIVE)
        return false;
    return !s_pet || !s_pet->cloud.has_operation || s_pet->cloud.operation.phase == PET_CLOUD_QUEUED;
}
/* Newer validator rules than the ones that proved this pack: check it once. */
static bool revalidate_slot(unsigned slot, const void *pack, size_t bytes)
{
    fp_pack_info_t info = {0};
    fp_error_t result = pet_face_pack_validate(pack, (uint32_t)bytes, &info);
    if (result != FP_OK || !info.approved)
    {
        ESP_LOGE(TAG, "pet slot %u fails validator %u: %s", slot, FP_VALIDATOR_REVISION,
                 result == FP_OK ? "unapproved" : fp_error_string(result));
        return false;
    }
    pet_slot_inventory_t next = s_slots.inventory.state;
    next.slots[slot].validator_revision = FP_VALIDATOR_REVISION;
    /* The proof holds for this boot even when it cannot be recorded now. */
    if (!pocket_inventory_writable() || pet_slot_store_commit(&s_slots, &next) != ESP_OK)
        ESP_LOGW(TAG, "pet slot %u validator revision not recorded", slot);
    return true;
}
/* "Lavender dragon" -> "LAVENDER_DRAGON": the pet's name as a terminal host. */
static void terminal_name(const uint8_t *name, size_t length, char *out, size_t capacity)
{
    size_t n = 0;
    for (size_t i = 0; i < length && n + 1 < capacity; i++)
    {
        unsigned char c = name[i];
        out[n++] = isalnum(c) ? (char)toupper(c) : '_';
    }
    out[n] = 0;
}
/* The copy is about to hold another pet: the one it holds stops talking and
 * nothing draws from it until the next binding. The screen keeps its last
 * frame meanwhile. */
static bool hold_shown(bool keep_session)
{
    /* The previous pet's voice ends before another pet shows. A swipe keeps the
     * session open for a rebind; storage work closes it. */
    if (keep_session)
        pause_voice(false);
    else
        freeze(NULL);
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    bool held = pet_display_lock(-1) == ESP_OK;
    if (held)
    {
        pet_face_pack_hold_external();
        pet_display_unlock();
        memset(&s_prepared[0], 0, sizeof(s_prepared[0]));
        s_copy_slot = -1;
    }
    xSemaphoreGive(s_audio_lock);
    return held;
}
/* Show an installed pet, without network. Control task only. */
static bool show_slot(unsigned slot, bool keep_session)
{
    const void *pack = s_shown_pack;
    size_t bytes = 0;
    fp_pack_info_t info = {0};
    if (!pack || !hold_shown(keep_session))
        return false;
    int64_t started = esp_timer_get_time();
    esp_err_t loaded = pet_slot_store_load(&s_slots, slot, s_shown_pack, PET_LAYOUT_THREE_SLOT_PACK_BYTES, &bytes);
    if (loaded != ESP_OK)
    {
        ESP_LOGE(TAG, "pet slot %u not shown: %s", slot, esp_err_to_name(loaded));
        return false;
    }
    ESP_LOGI(TAG, "pet slot %u copied and hashed: %u bytes in %lld ms", slot, (unsigned)bytes,
             (long long)((esp_timer_get_time() - started) / 1000));
    (void)started; /* Only logged. */
    s_copy_slot = (int)slot;
    if (s_slots.inventory.state.slots[slot].validator_revision != FP_VALIDATOR_REVISION &&
        !revalidate_slot(slot, pack, bytes))
        return false;
    pet_pack_verified_manifest_t manifest = {.bytes = (uint32_t)bytes};
    if (fp_inspect_prevalidated(pack, (uint32_t)bytes, &info) != FP_OK || info.id_len >= PET_FACE_ID_MAX ||
        info.version_len >= sizeof(manifest.version))
        return false;
    memcpy(manifest.face_id, info.id, info.id_len);
    memcpy(manifest.version, info.version, info.version_len);
    if (!prepare_pack(0, pack, bytes, &manifest, true, false))
        return false;
    /* A waiting tap reads the name on the audio task. */
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    terminal_name(info.name, info.name_len, s_shown_name, sizeof(s_shown_name));
    xSemaphoreGive(s_audio_lock);
    pet_control_context_t context;
    local_context(&manifest, &context);
    if (!activate(NULL, 0, &context, false))
    {
        ESP_LOGE(TAG, "pet slot %u did not start", slot);
        return false;
    }
    /* An installed pet is at home without a network: it idles, and only the
     * connection indicator reflects being offline. */
    pet_face_set_state(PET_FACE_IDLE);
    s_shown_slot = (int)slot;
    atomic_store(&s_runtime_fault, false);
    pet_battery_set_enabled(false);
    pet_face_show();
    return true;
}
/* True once the pet on screen is the saved selection. */
static bool persist_selection(void)
{
    if (s_shown_slot < 0 || s_shown_slot == s_slots.inventory.state.active)
        return true;
    if (!pocket_inventory_writable())
        return false;
    pet_slot_inventory_t next = s_slots.inventory.state;
    if (pet_slot_inventory_select(&next, (unsigned)s_shown_slot) && pet_slot_store_commit(&s_slots, &next) == ESP_OK)
        return true;
    ESP_LOGW(TAG, "pet selection not saved");
    return false;
}
static void pocket_show_active(void)
{
    esp_err_t opened = pet_slot_store_open(&s_slots);
    if (opened != ESP_OK)
    {
        ESP_LOGE(TAG, "pet store: %s", esp_err_to_name(opened));
        strlcpy(s_local_error, "PET_STORAGE_RECOVERY", sizeof(s_local_error));
        return;
    }
    if (!s_shown_pack)
        s_shown_pack =
            heap_caps_aligned_alloc(16, PET_LAYOUT_THREE_SLOT_PACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_shown_pack)
    {
        ESP_LOGE(TAG, "no PSRAM for the pet on screen");
        strlcpy(s_local_error, "PET_MEMORY", sizeof(s_local_error));
        return;
    }
    /* The recorded pet first, then any other installed pet that shows. */
    const pet_slot_inventory_t *inventory = &s_slots.inventory.state;
    for (int slot = inventory->active, tries = 0; slot >= 0 && tries < (int)PET_SLOT_COUNT; ++tries)
    {
        if (show_slot((unsigned)slot, false))
        {
            persist_selection();
            unsigned pets = 0;
            for (unsigned i = 0; i < PET_SLOT_COUNT; i++)
                pets += inventory->slots[i].state == PET_SLOT_READY;
            char line[PET_FACE_TERMINAL_MAX + 1];
            snprintf(line, sizeof(line), "> %u pet%s on board", pets, pets == 1 ? "" : "s");
            pet_face_announce(line);
            return;
        }
        int next = pet_slot_inventory_step(inventory, slot, 1);
        if (next == slot || next == inventory->active)
            return;
        slot = next;
    }
}
/* Showing another pet paused its voice. Back on the pet the cloud binds while
 * the session is still open, it talks again at once (or once a rebind to that
 * binding is accepted); otherwise the worker reconciles and it reconnects. */
static void pocket_readmit_bound(void)
{
    if (!s_pet || s_shown_slot < 0 || s_shown_slot != s_slots.inventory.state.bound)
        return;
    if (s_pet->admitted && s_pet->has_context &&
        (pet_network_bound_to(&s_pet->cloud.context) || pet_network_can_rebind()) &&
        pocket_activate(NULL, &s_pet->cloud.context))
        return;
    pet_replace_control_disconnect(s_pet);
}
static void pocket_switch(int direction)
{
    if (!s_slots.open || s_shown_slot < 0)
        return;
    const pet_slot_inventory_t *inventory = &s_slots.inventory.state;
    int from = s_shown_slot;
    bool attempted = false;
    for (int slot = pet_slot_inventory_step(inventory, from, direction), tries = 0;
         slot >= 0 && slot != from && tries < (int)PET_SLOT_COUNT;
         slot = pet_slot_inventory_step(inventory, slot, direction), ++tries)
    {
        attempted = true;
        if (show_slot((unsigned)slot, true))
        {
            /* A burst of swipes saves only the pet it ends on. */
            s_selection_save_at = esp_timer_get_time() + SELECTION_SETTLE_US;
            pocket_readmit_bound();
            pet_sfx_play(pet_sfx_for_face_swipe(s_swipe_sound++));
            /* The pet that just arrived says hello, and the terminal names it. */
            pet_face_touch_reaction();
            char line[PET_FACE_TERMINAL_MAX + 1];
            snprintf(line, sizeof(line), "> %s online", s_shown_name);
            pet_face_announce(line);
            return;
        }
    }
    /* One pet, or none of the others shows: stay with this one. */
    if (!attempted)
        return;
    if (show_slot((unsigned)from, true))
        pocket_readmit_bound();
    else
    {
        ESP_LOGE(TAG, "pet slot %d could not return", from);
        /* No pet to show: the emergency face replaces the held frame. */
        if (pet_display_lock(-1) == ESP_OK)
        {
            pet_face_pack_release_external();
            pet_display_unlock();
        }
    }
}
/* ---- Installation and voice ---- */
static pet_fw_storage_t pocket_health(void *unused)
{
    (void)unused;
    if (!pet_setup_control_ready() || !pet_network_wifi_is_ready() || !atomic_load(&s_identity_ready) || !s_firmware ||
        !s_pet)
        return PET_FW_STORAGE_NONE;
    if (s_shown_slot >= 0 && s_audio_started && pet_face_pack_ready())
        return PET_FW_STORAGE_PACK;
    if (!pet_onboarding_ui_ready())
        return PET_FW_STORAGE_NONE;
    const pet_slot_inventory_t *inventory = &s_slots.inventory.state;
    for (unsigned i = 0; i < PET_SLOT_COUNT; i++)
        if (inventory->slots[i].state != PET_SLOT_FREE)
            return PET_FW_STORAGE_RECOVERY;
    return PET_FW_STORAGE_SETUP;
}
/* A pet drawn with audio this boot, latched by pocket_tick: the open menu can
 * cover the pet screen, not undo that proof. */
static bool s_pet_drawn;
/* New firmware proves itself before the updater confirms it and so ends the
 * automatic rollback: with any pet installed, one must have been drawn with
 * audio and the runtime must not have faulted. A live UI is still required.
 * Only a store whose sole occupied slot is mid-installation is recovery. */
static pet_fw_storage_t pocket_firmware_health(void *unused)
{
    pet_fw_storage_t health = pocket_health(unused);
    if (health == PET_FW_STORAGE_NONE)
        return health;
    if (health == PET_FW_STORAGE_PACK || s_pet_drawn)
        return atomic_load(&s_runtime_fault) ? PET_FW_STORAGE_NONE : PET_FW_STORAGE_PACK;
    if (!s_slots.open || !s_slots.inventory.ready)
        return PET_FW_STORAGE_NONE;
    for (unsigned i = 0; i < PET_SLOT_COUNT; i++)
        if (s_slots.inventory.state.slots[i].state == PET_SLOT_READY)
            return PET_FW_STORAGE_NONE;
    return health;
}
/* Every slot, each proved by its signed record (pet_slot_protection.h). A pet
 * placed over USB has none: protection stays unknown and firmware updates
 * are refused until a cloud installation replaces that pet. The last
 * successful proof answers until the inventory, account or partition table
 * changes; flash actions ask for a fresh one. */
static pet_firmware_protection_t s_protection;
static uint64_t s_protection_generation;
static char s_protection_account[37], s_protection_partition[65];
static void pocket_protection(void *unused, bool fresh, pet_firmware_protection_t *out)
{
    (void)unused;
    memset(out, 0, sizeof(*out));
    /* The pet worker's scratch is safe on this serialized control task, between
     * HTTP operations; records are checked against its authenticated account. */
    if (!s_slots.open || !s_slots.inventory.ready || !s_pet || !s_pet->has_context)
        return;
    const char *account = s_pet->cloud.context.account_id;
    const uint64_t generation = s_slots.inventory.journal.generation;
    if (!fresh && s_protection.known && generation == s_protection_generation &&
        !strcmp(account, s_protection_account) && !strcmp(s_slots.partition_sha256, s_protection_partition))
    {
        *out = s_protection;
        return;
    }
    /* Only a proof is kept: a failure, perhaps a transient read, is retried. */
    pet_slot_protection(&s_slots, s_pet->chunk, account, &s_trust, s_trust_count, out);
    s_protection = *out;
    s_protection_generation = generation;
    strlcpy(s_protection_account, account, sizeof(s_protection_account));
    strlcpy(s_protection_partition, s_slots.partition_sha256, sizeof(s_protection_partition));
}
/* As single_compatible, on the three-pet layout. */
static bool pocket_compatible(void *unused, const pet_release_v2_t *release)
{
    (void)unused;
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
    if (!s_firmware || !s_firmware->authenticated || !s_firmware->next_poll_ms || now >= s_firmware->next_poll_ms ||
        !pet_setup_control_ready() || !pet_network_wifi_is_ready() || !atomic_load(&s_identity_ready))
        return false;
    pet_firmware_boot_state_t boot;
    if (!s_slots.open || !CONFIG_PET_VNEXT_FIRMWARE_EPOCH || !pet_firmware_image_bootloader_matches(&s_boot_profile) ||
        !pet_firmware_image_boot_state(&boot) ||
        !pet_ota_selection_is(&boot.selection, boot.running_slot, PET_OTA_STATE_VALID) ||
        boot.layout.id != s_slots.layout.id || strcmp(boot.partition_sha256, s_slots.partition_sha256))
        return false;
    pet_firmware_requirements_t observed = {.layout = boot.layout,
                                            .firmware_epoch = CONFIG_PET_VNEXT_FIRMWARE_EPOCH,
                                            .formats = 6,
                                            .codecs = 0xff,
                                            .imported_release = true,
                                            .bootloader = s_boot_profile};
    strcpy(observed.partition_sha256, boot.partition_sha256);
    if (!pet_release_v2_compatible(release, &observed))
        return false;
    const pet_replace_pack_t *replacement = release->has_replacement_target ? &release->replacement_target : NULL;
    if (!pet_slot_store_prepare_replacement(&s_slots, &release->pack, replacement))
        return false;
    if (!s_pocket_boot_healthy)
    {
        if (pocket_health(NULL) == PET_FW_STORAGE_NONE)
            return false;
        s_pocket_boot_healthy = true;
    }
    return true;
}
/* A downloaded pet is checked in the copy the screen draws from, never from
 * flash (pet_slot_store.h). The pet on screen holds its frame meanwhile and is
 * copied back afterwards. */
static bool pocket_verify(void *unused, const pet_release_v2_t *release)
{
    (void)unused;
    if (!s_shown_pack || !pocket_compatible(NULL, release))
        return false;
    int shown = s_shown_slot;
    size_t bytes = 0;
    bool ok = false;
    if (!hold_shown(false))
        return false;
    if (pet_slot_store_load_target(&s_slots, s_shown_pack, PET_LAYOUT_THREE_SLOT_PACK_BYTES, &bytes) == ESP_OK)
    {
        uint32_t size = fp_validation_workspace_size();
        void *workspace = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = workspace && pet_release_v2_validate_pack(release, s_shown_pack, bytes, workspace, size, NULL);
        heap_caps_free(workspace);
    }
    if (!ok)
        ESP_LOGE(TAG, "downloaded pet failed verification");
    if (shown >= 0 && !show_slot((unsigned)shown, false))
        ESP_LOGE(TAG, "pet slot %d could not return after verification", shown);
    return ok;
}
/* The cloud's binding is the conversation. It talks only on screen: after an
 * installation that is the new pet, which the commit made the selection; a
 * pet the owner swiped to instead stays on screen, offline, until a selection
 * rebinds the conversation. */
static bool pocket_activate(void *unused, const pet_control_context_t *cloud)
{
    (void)unused;
    const pet_slot_inventory_t *inventory = &s_slots.inventory.state;
    const pet_replace_t *state = &inventory->operation;
    int bound = inventory->bound;
    if (!cloud || bound < 0 || bound >= (int)PET_SLOT_COUNT || state->phase != PET_REPLACE_ACTIVE || !s_pet ||
        !s_pet->has_context || !s_pet->authenticated || strcmp(cloud->device_id, s_id) ||
        strcmp(cloud->account_id, s_pet->cloud.context.account_id) || !cloud->binding.assigned ||
        strcmp(cloud->binding.revision, state->binding_revision) ||
        strcmp(cloud->binding.relationship_id, state->relationship_id) ||
        strcmp(cloud->binding.build_id, state->active.build_id) ||
        strcmp(cloud->binding.sha256, state->active.sha256) || inventory->slots[bound].state != PET_SLOT_READY ||
        strcmp(inventory->slots[bound].pack.sha256, state->active.sha256))
        return false;
    /* The slot's signed record proves which release it holds. The worker's
     * scratch is safe here, on the control task between HTTP operations. */
    pet_release_v2_t release;
    if (pet_slot_store_read_record(&s_slots, (unsigned)bound, s_pet->chunk, PET_REPLACE_MANIFEST_BYTES) != ESP_OK ||
        !pet_release_v2_record_verify(s_pet->chunk, PET_REPLACE_MANIFEST_BYTES, cloud->account_id, &state->active,
                                      &s_trust, s_trust_count, &release) ||
        strcmp(release.face_id, cloud->config.face_id))
        return false;
    /* A swipe not saved yet is the owner's choice too. */
    if (s_shown_slot >= 0 && s_shown_slot != bound && (s_selection_save_at || inventory->active != bound))
        return true;
    if ((s_shown_slot != bound || s_copy_slot != bound) && !show_slot((unsigned)bound, true))
        return false;
    /* The copy was hashed against the slot's record, which holds this release. */
    pet_pack_verified_manifest_t manifest = {.bytes = release.pack.bytes};
    strcpy(manifest.account_id, release.account_id);
    strcpy(manifest.project_id, release.project_id);
    strcpy(manifest.face_id, release.face_id);
    strcpy(manifest.version, release.version);
    if (!prepare_pack(0, s_shown_pack, release.pack.bytes, &manifest, true, release.imported) ||
        !activate(NULL, 0, cloud, true))
        return false;
    atomic_store(&s_runtime_fault, false);
    pet_battery_set_enabled(false);
    pet_face_show();
    return true;
}
/* The worker's hooks for a selection (pet_replace_control.h): pause keeps the
 * session open, and mount moves it to the new binding over that session
 * (session-rebind-v1). Without a session that can rebind, mount freezes and
 * activates as before. */
static void pocket_pause(void *unused)
{
    (void)unused;
    pause_voice(true);
}
static bool pocket_rebind(void *unused, const pet_control_context_t *cloud)
{
    (void)unused;
    if (!pet_network_can_rebind())
        return false;
    pause_voice(true);
    return pocket_activate(NULL, cloud);
}
/* The pet in `slot` cannot talk on this account: it has no signed cloud release
 * (placed over USB), or the cloud refused selecting it for the current binding. */
static bool pocket_unlinked(unsigned slot)
{
    return slot < PET_SLOT_COUNT && s_select_refused_build[0] && s_pet && s_pet->has_context &&
           !strcmp(s_select_refused_build, s_slots.inventory.state.slots[slot].pack.build_id) &&
           !strcmp(s_select_refused_revision, s_pet->cloud.context.binding.revision);
}
/* Before the installer writes its target, nothing may hold that slot. The
 * screen only holds a copy, of another pet; if the owner swiped to the pet
 * being replaced, the inventory's own choice shows instead. */
static bool pocket_detach(void *unused)
{
    (void)unused;
    const pet_slot_inventory_t *inventory = &s_slots.inventory.state;
    int target = inventory->target;
    if (target < 0)
        return false;
    if (target != s_shown_slot)
        return true;
    s_selection_save_at = 0;
    if (inventory->active >= 0 && inventory->active != target && show_slot((unsigned)inventory->active, false))
        return true;
    /* Updating the only pet on screen: hold the display until the new one is ready. */
    if (!hold_shown(false))
        return false;
    s_shown_slot = -1;
    pet_face_announce("> UPDATING PET");
    return true;
}
/* Library deletion is an idle journal transaction, never an erase. A replay
 * after commit but before detachment must still evict the stale RAM copy. */
static bool pocket_remove(void *unused, const pet_replace_pack_t *pack)
{
    (void)unused;
    if (!pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL) || !s_slots.open || !s_slots.inventory.ready ||
        !s_pet || !s_pet->authenticated || s_pet->cloud.has_operation ||
        pet_firmware_control_blocks_pet(s_firmware) ||
        (s_pet->cloud.context.binding.assigned && !strcmp(s_pet->cloud.context.binding.build_id, pack->build_id)))
        return false;
    pet_slot_inventory_t next = s_slots.inventory.state;
    int target = pet_slot_inventory_find(&next, pack);
    bool removed_bound = target >= 0 && target == next.bound;
    if (!pet_slot_inventory_remove(&next, pack))
        return false;
    if (target >= 0)
    {
        if (target == s_shown_slot || target == s_slots.inventory.state.bound)
        {
            freeze(NULL);
            s_pet->admitted = false;
        }
        if (pet_slot_store_commit(&s_slots, &next) != ESP_OK)
            return false;
    }
    if (removed_bound && s_shown_slot >= 0)
        s_select_moment = SELECT_AFTER_SWIPE;
    if (s_shown_slot < 0 || s_slots.inventory.state.slots[s_shown_slot].state == PET_SLOT_READY)
        return true;
    s_selection_save_at = 0;
    s_pet->admitted = false;
    if (next.active >= 0)
    {
        if (!show_slot((unsigned)next.active, false))
            return false;
        s_select_moment = SELECT_AFTER_SWIPE;
        return true;
    }
    freeze(NULL);
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    bool detached = pet_display_lock(-1) == ESP_OK;
    if (detached)
    {
        detached = pet_onboarding_setup_ready();
        if (detached)
        {
            pet_face_pack_release_external();
            memset(s_prepared, 0, sizeof(s_prepared));
            memset(&s_applied, 0, sizeof(s_applied));
            s_shown_slot = s_copy_slot = s_active_slot = -1;
            s_select_moment = SELECT_NONE;
            pet_onboarding_open_from_ui();
        }
        pet_display_unlock();
    }
    xSemaphoreGive(s_audio_lock);
    return detached;
}
/* The pet on screen talks once the cloud binds it. At a legal moment, between
 * installations, ask to select it when it is not the pet the cloud and the
 * inventory bind. On success the inventory rebinds that slot to the cloud's
 * binding; the worker's next step then mounts it, and pocket_activate checks
 * its signed record against that binding before any voice. After a final
 * refusal the pet stays on screen without voice until the next legal moment;
 * with no answer, or a 429 or 5xx, the moment asks again after the backoff. */
static void pocket_select(uint64_t now_ms)
{
    const pet_slot_inventory_t *inventory = &s_slots.inventory.state;
    int slot = s_shown_slot;
    if (s_select_moment == SELECT_NONE)
        return;
    /* Not yet: a swipe still settling, an installation in flight, offline or backing off. */
    if (!s_slots.open || !s_slots.inventory.ready || s_selection_save_at || slot < 0 || slot >= (int)PET_SLOT_COUNT ||
        inventory->slots[slot].state != PET_SLOT_READY || inventory->target >= 0 ||
        (inventory->operation.phase != PET_REPLACE_EMPTY && inventory->operation.phase != PET_REPLACE_ACTIVE) ||
        !s_pet || !s_pet->has_context || !s_pet->authenticated || now_ms < s_pet->retry_at_ms ||
        s_pet->cloud.removal_count || s_pet->removal_ack_count ||
        pet_firmware_control_blocks_pet(s_firmware) ||
        (s_pet->cloud.has_operation && s_pet->cloud.operation.phase != PET_CLOUD_INSTALLED &&
         s_pet->cloud.operation.phase != PET_CLOUD_CANCELLED && s_pet->cloud.operation.phase != PET_CLOUD_SUPERSEDED))
        return;
    const pet_replace_pack_t *pack = &inventory->slots[slot].pack;
    const pet_control_binding_t *cloud = &s_pet->cloud.context.binding;
    const pet_replace_t *local = &inventory->operation;
    if (slot == inventory->bound && cloud->assigned && !strcmp(cloud->build_id, pack->build_id) &&
        !strcmp(cloud->sha256, pack->sha256) && !strcmp(cloud->revision, local->binding_revision) &&
        !strcmp(cloud->relationship_id, local->relationship_id))
    {
        s_select_moment = SELECT_NONE;
        return; /* Already the conversation. */
    }
    if (s_select_moment == SELECT_AFTER_RECONNECT && !strcmp(s_select_refused_build, pack->build_id) &&
        !strcmp(s_select_refused_revision, cloud->revision))
    {
        s_select_moment = SELECT_NONE;
        return;
    }
    /* A turn of the bound pet still holds voice: ask after it. */
    if (!pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL) && !pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL))
        return;
    /* Only a signed cloud release can be selected. A pet placed over USB has
     * no record the cloud knows (the three-slot contract's open decision). */
    pet_release_v2_t release;
    pet_replace_select_t result = PET_REPLACE_SELECT_REFUSED;
    int http = 0;
    int64_t took_ms = 0;
    pet_control_http_timing_t timing = {0};
    if (pet_slot_store_read_record(&s_slots, (unsigned)slot, s_pet->chunk, PET_REPLACE_MANIFEST_BYTES) == ESP_OK &&
        pet_release_v2_record_verify(s_pet->chunk, PET_REPLACE_MANIFEST_BYTES, s_pet->cloud.context.account_id, pack,
                                     &s_trust, s_trust_count, &release))
    {
        int64_t asked = esp_timer_get_time();
        result = pet_replace_control_select(s_pet, now_ms, pack, &http);
        took_ms = (esp_timer_get_time() - asked) / 1000;
        timing = pet_control_http_last_timing();
        if (result != PET_REPLACE_SELECT_CHOSEN)
            ESP_LOGW(TAG, "pet slot %d not selected: HTTP %d after %lld ms", slot, http, (long long)took_ms);
    }
    else
        ESP_LOGW(TAG, "pet slot %d has no signed cloud release: it shows without voice", slot);
    (void)took_ms;
    (void)timing; /* Only logged. */
    if (result == PET_REPLACE_SELECT_CHOSEN)
    {
        const pet_control_context_t *chosen = &s_pet->cloud.context;
        pet_slot_inventory_t next = *inventory;
        s_select_moment = SELECT_NONE;
        s_select_refused_build[0] = 0;
        if (pet_slot_inventory_rebind(&next, (unsigned)slot, chosen->binding.revision, chosen->binding.relationship_id,
                                      chosen->config.version) &&
            pet_slot_store_commit(&s_slots, &next) == ESP_OK)
            ESP_LOGI(TAG, "pet slot %d is the conversation now (select %lld ms: connect %lu ms%s, request %lu ms)",
                     slot, (long long)took_ms, (unsigned long)timing.connect_ms,
                     timing.reused ? ", kept connection" : "", (unsigned long)timing.total_ms);
        /* Unrecorded, the worker reports the mismatch and its reconnect asks again. */
        else
            ESP_LOGE(TAG, "pet slot %d selection not recorded", slot);
    }
    else if (result == PET_REPLACE_SELECT_REFUSED)
    {
        /* Final, and the cloud state unchanged: a reconnect asks again only once
         * something changed. Otherwise (no answer, 429 or 5xx) the moment stays
         * for after the backoff. */
        s_select_moment = SELECT_NONE;
        strlcpy(s_select_refused_build, pack->build_id, sizeof(s_select_refused_build));
        strlcpy(s_select_refused_revision, cloud->revision, sizeof(s_select_refused_revision));
    }
    if (!pet_firmware_control_blocks_pet(s_firmware))
        pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
}
/* Both workers need the paired identity; installed pets show without it. */
static void pocket_start_workers(pet_firmware_control_config_t *firmware, pet_replace_control_config_t *pet)
{
    if (!s_firmware)
    {
        s_firmware = heap_caps_calloc(1, sizeof(*s_firmware), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_firmware && !pet_firmware_control_init(s_firmware, firmware, &s_fw_receipt))
        {
            free(s_firmware);
            s_firmware = NULL;
        }
    }
    if (!s_pet && s_slots.installer)
    {
        s_pet = heap_caps_calloc(1, sizeof(*s_pet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pet && !pet_replace_control_init(s_pet, pet, &s_slots.machine, &s_slots.writer))
        {
            free(s_pet);
            s_pet = NULL;
        }
    }
}
/* The workers' configuration; they start once the device is paired. */
static pet_firmware_control_config_t s_pocket_firmware;
static pet_replace_control_config_t s_pocket_pet;
static int64_t s_pocket_next_status, s_pocket_next_allocate, s_pocket_next_reopen, s_pocket_next_store_reopen;
/* A failed journal write leaves the store refusing commits: reopen it as
 * single_reopen does. The pet on screen is a RAM copy and keeps showing. */
static bool pocket_reopen(void)
{
    if (!pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL))
        return false;
    pet_replace_control_disconnect(s_pet);
    if (s_pet)
    {
        s_pet->manifest_ready = s_pet->verified = false;
        s_pet->next_poll_ms = 0;
    }
    const pet_slot_readers_t readers = {.freeze = freeze, .detach = pocket_detach};
    bool ok = pet_slot_store_open(&s_slots) == ESP_OK && pet_slot_store_attach_installer(&s_slots, &readers) == ESP_OK;
    if (s_firmware)
        s_firmware->next_poll_ms = 0;
    return ok;
}
static void pocket_control_start(void)
{
    pocket_show_active();
    /* Each worker pass claims control itself (independent_updates). */
    const pet_slot_readers_t readers = {.freeze = freeze, .detach = pocket_detach};
    if (s_slots.open && pet_slot_store_attach_installer(&s_slots, &readers) != ESP_OK)
        ESP_LOGE(TAG, "pet installer did not start");
    pet_firmware_receipt_open_nvs(&s_fw_receipt); /* Failure must not stop the pets or networking. */
    s_pocket_firmware = (pet_firmware_control_config_t){.http = {s_origin, s_id, s_credential},
                                                        .keys = &s_trust,
                                                        .key_count = s_trust_count,
                                                        .firmware_epoch = CONFIG_PET_VNEXT_FIRMWARE_EPOCH,
                                                        .bootloader = s_boot_profile,
                                                        .protection = pocket_protection,
                                                        .health = pocket_firmware_health,
                                                        .freeze = freeze,
                                                        .restart = firmware_restart,
                                                        .now_ms = firmware_now_ms};
    random_uuid(s_pocket_firmware.boot_id);
    s_pocket_pet = (pet_replace_control_config_t){.http = {s_origin, s_id, s_credential},
                                                  .keys = &s_trust,
                                                  .key_count = s_trust_count,
                                                  .compatible_healthy = pocket_compatible,
                                                  .verify = pocket_verify,
                                                  .activate = pocket_activate,
                                                  .freeze = freeze,
                                                  .pause = pocket_pause,
                                                  .rebind = pocket_rebind,
                                                  .remove = pocket_remove,
                                                  .now_ms = firmware_now_ms};
    strcpy(s_pocket_pet.boot_id, s_pocket_firmware.boot_id);
    s_pocket_next_status = s_pocket_next_allocate = 0;
    s_pocket_next_reopen = s_pocket_next_store_reopen = 15000000;
}
/* One pass of the control task, between commands. */
static void pocket_tick(int64_t now)
{
    independent_health_guard();
    if (!s_pet_drawn && s_shown_slot >= 0 && s_audio_started && pet_face_pack_ready())
        s_pet_drawn = true;
    if (atomic_load(&s_identity_ready) && now >= s_pocket_next_allocate)
    {
        pocket_start_workers(&s_pocket_firmware, &s_pocket_pet);
        s_pocket_next_allocate = now + 15000000;
    }
    if (!s_fw_receipt.loaded && now >= s_pocket_next_reopen)
    {
        pet_firmware_receipt_open_nvs(&s_fw_receipt);
        s_pocket_next_reopen = now + 15000000;
    }
    /* A swipe is saved once it settles, and at once before a queued
     * installation picks its target, so the target is never the pet on screen.
     * During an installation it waits (pocket_inventory_writable). */
    bool queued = s_pet && s_pet->cloud.has_operation && s_pet->cloud.operation.phase == PET_CLOUD_QUEUED;
    if (s_selection_save_at && (now >= s_selection_save_at || queued) && pocket_inventory_writable() &&
        persist_selection())
    {
        s_selection_save_at = 0;
        s_select_moment = SELECT_AFTER_SWIPE; /* The swipe settled: ask for its pet. */
    }
    if ((s_store_lost || (s_slots.open && !s_slots.inventory.ready)) && now >= s_pocket_next_store_reopen &&
        (pet_runtime_owns(&s_resources, PET_RESOURCE_CONTROL) || pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL)))
    {
        s_store_lost = !pocket_reopen();
        if (s_store_lost)
            ESP_LOGW(TAG, "pet store did not reopen");
        if (!pet_firmware_control_blocks_pet(s_firmware))
            pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
        s_pocket_next_store_reopen = now + 15000000;
    }
    if (s_firmware || s_pet)
    {
        pet_replace_phase_t before = s_slots.inventory.state.operation.phase;
        independent_updates((uint64_t)now / 1000);
        /* The commit makes the new pet the selection (pet_slot_inventory_apply);
         * a swipe made during the installation is older and is dropped. */
        if (before == PET_REPLACE_ACTIVATING && s_slots.inventory.state.operation.phase == PET_REPLACE_ACTIVE)
            s_selection_save_at = 0;
    }
    /* Control authenticating again is the other legal moment to ask. */
    bool authenticated = s_pet && s_pet->authenticated;
    if (authenticated && !s_select_authenticated && s_select_moment == SELECT_NONE)
        s_select_moment = SELECT_AFTER_RECONNECT;
    s_select_authenticated = authenticated;
    pocket_select((uint64_t)now / 1000);
    if (now >= s_pocket_next_status)
    {
        if (!atomic_load(&s_identity_ready) && pet_setup_identity(s_id, s_credential))
            atomic_store(&s_identity_ready, true);
        uint64_t retry = s_firmware ? s_firmware->retry_at_ms : 0;
        if (s_pet && s_pet->retry_at_ms > retry)
            retry = s_pet->retry_at_ms;
        publish_status((int64_t)retry * 1000);
        s_pocket_next_status = now + 1000000;
    }
}
static void pocket_command(control_command_t *command)
{
    if (command->kind == CONTROL_SWITCH)
    {
        /* The latest swipe: those made since this command was queued replaced it. */
        int direction = atomic_exchange(&s_swipe, 0);
        if (direction)
            pocket_switch(direction);
    }
    /* A level the owner settled on in the menu: every pet starts with it.
     * Applying it again covers a live change the audio queue had no room for. */
    else if (command->kind == CONTROL_KEEP_VOLUME)
    {
        s_wifi.volume = command->value;
        pet_audio_set_volume((uint8_t)atomic_load(&s_volume_now));
        if (pet_config_store_volume(command->value) != ESP_OK)
            ESP_LOGW(TAG, "volume not saved");
    }
    else if (command->kind == CONTROL_KEEP_BRIGHTNESS)
    {
        s_wifi.brightness = command->value;
        uint8_t now = (uint8_t)atomic_load(&s_brightness_now);
        if (pet_face_set_brightness(now) != ESP_OK)
            bsp_display_brightness_set(now);
        if (pet_config_store_brightness(command->value) != ESP_OK)
            ESP_LOGW(TAG, "brightness not saved");
    }
    else if (command->kind == CONTROL_RESTART)
    {
        persist_selection();
        freeze(NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    else if (command->kind == CONTROL_RETRY)
    {
        uint64_t current = firmware_now_ms(NULL);
        if (s_firmware && current >= s_firmware->retry_at_ms)
            s_firmware->next_poll_ms = 0;
        if (s_pet && current >= s_pet->retry_at_ms)
            s_pet->next_poll_ms = 0;
    }
    else if (command->kind == CONTROL_OTA)
    {
        pet_network_ota_status(command->ota->release_id, "failed", 0, "OTA_V2_REQUIRED");
        pet_enrollment_clear(command->ota, sizeof(*command->ota));
        free(command->ota);
    }
    /* Pets arrive through the website's installation, not a device library. */
}
/* One pass of the control task: its cloud steps, then the next command. */
static void pocket_control_pass(void)
{
    atomic_store(&s_control_busy_since, (unsigned)(esp_timer_get_time() / 1000) | 1u);
    pocket_tick(esp_timer_get_time());
    atomic_store(&s_control_busy_since, 0);
    control_command_t command;
    if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(100)) == pdTRUE)
        pocket_command(&command);
}
static void pocket_control_loop(void)
{
    pocket_control_start();
    for (;;)
        pocket_control_pass();
}
#endif

static void control_task(void *unused)
{
    (void)unused;
#if CONFIG_PET_POCKET_TERMINAL
    pocket_control_loop();
#endif
    while (!pet_setup_identity(s_id, s_credential))
    {
        independent_health_guard();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    atomic_store(&s_identity_ready, true);
    if (s_fw_lifecycle)
    {
        independent_control_loop();
        return;
    }
    if (pet_asset_store_open(&s_store) != ESP_OK)
    {
        strlcpy(s_local_error, "ASSET_STORAGE_LAYOUT", sizeof(s_local_error));
        publish_status(0);
        vTaskDelete(NULL);
        return;
    }
    s_sync = calloc(1, sizeof(*s_sync));
    const pet_control_http_t http = {s_origin, s_id, s_credential};
    const pet_sync_runtime_t runtime = {freeze, prepare, activate, release_slot, busy, healthy, NULL};
    if (!s_sync ||
        !pet_sync_init(s_sync, &s_store, &http, &s_trust, s_trust_count, &runtime, esp_app_get_description()->version))
    {
        strlcpy(s_local_error, "CONTROL_WORKER_START", sizeof(s_local_error));
        publish_status(0);
        vTaskDelete(NULL);
        return;
    }
    pet_sync_show_cached(s_sync);
    int64_t next_poll = 0, retry_not_before = 0, next_status = 0;
    control_command_t command;
    for (;;)
    {
        int64_t now = esp_timer_get_time();
        brain_disconnect_step();
        if (atomic_load(&s_handshake_pending) && now >= s_handshake_deadline)
            revalidate("handshake timed out");
        if (s_ota_reserved && !pet_ota_busy())
        {
            s_ota_reserved = false;
            pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
            revalidate("firmware update finished");
        }
        if (atomic_exchange(&s_revalidate, false))
        {
            freeze(NULL);
            s_sync->admitted = false;
            next_poll = 0;
        }
        bool online = pet_setup_control_ready() && pet_network_wifi_is_ready();
        if (!online)
        {
            atomic_store(&s_control_healthy, false);
            if (s_sync->admitted)
            {
                freeze(NULL);
                s_sync->admitted = false;
            }
            next_poll = 0;
        }
        if (online && !busy(NULL) && now >= next_poll && now >= retry_not_before &&
            pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL))
        {
            atomic_store(&s_control_healthy, false);
            pet_sync_step(s_sync);
            next_poll = esp_timer_get_time() + (int64_t)s_sync->retry_seconds * 1000000;
            retry_not_before =
                s_sync->http_status == 429 || s_sync->http_status == 503 || s_sync->http_status == 0 ? next_poll : 0;
            pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
            publish_status(retry_not_before);
            next_status = esp_timer_get_time() + 1000000;
        }
        now = esp_timer_get_time();
        if (online && s_sync->admitted && !busy(NULL) &&
            pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL))
        {
            brain_maintenance(&s_sync->cloud, (uint64_t)now / 1000);
            pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
        }
        if (now >= next_status)
        {
            publish_status(retry_not_before);
            next_status = now + 1000000;
        }
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (command.kind == CONTROL_RETURN)
        {
            pet_battery_set_enabled(false);
            if (s_face_started)
                pet_face_show();
        }
        else if (command.kind == CONTROL_RESTART)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        else if (command.kind == CONTROL_RETRY)
        {
            /* Retry is only a request to run at the next legal checkpoint. It
             * never shortens a server/transport Retry-After window. */
            if (esp_timer_get_time() >= retry_not_before)
                next_poll = 0;
            publish_status(retry_not_before);
        }
        else if (command.kind == CONTROL_OTA)
        {
            if (online && s_store.state.phase == PET_INSTALL_IDLE && !busy(NULL) &&
                pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL))
            {
                /* Retain the authenticated socket for OTA progress receipts;
                 * only voice is suspended while the OTA worker owns flash. */
                atomic_store(&s_voice_allowed, false);
                if (s_face_started)
                    pet_face_set_state(PET_FACE_CONNECTING);
                if (pet_ota_start(command.ota) == ESP_OK)
                    s_ota_reserved = true;
                else
                {
                    pet_network_ota_status(command.ota->release_id, "failed", 0, "OTA_BUSY_OR_REJECTED");
                    pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
                    revalidate("firmware update refused");
                }
            }
            else
                pet_network_ota_status(command.ota->release_id, "failed", 0, "OTA_BUSY_OR_REJECTED");
            pet_enrollment_clear(command.ota, sizeof(*command.ota));
            free(command.ota);
        }
        else if (online && !busy(NULL) && esp_timer_get_time() >= retry_not_before &&
                 pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL))
        {
            if (command.kind == CONTROL_LIBRARY)
            {
                char cursor[513];
                strlcpy(cursor, command.next ? s_library.next_cursor : "", sizeof(cursor));
                if (pet_sync_library(s_sync, cursor, &s_library))
                    pet_onboarding_set_library(&s_library);
                else
                {
                    retry_not_before = esp_timer_get_time() + (int64_t)s_sync->retry_seconds * 1000000;
                    publish_status(retry_not_before);
                }
            }
            else if (command.kind == CONTROL_SELECT)
            {
                for (size_t i = 0; i < s_library.count; ++i)
                    if (!strcmp(command.build_id, s_library.items[i].build_id))
                    {
                        char request[37];
                        random_uuid(request);
                        if (pet_sync_select(s_sync, &s_library.items[i], request))
                            next_poll = 0;
                        break;
                    }
            }
            pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
        }
        else
            publish_status(retry_not_before);
    }
}

static void recover(pet_diagnostics_recovery_t reason)
{
    (void)reason;
    revalidate("health recovery");
}

esp_err_t pet_vnext_start(const pet_config_t *wifi, const char *origin)
{
    if (!wifi || !origin || s_commands)
        return ESP_ERR_INVALID_ARG;
    /* Like an empty origin, an invalid one leaves the device offline. */
    if (origin[0] && !pet_session_wire_origin(origin, NULL, 0, NULL))
    {
        ESP_LOGE("pet_vnext", "BACKEND_ORIGIN_INVALID: %s", origin);
        origin = "";
    }
    s_wifi = *wifi;
    s_origin = origin;
    s_mouth_offset_kept = s_wifi.has_speech_mouth_offset;
    s_mouth_offset_kept_ms = s_wifi.speech_mouth_offset_ms;
    pet_vnext_independent_control();
    if (strlen(CONFIG_PET_VNEXT_BOOTLOADER_SHA256) == 64 &&
        strspn(CONFIG_PET_VNEXT_BOOTLOADER_SHA256, "0123456789abcdef") == 64 &&
        CONFIG_PET_VNEXT_BOOTLOADER_BYTES >= 4096 && CONFIG_PET_VNEXT_BOOTLOADER_BYTES <= 0x8000)
    {
        s_boot_profile.bytes = CONFIG_PET_VNEXT_BOOTLOADER_BYTES;
        strcpy(s_boot_profile.sha256, CONFIG_PET_VNEXT_BOOTLOADER_SHA256);
    }
    /* The public release key is a build input, never downloaded.
     * An empty/invalid override leaves every private pack untrusted. */
    const char *hex = CONFIG_PET_VNEXT_RELEASE_PUBLIC_KEY;
    if (strlen(CONFIG_PET_VNEXT_RELEASE_KEY_ID) > 0 && strlen(hex) == 130 &&
        strspn(hex, "0123456789abcdefABCDEF") == 130)
    {
        s_trust.key_id = CONFIG_PET_VNEXT_RELEASE_KEY_ID;
        for (unsigned i = 0; i < 65; ++i)
        {
            char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
            s_trust.public_point[i] = (uint8_t)strtoul(pair, NULL, 16);
        }
        if (s_trust.public_point[0] == 4)
            s_trust_count = 1;
    }
#if CONFIG_PET_POCKET_TERMINAL
    atomic_store(&s_volume_now, s_wifi.volume <= 100 ? s_wifi.volume : 100);
    atomic_store(&s_brightness_now, s_wifi.brightness < PET_BRIGHTNESS_MIN ? PET_BRIGHTNESS_MIN
                                    : s_wifi.brightness > 100              ? 100
                                                                           : s_wifi.brightness);
    pet_audio_set_volume((uint8_t)atomic_load(&s_volume_now));
#endif
    s_commands = xQueueCreate(8, sizeof(control_command_t));
    s_audio_events = xQueueCreate(16, sizeof(audio_event_t));
    s_audio_lock = xSemaphoreCreateMutex();
    if (!s_commands || !s_audio_events || !s_audio_lock)
        return ESP_ERR_NO_MEM;
    const pet_onboarding_control_t controls = {
        .library = library_requested,
        .select = selection_requested,
        .return_to_pet = return_requested,
        .retry_sync = retry_requested,
        .restart = restart_requested,
#if CONFIG_PET_POCKET_TERMINAL
        .volume = volume_requested,
        .brightness = brightness_requested,
#endif
    };
    pet_onboarding_bind_control(&controls);
    esp_err_t result = pet_onboarding_start(wifi, origin);
    if (result != ESP_OK)
        return result;
    const pet_network_callbacks_t management = {
        .config_v2_received = config_received,
        .speech_preference_received = speech_received,
    };
    result = pet_network_start_management(&management);
    if (result != ESP_OK)
        return result;
    char health_id[37] = {0}, health_credential[44] = {0};
    bool already_enrolled = pet_setup_identity(health_id, health_credential);
    pet_enrollment_clear(health_id, sizeof(health_id));
    pet_enrollment_clear(health_credential, sizeof(health_credential));
    if (!s_fw_lifecycle && !already_enrolled && pet_onboarding_setup_ready())
    {
        const pet_ota_health_evidence_t evidence = {
            .native_ui_ready = true,
            .setup_service_ready = true,
            .storage_ready = false,
            .authenticated_control = false,
            .pet_runtime_ready = false,
        };
        pet_ota_confirm_health_stage(PET_OTA_HEALTH_PRE_ENROLLMENT, &evidence);
    }
    result = pet_battery_start(battery_updated);
    s_battery_started = result == ESP_OK;
    if (s_battery_started)
        pet_battery_set_enabled(true);
    if (pet_diagnostics_start(recover) != ESP_OK)
        return ESP_FAIL;
    if (xTaskCreate(audio_task, "pet_vnext_audio", 8192, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(control_task, "pet_vnext_sync", 24576, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
