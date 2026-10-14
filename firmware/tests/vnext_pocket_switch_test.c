/* Pocket Terminal: switching pets over the open voice session
 * (session-rebind-v1). A swipe pauses voice and keeps the socket; a selection
 * moves the session with a rebind instead of a new socket; the VOICE tile says
 * CONNECTING, not NOT LINKED, while that happens; a tap while connecting waits
 * calmly and listens once the session is ready; a refusal, a timeout or an
 * older gateway falls back to today's reconnect; the new pet's configuration
 * after the rebind applies as after a hello.
 *
 * Listening on the open session: a tap during a control step waits for it and
 * listens, however long the step (up to 10 s); control leaves the microphone
 * free for 8 s after a turn; nothing listens without a tap; a listen that
 * cannot be sent says so. The runtime, inventory, pack rules and frame player
 * are real; signed records, flash, the pet worker's HTTP and the socket are
 * seams.
 *
 * Story events (story-event-v1): the cache downloads packs between turns into
 * the unused tail of the pet's copy, keeps, forgets and adds them as the list
 * and the copy change, and refuses a pack that is not exactly the one named; a
 * cued event shows once the speech has drained, and a touch, a new reply, the
 * microphone, a rebind or a pet switch ends it. Every cue ends with
 * story.event.ended (story-reaction-v1). In story mode a tap holds the pet's
 * answer and the next lets it play on (speech-pause-v1); with a gateway that
 * keeps it (speech-pause-listen-v1) the tap listens over it instead, and the
 * answer plays on if nothing is said; a double tap stops it. The cache and
 * the pack checks are real; the renderer, the speaker and the download are
 * seams. */
#include <assert.h>
#include "../main/pet_vnext.c"
#include "mbedtls/sha256.h"

static uint8_t pack_bytes[200000];
static size_t pack_length;
static pet_slot_inventory_t stored;
static pet_replace_control_t pet_worker;
static pet_firmware_control_t firmware_worker;
static pet_onboarding_status_t visible_status;
static pet_release_v2_t signed_release;
static unsigned unsigned_slots;
static uint64_t clock_us=100000000;
static pet_face_state_t face_state=PET_FACE_BOOTING;
static char announced[PET_FACE_TERMINAL_MAX+1];
static pet_sfx_t last_sound=PET_SFX_COUNT;
static unsigned sounds,switch_sounds,error_cues,listen_cues,capture_starts,input_starts,cancels,freezes,starts,rebinds,disconnects,selects;
static uint32_t last_cancel, previous_cancel;
static bool capturing,playing,drawing_copy,wifi_ok=true;
/* The face while a swipe copies the next pack (0.4-0.9 s on the Pocket), when
 * pet_diagnostics may look at it. */
static pet_face_state_t face_during_copy;

/* The socket: open or not, the gateway's hello, whether it can rebind, the
 * binding it talks as and a rebind awaiting its answer. */
static struct {bool open,hello,rebind,pending;pet_control_context_t bound,asked;} socket_state;
static bool same_context(const pet_control_context_t *a,const pet_control_context_t *b)
{
    return a->binding.assigned&&b->binding.assigned&&!strcmp(a->binding.revision,b->binding.revision)&&
        !strcmp(a->binding.relationship_id,b->binding.relationship_id)&&!strcmp(a->binding.build_id,b->binding.build_id)&&
        !strcmp(a->binding.sha256,b->binding.sha256)&&!strcmp(a->config.version,b->config.version);
}
bool pet_network_is_ready(void){return socket_state.open&&socket_state.hello&&!socket_state.pending;}
bool pet_network_bound_to(const pet_control_context_t *c){return pet_network_is_ready()&&same_context(&socket_state.bound,c);}
bool pet_network_can_rebind(void){return pet_network_is_ready()&&socket_state.rebind;}
bool pet_network_can_story(void){return false;}
esp_err_t pet_network_story(uint32_t stream,const char *pet){(void)stream;(void)pet;assert(!"no story while switching");return ESP_FAIL;}
/* Story mode (story-mode-v1) as the session last told it, the pauses sent, and
 * the story frame over the pet. */
static bool story_mode,frame_shown;
static unsigned pauses;
bool pet_network_story_mode(void){return story_mode;}
bool pet_network_can_blessing(void){return false;}
bool pet_network_can_speak_blessing(void){return false;}
esp_err_t pet_network_blessing(uint32_t stream,const char *blessing,const char *pet)
{(void)stream;(void)blessing;(void)pet;assert(!"no blessing while switching");return ESP_FAIL;}
esp_err_t pet_network_story_request(const char *type,const char *id)
{assert(!strcmp(type,"story.pause")&&!id);++pauses;return ESP_OK;}
/* speech-pause-v1 as the gateway offers it, and the holds sent for a reply. */
static bool speech_pause_offered;
static unsigned output_pauses, output_resumes;
static uint32_t output_stream;
bool pet_network_can_pause_speech(void)
{
    return speech_pause_offered;
}
esp_err_t pet_network_output_pause(uint32_t stream, bool paused)
{
    assert(speech_pause_offered);
    output_stream = stream;
    if (paused)
        ++output_pauses;
    else
        ++output_resumes;
    return ESP_OK;
}
/* speech-pause-listen-v1 as the gateway offers it: it keeps a held reply
 * while the owner talks over it. */
static bool listen_offered;
bool pet_network_can_listen_over_speech(void)
{
    return listen_offered;
}
void pet_face_set_story_frame(bool shown){frame_shown=shown;}
void pet_onboarding_set_story_state(const char *face_id,const pet_story_state_t *state){(void)face_id;(void)state;}
esp_err_t pet_network_rebind(const pet_control_context_t *c)
{
    if(!pet_network_can_rebind())return ESP_ERR_NOT_SUPPORTED;
    ++rebinds;socket_state.asked=*c;socket_state.pending=true;return ESP_OK;
}
esp_err_t pet_network_start_bound(const pet_config_t *config,const pet_network_callbacks_t *callbacks,
    const char *device,const pet_control_context_t *context)
{
    (void)callbacks;assert(!strcmp(device,s_id));
    assert(config->gateway_secure && config->gateway_port == 443);
    assert(!strcmp(config->gateway_host, "fixture.invalid"));
    assert(pet_session_wire_brain_token(config->pairing_token));
    assert(strcmp(config->pairing_token, s_credential));
    ++starts;socket_state.open=true;socket_state.hello=socket_state.pending=false;socket_state.bound=*context;return ESP_OK;
}
void pet_network_freeze_bound(void){++freezes;socket_state.open=socket_state.hello=socket_state.pending=false;}
pet_control_http_timing_t pet_control_http_last_timing(void){return (pet_control_http_timing_t){0};}
void pet_control_http_close_kept(void){}
static unsigned brain_requests;
static bool brain_unavailable;
bool pet_control_http_json(const pet_control_http_t *http, const char *path, const char *body,
                          char *out, size_t capacity, pet_control_http_result_t *result)
{
    assert(http && !strcmp(path, "/v1/device/brain"));
    ++brain_requests;
    if (brain_unavailable)
    {
        *result = (pet_control_http_result_t){.status = 503};
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    assert(root);
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "endpoint", "wss://fixture.invalid/v1/device");
    cJSON_AddNumberToObject(root, "expiresInSeconds", 900);
    cJSON_AddStringToObject(root, "token", "brain1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    assert(cJSON_PrintPreallocated(root, out, (int)capacity, false));
    cJSON_Delete(root);
    *result = (pet_control_http_result_t){.status = 200, .bytes = strlen(out)};
    return true;
}
void pet_network_set_brain_token(const char *token)
{
    assert(!strncmp(token, "brain1_", 7));
}
bool pet_network_management_request(char *out, size_t capacity)
{
    (void)out;
    (void)capacity;
    return false;
}
bool pet_network_management_response(const char *json, size_t bytes)
{
    (void)json;
    (void)bytes;
    return false;
}
esp_err_t pet_network_cancel(uint32_t stream){++cancels;previous_cancel=last_cancel;last_cancel=stream;return ESP_OK;}
static esp_err_t input_start_result=ESP_OK;
esp_err_t pet_network_input_start(uint32_t stream,const char *ai_pet_id,const char *blessing){(void)stream;(void)ai_pet_id;(void)blessing;++input_starts;return input_start_result;}
esp_err_t pet_network_input_end(uint32_t stream,uint32_t sequence,uint32_t duration){(void)stream;(void)sequence;(void)duration;return ESP_OK;}
/* The device's answers to pet.config.desired: how many, and the last one. */
static unsigned config_answers;
static bool config_applied;
static pet_synced_config_v2_t config_echo;
esp_err_t pet_network_config_v2_result(const pet_synced_config_v2_t *c,bool applied,const char *error,const char *field)
{(void)field;assert(applied==!error);++config_answers;config_applied=applied;config_echo=*c;return ESP_OK;}
esp_err_t pet_network_speech_profile_result(const pet_speech_preference_t *s,bool applied){(void)s;(void)applied;return ESP_OK;}
bool pet_network_wifi_is_ready(void){return wifi_ok;}
void pet_network_set_battery_snapshot(const pet_battery_snapshot_t *snapshot){(void)snapshot;}
esp_err_t pet_network_ota_status(const char *release,const char *status,unsigned progress,const char *error)
{(void)release;(void)status;(void)progress;(void)error;return ESP_OK;}
/* What the gateway answers, delivered as pet_network delivers it. */
static void gateway_hello(bool can_rebind){socket_state.hello=true;socket_state.rebind=can_rebind;connected(true);}
static void rebind_accepted(void){assert(socket_state.pending);socket_state.pending=false;socket_state.bound=socket_state.asked;connected(true);}


/* The store: every slot holds the fixture pack and, unless unsigned, its signed record. */
esp_err_t pet_slot_store_open(pet_slot_store_t *s)
{
    memset(s,0,sizeof(*s));s->open=true;s->inventory.ready=true;s->inventory.state=stored;s->machine.state=stored.operation;
    assert(pet_flash_layout_known(PET_LAYOUT_ONE_10M,&s->layout));memset(s->partition_sha256,'c',64);return ESP_OK;
}
static int stream_read(void *context, uint32_t offset, void *out, uint32_t bytes)
{
    assert(offset <= pack_length && bytes <= pack_length - offset);
    memcpy(out, (const uint8_t *)context + offset, bytes);
    return 1;
}

esp_err_t pet_slot_store_stream(pet_slot_store_t *s, unsigned slot, bool target, void *buffer, uint32_t capacity,
                                uint32_t minimum_tail, fp_stream_t **stream)
{
    assert(s->open && slot < PET_SLOT_COUNT && buffer == s_shown_pack && capacity == PET_POCKET_BUFFER_BYTES &&
           !drawing_copy);
    const uint8_t *pack = pack_bytes;
    bool prove = s->inventory.state.slots[slot].validator_revision != FP_VALIDATOR_REVISION;
    if (target || s->inventory.state.slots[slot].state != PET_SLOT_READY) return ESP_ERR_INVALID_STATE;
    face_during_copy = face_state;
    return fp_stream_open(buffer, capacity, (uint32_t)pack_length, stream_read, (void *)pack, prove, minimum_tail,
                          stream) == FP_OK
               ? ESP_OK
               : ESP_ERR_INVALID_CRC;
}
esp_err_t pet_slot_store_read_record(pet_slot_store_t *s,unsigned slot,void *record,size_t bytes)
{
    assert(s==&s_slots&&slot<PET_SLOT_COUNT&&bytes==PET_REPLACE_MANIFEST_BYTES);
    memset(record,(unsigned_slots&(1u<<slot))?0xff:0x5a,bytes);((uint8_t *)record)[1]=(uint8_t)slot;return ESP_OK;
}
esp_err_t pet_slot_store_commit(pet_slot_store_t *s,const pet_slot_inventory_t *next)
{assert(pet_slot_inventory_valid(next));s->inventory.state=stored=*next;s->machine.state=next->operation;return ESP_OK;}
esp_err_t pet_slot_store_attach_installer(pet_slot_store_t *s,const pet_slot_readers_t *readers){s->installer=true;s->readers=*readers;return ESP_OK;}
bool pet_release_v2_record_verify(const void *record,size_t bytes,const char *account,const pet_replace_pack_t *expected,
                                  const pet_pack_trust_key_t *keys,size_t count,pet_release_v2_t *out)
{
    (void)bytes;(void)keys;(void)count;const uint8_t *raw=record;
    if(raw[0]==0xff||strcmp(account,signed_release.account_id))return false;
    *out=signed_release;out->pack=*expected;return true;
}
fp_error_t pet_face_pack_validate(const void *bytes,uint32_t length,fp_pack_info_t *info)
{
    uint32_t size=fp_validation_workspace_size();void *workspace=malloc(size);assert(workspace);
    fp_error_t result=fp_validate_with_workspace(bytes,length,info,workspace,size);free(workspace);return result;
}
esp_err_t pet_face_pack_use_external(const char *id,const void *pack,size_t bytes){(void)id;assert(pack==s_shown_pack&&bytes==pack_length);drawing_copy=true;return ESP_OK;}
esp_err_t pet_face_pack_use_source(const char *id, const fp_pack_source_t *source, const fp_pack_info_t *info)
{
    assert(source == fp_stream_source(s_stream) && source->bytes == pack_length);
    assert(info->approved && pet_face_id_matches(id, info->id, info->id_len, true));
    drawing_copy = true;
    return ESP_OK;
}
/* A story event is always over before the pet's copy is held or released. */
static const void *event_shown;
void pet_face_pack_hold_external(void){assert(!event_shown);drawing_copy=false;}
void pet_face_pack_release_external(void){assert(!event_shown);drawing_copy=false;}
bool pet_face_pack_ready(void){return drawing_copy;}

/* The worker (pet_replace_control.c, tested on its own): a selection is
 * adopted like a poll, pausing the Pocket's session through its hook, and the
 * next step mounts through the rebind hook before freezing and activating.
 * Without the hooks, as before session-rebind-v1, both freeze. */
#ifndef WORKER_PAUSE
#define WORKER_PAUSE() (s_pocket_pet.pause?s_pocket_pet.pause(NULL):freeze(NULL))
#define WORKER_REBIND(cloud) (s_pocket_pet.rebind&&s_pocket_pet.rebind(NULL,cloud))
#endif
static pet_replace_select_t select_answer=PET_REPLACE_SELECT_CHOSEN;
static unsigned select_revision;
pet_replace_select_t pet_replace_control_select(pet_replace_control_t *worker,uint64_t now,const pet_replace_pack_t *pack,int *http)
{
    assert(worker==s_pet&&worker->has_context&&worker->authenticated&&now>=worker->retry_at_ms&&http);++selects;
    if(select_answer!=PET_REPLACE_SELECT_CHOSEN){*http=select_answer==PET_REPLACE_SELECT_REFUSED?409:0;return select_answer;}
    pet_control_context_t *c=&worker->cloud.context;c->binding.assigned=true;
    strcpy(c->binding.build_id,pack->build_id);strcpy(c->binding.sha256,pack->sha256);
    snprintf(c->binding.revision,sizeof(c->binding.revision),"selected-%u",++select_revision);
    snprintf(c->binding.relationship_id,sizeof(c->binding.relationship_id),"00000000-0000-4000-8000-0000000006%02u",select_revision%100);
    snprintf(c->config.version,sizeof(c->config.version),"%u",100+select_revision);
    if(worker->admitted){WORKER_PAUSE();worker->admitted=false;}
    *http=200;return PET_REPLACE_SELECT_CHOSEN;
}
static void mount(void)
{
    if(pet_worker.admitted)return;
    if(!WORKER_REBIND(&pet_worker.cloud.context)){freeze(NULL);assert(pocket_activate(NULL,&pet_worker.cloud.context));}
    pet_worker.admitted=true;
}
void pet_replace_control_disconnect(pet_replace_control_t *worker)
{assert(worker==s_pet);++disconnects;if(worker->admitted)freeze(NULL);worker->admitted=false;worker->next_poll_ms=0;}
static unsigned pet_steps,firmware_steps;
pet_replace_control_status_t pet_replace_control_step(pet_replace_control_t *worker,uint64_t now,bool allow)
{(void)worker;(void)now;(void)allow;assert(pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));++pet_steps;return PET_REPLACE_CONTROL_READY;}
bool pet_replace_control_init(pet_replace_control_t *w,const pet_replace_control_config_t *c,pet_replace_journal_t *s,pet_replace_writer_t *writer)
{(void)w;(void)c;(void)s;(void)writer;return true;}
bool pet_firmware_control_init(pet_firmware_control_t *w,const pet_firmware_control_config_t *c,pet_firmware_receipt_store_t *s)
{(void)w;(void)c;(void)s;return true;}
bool pet_firmware_control_blocks_pet(const pet_firmware_control_t *worker){(void)worker;return false;}
bool pet_firmware_control_health_deadline(pet_firmware_control_t *worker,uint64_t now){(void)worker;(void)now;return false;}
/* What happens while a cloud step holds the control task, if anything. */
static void (*during_step)(void);
pet_firmware_control_status_t pet_firmware_control_step(pet_firmware_control_t *worker,uint64_t now)
{(void)worker;(void)now;++firmware_steps;if(during_step)during_step();return PET_FW_CONTROL_WORKING;}
bool pet_firmware_receipt_open_nvs(pet_firmware_receipt_store_t *store){store->loaded=true;return true;}
esp_err_t pet_flash_layout_read(pet_flash_layout_t *layout,char sha[65])
{(void)layout;(void)sha;assert(!"no firmware protection is proved here");return ESP_FAIL;}
bool pet_setup_identity(char id[37],char credential[44]){(void)id;(void)credential;return true;}
bool pet_setup_control_ready(void){return true;}
bool pet_onboarding_ui_ready(void){return true;}
void pet_onboarding_update_status(const pet_onboarding_status_t *status){visible_status=*status;}
const esp_app_desc_t *esp_app_get_description(void){static const esp_app_desc_t app={.version="host-fixture"};return &app;}
bool pet_firmware_image_bootloader_matches(const pet_firmware_bootloader_t *profile){(void)profile;return true;}
bool pet_firmware_image_boot_state(pet_firmware_boot_state_t *out){(void)out;return false;}
esp_err_t pet_ota_confirm_health_stage(pet_ota_health_stage_t stage,const pet_ota_health_evidence_t *evidence)
{(void)stage;(void)evidence;return ESP_OK;}
bool pet_ota_busy(void){return false;}

/* Face, sound and audio. */
esp_err_t pet_face_init(const pet_face_callbacks_t *callbacks,uint8_t volume,uint8_t brightness,uint8_t shake,
    const char *ssid,const char *face_id,pet_voice_t voice,uint16_t timeout,pet_animation_profile_t profile,pet_ai_mode_t mode,
    pet_realtime_model_t model,pet_speech_profile_t speech,pet_realtime_voice_t realtime,pet_realtime_boost_t boost,
    pet_cartesia_voice_gender_t cartesia,pet_face_gender_t gender,pet_speech_mouth_mode_t mouth)
{
    (void)callbacks;(void)volume;(void)brightness;(void)shake;(void)ssid;(void)face_id;(void)voice;(void)timeout;(void)profile;
    (void)mode;(void)model;(void)speech;(void)realtime;(void)boost;(void)cartesia;(void)gender;(void)mouth;return ESP_OK;
}
esp_err_t pet_face_set_id(const char *face_id){(void)face_id;return ESP_OK;}
void pet_face_set_state(pet_face_state_t state){face_state=state;}
pet_face_state_t pet_face_get_state(void){return face_state;}
void pet_face_announce(const char *text)
{
    assert(strlen(text) <= PET_FACE_TERMINAL_MAX);
    strlcpy(announced, text, sizeof(announced));
}
void pet_face_show(void){}
bool pet_face_touch_reaction(void){return true;}
esp_err_t pet_face_set_animation_profile(pet_animation_profile_t profile){(void)profile;return ESP_OK;}
esp_err_t pet_face_set_brightness(uint8_t brightness){(void)brightness;return ESP_OK;}
esp_err_t pet_face_set_recording_timeout(uint16_t seconds){(void)seconds;return ESP_OK;}
esp_err_t pet_face_set_shake_sensitivity(uint8_t sensitivity){(void)sensitivity;return ESP_OK;}
esp_err_t pet_face_set_speech_profile(pet_speech_profile_t mode,bool batch,bool realtime,bool fish)
{(void)mode;(void)batch;(void)realtime;(void)fish;return ESP_OK;}
void pet_face_set_expression(pet_expression_t expression){(void)expression;}
bool pet_expression_from_wire(const char *name,pet_expression_t *value){(void)name;(void)value;return false;}
uint8_t pet_face_trigger_gesture(uint8_t gesture){(void)gesture;return 0;}
bool pet_face_shake_detection_enabled(void){return false;}
void pet_face_set_battery(const pet_battery_snapshot_t *snapshot){(void)snapshot;}
esp_err_t pet_sfx_play(pet_sfx_t effect){++sounds;switch_sounds+=effect==PET_SFX_FACE_SWIPE_0;last_sound=effect;if(effect==PET_SFX_ERROR)++error_cues;return ESP_OK;}
esp_err_t pet_sfx_play_listen(uint32_t token){(void)token;++listen_cues;return ESP_OK;}
void pet_sfx_cancel(void){}
esp_err_t pet_sfx_init(pet_sfx_listen_done_t done){(void)done;return ESP_OK;}
pet_sfx_t pet_sfx_for_face_swipe(uint8_t index){(void)index;return PET_SFX_FACE_SWIPE_0;}
esp_err_t pet_audio_init(pet_audio_capture_chunk_cb_t chunk,pet_audio_capture_done_cb_t done,pet_audio_playback_done_cb_t played)
{(void)chunk;(void)done;(void)played;return ESP_OK;}
esp_err_t pet_audio_capture_start(uint32_t stream){(void)stream;++capture_starts;capturing=true;return ESP_OK;}
void pet_audio_capture_stop(void){capturing=false;}
bool pet_audio_is_capturing(void){return capturing;}
bool pet_audio_is_playing(void){return playing;}
/* The reply is held, its last audio drains (a hold is then refused), and the
 * speaker's queue is nearly full. */
static bool held, draining, queue_full;
bool pet_audio_playback_queue_healthy(void){return !queue_full;}
void pet_audio_playback_cancel(void){playing=held=false;}
bool pet_audio_playback_hold(bool hold)
{
    if (hold && (!playing || draining))
        return false;
    held = hold;
    return true;
}
bool pet_audio_playback_held(void)
{
    return held && playing;
}
bool pet_audio_playback_hold_to_listen(void)
{
    return pet_audio_playback_hold(true);
}
pet_atmosphere_t pet_atmosphere_from_name(const char *name){(void)name;return PET_ATMOSPHERE_NONE;}
void pet_atmosphere_cue(pet_atmosphere_t atmosphere){(void)atmosphere;}
void pet_atmosphere_cue_name(const char *name){(void)name;}
/* The bank's one-shots: the cues queued, which hold the next reply, and the
 * stingers started. */
static unsigned cues_queued, stingers;
static bool cues_waiting;
static char stinger[PET_SOUND_ID_BYTES + 1];
bool pet_atmosphere_queue_sound(const char *id){(void)id;++cues_queued;cues_waiting=true;return true;}
bool pet_audio_playback_hold_for_sounds(void (*played)(uint32_t stream))
{
    assert(played == sounds_played);
    if (!cues_waiting)
        return false;
    cues_waiting = false;
    held = true;
    return true;
}
bool pet_audio_play_story_sound(const char *id){strcpy(stinger,id);++stingers;return true;}
/* The sound bank (pet_sound_bank.c has its own test): none is named here. */
void pet_sound_bank_open(bool usable){(void)usable;}
void pet_sound_bank_want(const char *sha256,uint32_t bytes){(void)sha256;(void)bytes;}
bool pet_sound_bank_step(const pet_control_http_t *http,uint8_t *buffer,size_t capacity,uint64_t now_ms)
{(void)http;(void)buffer;(void)capacity;(void)now_ms;return false;}
esp_err_t pet_audio_playback_start(uint32_t stream,uint32_t rate,pet_realtime_boost_t boost,pet_speech_mouth_mode_t mouth)
{(void)stream;(void)rate;(void)boost;(void)mouth;playing=true;held=false;return ESP_OK;}
esp_err_t pet_audio_playback_enqueue(uint32_t stream,uint32_t sequence,const uint8_t *pcm,size_t length)
{(void)stream;(void)sequence;(void)pcm;(void)length;return ESP_OK;}
void pet_audio_playback_finish(uint32_t stream){(void)stream;}
esp_err_t pet_audio_set_capture_timeout(uint16_t seconds){(void)seconds;return ESP_OK;}
void pet_audio_set_volume(uint8_t volume){(void)volume;}
/* Mouth timing: what the speaker uses now, and what NVS keeps. */
static int16_t mouth_offset;
static unsigned mouth_offset_stores;
static bool mouth_offset_kept;
static int16_t mouth_offset_kept_ms;
void pet_audio_set_speech_mouth_offset(int16_t offset_ms){mouth_offset=offset_ms;}
esp_err_t pet_config_store_volume(uint8_t volume){(void)volume;return ESP_OK;}
esp_err_t pet_config_store_brightness(uint8_t brightness){(void)brightness;return ESP_OK;}
esp_err_t pet_config_store_menu_style(uint8_t style){(void)style;return ESP_OK;}
esp_err_t pet_config_store_speech_mouth_offset(bool present,int16_t offset_ms)
{++mouth_offset_stores;mouth_offset_kept=present;mouth_offset_kept_ms=offset_ms;return ESP_OK;}
void pet_motion_set_sensitivity(uint8_t sensitivity){(void)sensitivity;}
esp_err_t pet_motion_start(pet_motion_gesture_callback_t callback,pet_motion_enabled_callback_t enabled,uint8_t sensitivity)
{(void)callback;(void)enabled;(void)sensitivity;return ESP_OK;}
void pet_battery_set_enabled(bool enabled){(void)enabled;}
esp_err_t pet_battery_start(pet_battery_callback_t callback){(void)callback;return ESP_OK;}
void pet_diagnostics_note_dropped_app_event(void){}
void pet_diagnostics_note_app_event(int32_t event,const char *name,uint32_t depth){(void)event;(void)name;(void)depth;}
void pet_diagnostics_note_streams(uint32_t input,uint32_t output){(void)input;(void)output;}
void pet_enrollment_clear(void *data,size_t bytes){memset(data,0,bytes);}
void pet_onboarding_open_from_ui(void){}
bool pet_onboarding_setup_ready(void){return true;}
static unsigned screens_opened;
void pet_onboarding_open_screen(pet_onboarding_screen_t screen)
{assert((unsigned)screen<=PET_ONBOARDING_STORY);++screens_opened;}
int64_t esp_timer_get_time(void){return (int64_t)clock_us;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out){(void)args;*out=NULL;return ESP_FAIL;}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t timeout){(void)timer;(void)timeout;return ESP_OK;}
esp_err_t gpio_config(const gpio_config_t *config){(void)config;return ESP_OK;}
int gpio_get_level(gpio_num_t gpio_num){(void)gpio_num;return 1;}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer,uint64_t period){(void)timer;(void)period;return ESP_OK;}
uint32_t esp_random(void){return 7;}
void esp_fill_random(void *buffer,size_t bytes){memset(buffer,7,bytes);}
/* The only aligned PSRAM block is the copy of the pet on screen: story events'
 * packs and their check live in its tail. */
void *heap_caps_aligned_alloc(size_t alignment,size_t size,unsigned caps)
{
    (void)caps;
    assert(size == PET_POCKET_BUFFER_BYTES);
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}
void heap_caps_free(void *memory){free(memory);}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps){(void)caps;return calloc(count,size);}
static bool display_locked;
esp_err_t pet_display_lock(int timeout){(void)timeout;assert(!display_locked);display_locked=true;return ESP_OK;}
void pet_display_unlock(void){assert(display_locked);display_locked=false;}
/* The renderer shows a story event over the pet until it ends by itself
 * (event_ends) or is stopped. The device's origin serves the event pack under
 * any sha256, zeros past its end; it can damage one byte, or cut downloads
 * off halfway. */
static uint8_t event_pack[PET_STORY_EVENT_BYTES];
static size_t event_length, event_damage = SIZE_MAX;
static char event_sha[65];
static void (*event_done)(void);
static unsigned events_shown, events_stopped, event_downloads, cut_downloads;
esp_err_t pet_face_pack_play_event(const void *pack, size_t bytes, void (*done)(void))
{
    assert(display_locked && drawing_copy && !event_shown && done);
    assert(bytes == event_length && !memcmp(pack, event_pack, bytes));
    event_shown = pack;
    event_done = done;
    ++events_shown;
    return ESP_OK;
}
static void event_ends(void)
{
    assert(event_shown);
    event_shown = NULL;
    event_done();
}
void pet_face_pack_stop_event(void)
{
    assert(display_locked);
    if (event_shown)
    {
        ++events_stopped;
        event_ends();
    }
}
/* story.event.ended sent (story-reaction-v1): how often, for which event, and
 * how many cancels had been sent before the last one. */
static unsigned ends_sent, cancels_before_end;
static char end_sent[PET_STORY_EVENT_ID_BYTES + 1];
esp_err_t pet_network_story_event_ended(const char *id)
{
    ++ends_sent;
    strlcpy(end_sent, id, sizeof(end_sent));
    cancels_before_end = cancels;
    return ESP_OK;
}
/* The unused tail of the pet's copy, past the check's workspace. */
static uint8_t *copy_tail(void)
{
    return (uint8_t *)fp_stream_tail(s_stream, NULL) + fp_validation_workspace_size();
}
bool pet_control_http_event_range(const pet_control_http_t *http, const char *sha256, uint32_t total, uint32_t offset,
                                  void *out, size_t bytes, pet_control_http_result_t *result)
{
    assert(!strcmp(http->origin, s_origin) && !strcmp(http->device_id, s_id) && strlen(sha256) == 64);
    assert(offset <= total && bytes <= total - offset && bytes <= 32768u && total <= PET_STORY_EVENT_BYTES);
    assert(!atomic_load(&s_turn_active) && !playing && !capturing);      /* only between turns */
    assert((uint8_t *)out >= copy_tail() && (uint8_t *)out + bytes <= s_shown_pack + PET_POCKET_BUFFER_BYTES);
    ++event_downloads;
    for (size_t i = 0; i < bytes; ++i) ((uint8_t *)out)[i] = offset + i < event_length ? event_pack[offset + i] : 0;
    if (event_damage >= offset && event_damage - offset < bytes) ((uint8_t *)out)[event_damage - offset] ^= 1;
    *result = (pet_control_http_result_t){.status = 206, .retry_seconds = 30, .bytes = bytes};
    if (!cut_downloads)
        return true;
    --cut_downloads;
    result->bytes = bytes / 2;
    return false;
}
esp_err_t bsp_display_brightness_set(int brightness){(void)brightness;return ESP_OK;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (SemaphoreHandle_t)1;}
int xSemaphoreTake(SemaphoreHandle_t handle,TickType_t ticks){(void)handle;(void)ticks;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t handle){(void)handle;return pdTRUE;}
/* Queued audio events are delivered by the test itself; commands wait for the
 * control task's next pass (pocket_control_pass), eight at most. */
static audio_event_t queued[16];static unsigned queued_count;
static control_command_t commands[8];static unsigned command_count;
int xQueueSend(QueueHandle_t queue,const void *item,TickType_t ticks)
{
    (void)ticks;
    if(queue==s_audio_events&&queued_count<16)queued[queued_count++]=*(const audio_event_t *)item;
    if(queue==s_commands){if(command_count==8)return 0;commands[command_count++]=*(const control_command_t *)item;}
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue,void *item,TickType_t ticks)
{
    (void)ticks;assert(queue==s_commands);if(!command_count)return 0;
    *(control_command_t *)item=commands[0];memmove(commands,commands+1,--command_count*sizeof(*commands));return pdTRUE;
}
void esp_restart(void){assert(!"no restart");abort();}
/* Waits for the speaker's room, among others. */
static unsigned delays;
void vTaskDelay(TickType_t ticks){(void)ticks;++delays;}

/* One audio-task pass: the event, then the send it asks for. */
static void deliver(audio_event_t event)
{
    audio_send_t send={.kind=SEND_NONE};
    if(!audio_event_current(&event))return;
    handle_audio(&event,&send);finish_send(&send,&event);
}
static void drain(void){while(queued_count){audio_event_t e=queued[0];memmove(queued,queued+1,--queued_count*sizeof(*queued));deliver(e);}}
static void tap(void)
{
    const int64_t touched = (int64_t)clock_us;
    deliver((audio_event_t){.kind = AUDIO_TAP, .touched_us = touched, .generation = atomic_load(&s_generation)});
}
static void retry(void){deliver((audio_event_t){.kind=AUDIO_TAP_RETRY,.generation=atomic_load(&s_generation)});}
static void cue_done(void){deliver((audio_event_t){.kind=AUDIO_LISTEN_CUE_DONE,.stream=s_listen_token,.result=ESP_OK,.generation=atomic_load(&s_generation)});}
static const char *tile(void)
{
    publish_status(0);const pet_onboarding_status_t *s=&visible_status;
    if(s->conversation!=PET_ONBOARDING_CONVERSATION_UNAVAILABLE)return s->conversation==PET_ONBOARDING_CONVERSATION_IDLE?"READY":"BUSY";
    return !s->has_pet?"NO PET":s->wifi!=PET_ONBOARDING_WIFI_CONNECTED?"NO WIFI":!s->pet_linked?"NOT LINKED":"CONNECTING";
}
static void hex_sha256(const uint8_t *bytes,size_t length,char out[65])
{uint8_t digest[32];assert(!mbedtls_sha256(bytes,length,digest,0));for(size_t i=0;i<32;++i)snprintf(out+2*i,3,"%02x",digest[i]);}
static const char *builds[PET_SLOT_COUNT]={"00000000-0000-4000-8000-000000000040"};
/* Two signed cloud pets; slot 1 shows and is bound, and its session is open:
 * answered (it talks) or still connecting. */
static void reset_counts(void)
{freezes=starts=rebinds=cancels=disconnects=selects=sounds=switch_sounds=error_cues=listen_cues=capture_starts=input_starts=0;}
static void boot(bool answered)
{
    reset_counts();
    atomic_store(&s_brain_disconnected, false);
    atomic_store(&s_brain_reconnect, false);
    atomic_store(&s_brain_expires_ms, 0);
    s_brain_token[0] = 0;
    s_brain_retry_ms = 0;
    brain_unavailable = false;
    pet_slot_inventory_t three;assert(pet_slot_inventory_empty(&three));
    for(unsigned slot=0;slot<PET_SLOT_COUNT;++slot){
        pet_slot_t *t=&three.slots[slot];t->state=PET_SLOT_READY;t->pack.bytes=(uint32_t)pack_length;
        t->validator_revision=FP_VALIDATOR_REVISION;t->shown_at=slot+1;strcpy(t->pack.build_id,builds[slot]);
        hex_sha256(pack_bytes,pack_length,t->pack.sha256);
    }
    three.active=0;three.selection_revision=3;
    assert(pet_slot_inventory_rebind(&three,0,"binding-41","00000000-0000-4000-8000-000000000050","20"));
    stored=three;memset(&socket_state,0,sizeof(socket_state));
    s_stream = NULL;
    free(s_shown_pack);
    s_shown_pack = NULL;
    s_shown_slot = -1;
    s_selection_save_at = 0;
    drawing_copy = false;
    memset(&s_prepared,0,sizeof(s_prepared));
    s_select_moment=SELECT_NONE;s_select_refused_build[0]=0;
    pocket_control_start();assert(s_shown_slot==0&&s_slots.inventory.state.bound==0);
    s_pet=&pet_worker;s_firmware=&firmware_worker;memset(&pet_worker,0,sizeof(pet_worker));
    pet_worker.has_context=pet_worker.authenticated=true;
    pet_control_context_t *c=&pet_worker.cloud.context;strcpy(c->device_id,s_id);strcpy(c->account_id,signed_release.account_id);
    c->binding=(pet_control_binding_t){.assigned=true,.revision="binding-41",.relationship_id="00000000-0000-4000-8000-000000000050"};
    strcpy(c->binding.build_id,builds[0]);strcpy(c->binding.sha256,three.slots[0].pack.sha256);
    strcpy(c->config.version,"20");strcpy(c->config.face_id,signed_release.face_id);strcpy(c->config.ai_pet_id,signed_release.face_id);
    c->config.volume=40;c->config.brightness=80;c->config.recording_timeout=30;
    mount();assert(starts==1&&!rebinds&&socket_state.open);
    if(answered){gateway_hello(true);assert(atomic_load(&s_voice_allowed)&&!strcmp(tile(),"READY"));}
    else assert(!atomic_load(&s_voice_allowed)&&!strcmp(tile(),"CONNECTING"));
    reset_counts();
}
static void end_turn(void)
{capturing=false;atomic_store(&s_turn_active,false);pet_runtime_release(&s_resources,PET_RESOURCE_VOICE);s_input_stream=0;}
/* A swipe to the next pet and the pass after it settles, which asks to select it. */
static void swipe(int direction){pocket_switch(direction);}


/* A swipe keeps the session and says CONNECTING, not NOT LINKED; the
 * previous pet's late answer is dropped quietly. */

/* The message a tap types while the open session moves to the pet on screen. */

/* The swipe settles: the cloud selects the pet and the session moves to it
 * with a rebind, not a new socket. The pet stays idle on screen, never the
 * connecting face. A tap meanwhile waits calmly, with the microphone closed,
 * says it is switching, and listens once the rebind is accepted. */

/* A tap 2.4 s after the swipe, while the swipe settles and the cloud selects
 * (the 26 Sep capture): it waits for the whole switch however long, past the
 * 5 s a connecting tap waits, and listens once the rebind is accepted. The
 * face never shows connecting. */

/* A pet the cloud refuses to select: a tap that waits for the switch hears
 * why as soon as it is known, not after its wait. */

/* Back on the pet the open session talks as: voice returns at once. */

/* While the session connects a tap waits calmly: it listens when the session
 * is ready, gives up after 5 s with a friendly retry, and a second tap
 * cancels it. No error cue, and the microphone stays closed until ready. */
static void tap_waits_for_the_session(void)
{
    boot(false);
    tap();
    assert(atomic_load(&s_tap_pending)&&face_state==PET_FACE_CONNECTING&&!strcmp(announced,"> connecting...")&&!sounds);
    clock_us+=4900000;retry();assert(atomic_load(&s_tap_pending)&&!listen_cues);
    clock_us+=200000;retry();
    assert(!atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&!strcmp(announced,"> not ready, try again"));
    assert(!error_cues&&!sounds&&!capture_starts&&!input_starts&&!listen_cues);
    tap();assert(atomic_load(&s_tap_pending));tap();
    assert(!atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&!strcmp(announced,"> cancelled")&&!sounds&&!capture_starts);
    tap();assert(atomic_load(&s_tap_pending)&&!capture_starts);
    gateway_hello(true);assert(atomic_load(&s_voice_allowed)&&face_state==PET_FACE_CONNECTING);
    retry();cue_done();
    assert(!atomic_load(&s_tap_pending)&&capture_starts==1&&input_starts==1&&face_state==PET_FACE_LISTENING&&!error_cues);
    end_turn();
}
/* A refused rebind reconnects as before, calmly: a waiting tap keeps its
 * connecting face and listens once the new socket's hello arrives. */

/* No answer within the rebind window: reconnect. */

/* An older gateway (no session-rebind-v1): the swipe still keeps the socket,
 * and the selection reconnects as today. */

/* The gateway's pet.config.desired for `c`, delivered as pet_network delivers it. */
static void desired_config(const pet_control_context_t *c,bool has_offset,int16_t offset_ms)
{
    pet_synced_config_v2_t s={.version=(uint32_t)strtoul(c->config.version,NULL,10),.volume=c->config.volume,
        .brightness=c->config.brightness,.shake_sensitivity=c->config.shake_sensitivity,
        .recording_timeout_seconds=c->config.recording_timeout,.animation_profile=(pet_animation_profile_t)c->config.animation_profile,
        .speech_profile=(pet_speech_profile_t)c->config.speech_profile,.has_speech_mouth_offset=has_offset,
        .speech_mouth_offset_ms=offset_ms};
    strlcpy(s.face_id,c->config.face_id,sizeof(s.face_id));strlcpy(s.ai_pet_id,c->config.ai_pet_id,sizeof(s.ai_pet_id));
    assert(config_received(&s));drain();
}
/* After an accepted rebind the gateway sends the new pet's configuration, as
 * after a hello (session-rebind-v1's afterAcceptance). It applies
 * exactly as it does after a hello, mouth timing included: the speaker uses it
 * at once, NVS keeps it and the answer echoes it. */

/* A management answer that crosses a swipe still names the previous pet. It is
 * no mismatch while the session moves: revalidating there turned a switch into
 * a reconnect (1 Oct). After the switch, the same answer still revalidates. */
static void management_answer_before_the_session_waits(void)
{
    // Booted, the socket opening, no session yet: a newer cloud config is no
    // reason to start over; the admission brings it.
    boot(false);
    pet_control_context_t newer=pet_worker.cloud.context;strcpy(newer.config.version,"21");
    desired_config(&newer,false,0);
    assert(!atomic_load(&s_revalidate)&&!freezes&&socket_state.open);
    // Once the session answers, a config that differs from it revalidates.
    gateway_hello(true);desired_config(&newer,false,0);assert(atomic_load(&s_revalidate));
}

/* A pet placed over USB has no cloud release: NOT LINKED once known, and a
 * tap says so with the no-voice cue. Until then it is CONNECTING. */


/* ---- Listening --------------------------------------------------------- */

/* One reply heard to its end: speech, then the speaker's done report. */
static void reply_heard(uint32_t stream)
{
    assert(audio_start(stream,24000)==ESP_OK&&playing&&face_state==PET_FACE_SPEAKING);
    assert(audio_chunk(stream,0,(const uint8_t *)"\0\0",2)==ESP_OK);audio_end(stream,0);
    playing=false;deliver((audio_event_t){.kind=AUDIO_PLAYBACK_DONE,.stream=stream,.generation=atomic_load(&s_generation)});
    assert(face_state==PET_FACE_IDLE&&!atomic_load(&s_turn_active)&&!pet_runtime_owns(&s_resources,PET_RESOURCE_VOICE));
}
/* The control task is in a cloud step (a poll, several seconds): a tap waits
 * for it, however long, with the microphone closed and the face not
 * LISTENING; it listens, with its cue, as soon as control releases. It used
 * to give up after 2 s, showing LISTENING and then idle. */
static void tap_waits_for_a_control_step(void)
{
    boot(true);
    assert(pet_runtime_claim(&s_resources,PET_RESOURCE_CONTROL));
    tap();
    assert(atomic_load(&s_tap_pending)&&atomic_load(&s_tap_for_control)&&face_state==PET_FACE_IDLE);
    assert(!strcmp(announced,"> one moment...")&&!listen_cues&&!capture_starts&&!input_starts);
    for(int waited=0;waited<50;++waited){clock_us+=100000;retry();} /* 5 s, polled as the audio task does */
    assert(atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&!listen_cues&&!capture_starts);
    pet_runtime_release(&s_resources,PET_RESOURCE_CONTROL);
    retry();
    assert(!atomic_load(&s_tap_pending)&&!atomic_load(&s_tap_for_control)&&listen_cues==1&&face_state==PET_FACE_LISTENING);
    cue_done();assert(capture_starts==1&&input_starts==1&&capturing);
    end_turn();
    // A step that keeps control past 10 s gives up kindly; a second tap cancels.
    assert(pet_runtime_claim(&s_resources,PET_RESOURCE_CONTROL));
    tap();clock_us+=9900000;retry();assert(atomic_load(&s_tap_pending));
    clock_us+=200000;retry();
    assert(!atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&!strcmp(announced,"> not ready, try again"));
    tap();assert(atomic_load(&s_tap_pending));tap();
    assert(!atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&!strcmp(announced,"> cancelled"));
    assert(!error_cues&&listen_cues==1&&capture_starts==1);
    pet_runtime_release(&s_resources,PET_RESOURCE_CONTROL);
}
/* Control yields to a waiting tap between its two cloud calls: the pet
 * worker's call comes after the listen. */
static void control_yields_to_a_waiting_tap(void)
{
    boot(true);
    assert(pet_runtime_claim(&s_resources,PET_RESOURCE_CONTROL));
    tap();assert(atomic_load(&s_tap_for_control));
    unsigned firmware=firmware_steps,pets=pet_steps;
    independent_updates(clock_us/1000);
    assert(firmware_steps==firmware+1&&pet_steps==pets&&!pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));
    drain(); /* The retry control queued: it listens. */
    assert(!atomic_load(&s_tap_pending)&&listen_cues==1&&pet_runtime_owns(&s_resources,PET_RESOURCE_VOICE));
    cue_done();assert(capture_starts==1);
    end_turn();
}
/* Nothing listens without a tap: after a reply the face stays idle and the
 * microphone closed, however long nothing happens. And the cloud poll the
 * turn postponed waits 8 s, so a follow-up tap listens at once. */
static void a_follow_up_tap_listens_at_once(void)
{
    boot(true);
    tap();cue_done();assert(capture_starts==1);
    deliver((audio_event_t){.kind=AUDIO_CAPTURE_DONE,.stream=s_input_stream,.sequence=9,.duration=2000,.result=ESP_OK,
        .generation=atomic_load(&s_generation)});
    capturing=false;assert(face_state==PET_FACE_THINKING);
    reply_heard(101);
    unsigned pets=pet_steps,firmware=firmware_steps;
    for(int t=0;t<79;++t){clock_us+=100000;pocket_tick((int64_t)clock_us);retry();}
    assert(pet_steps==pets&&firmware_steps==firmware&&face_state==PET_FACE_IDLE&&listen_cues==1&&capture_starts==1);
    assert(!pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));
    // A follow-up tap 7.9 s after the reply: the microphone is free.
    tap();assert(!atomic_load(&s_tap_pending)&&listen_cues==2&&face_state==PET_FACE_LISTENING);
    cue_done();assert(capture_starts==2);
    deliver((audio_event_t){.kind=AUDIO_CAPTURE_DONE,.stream=s_input_stream,.sequence=9,.duration=2000,.result=ESP_OK,
        .generation=atomic_load(&s_generation)});
    capturing=false;reply_heard(102);
    // With no tap the postponed poll runs after 8 s, and nothing listens.
    pets=pet_steps;
    for(int t=0;t<120;++t){clock_us+=100000;pocket_tick((int64_t)clock_us);retry();}
    assert(pet_steps>pets&&face_state==PET_FACE_IDLE&&listen_cues==2&&capture_starts==2&&!capturing);
    // One installed pet has no neighbouring slot to select.
    reply_heard(103);pets=pet_steps;
    swipe(1);pocket_tick((int64_t)clock_us);assert(pet_steps==pets);
}
/* A listen whose audio cannot be sent (the microphone's 4 s send queue
 * overflowed while the socket stalled) ends with the error face, the error
 * cue and a message, never a quiet return to idle. */
static void a_stalled_listen_says_so(void)
{
    boot(true);
    tap();cue_done();assert(capturing);
    uint32_t stream=s_input_stream;unsigned before=cancels;
    capturing=false;
    deliver((audio_event_t){.kind=AUDIO_CAPTURE_DONE,.stream=stream,.sequence=150,.duration=3000,.result=ESP_ERR_NO_MEM,
        .generation=atomic_load(&s_generation)});
    assert(face_state==PET_FACE_ERROR&&error_cues==1&&!strcmp(announced,"> network slow, try again"));
    assert(cancels==before+1&&last_cancel==stream&&!atomic_load(&s_turn_active)&&!pet_runtime_owns(&s_resources,PET_RESOURCE_VOICE));
    // The uplink stalls as the listen starts: the socket does not take the
    // turn's start, so it says so and reconnects.
    input_start_result=ESP_FAIL;atomic_store(&s_revalidate,false);
    tap();cue_done();
    assert(!capturing&&face_state==PET_FACE_ERROR&&error_cues==2&&!strcmp(announced,"> network slow, try again"));
    assert(atomic_load(&s_revalidate)&&!atomic_load(&s_turn_active));
    input_start_result=ESP_OK;atomic_store(&s_revalidate,false);
}

static void brain_disconnect_releases_voice(void)
{
    for (unsigned speaking = 0; speaking < 2; ++speaking)
    {
        boot(true);
        assert(pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE));
        atomic_store(&s_turn_active, true);
        s_output_stream = speaking ? 17 : 0;
        playing = speaking;
        unsigned generation = atomic_load(&s_generation);
        unsigned requests = brain_requests;
        socket_state.open = socket_state.hello = false;
        connected(false);
        independent_updates(clock_us / 1000);
        assert(!playing && !capturing && !atomic_load(&s_turn_active));
        assert(!pet_runtime_owns(&s_resources, PET_RESOURCE_VOICE));
        assert(atomic_load(&s_generation) != generation);
        assert(socket_state.open && starts == 1 && !disconnects);
        assert(brain_requests == requests); /* Network loss keeps an unexpired grant. */
        gateway_hello(true);
        connected(false); /* Old socket drained during replacement. */
        gateway_hello(true); /* Replacement answers before the next worker pass. */
        assert(pet_runtime_claim(&s_resources, PET_RESOURCE_VOICE));
        atomic_store(&s_turn_active, true);
        independent_updates(clock_us / 1000);
        assert(atomic_load(&s_turn_active) && atomic_load(&s_voice_allowed));
        end_turn();
    }
}

static void brain_failure_and_renewal(void)
{
    boot(true);
    unsigned requests = brain_requests;
    atomic_store(&s_brain_expires_ms, (uint32_t)(clock_us / 1000) + 30000);
    brain_unavailable = true;
    independent_updates(clock_us / 1000);
    assert(brain_requests == requests + 1 && socket_state.open && !freezes && !disconnects);
    assert(pet_worker.admitted && atomic_load(&s_voice_allowed));
    clock_us += 31000000;
    tap();
    assert(!capturing && !listen_cues); /* An expired grant cannot begin a new turn. */
    atomic_store(&s_tap_for_control, false);
    atomic_store(&s_tap_pending, false);
    independent_updates(clock_us / 1000);
    assert(!socket_state.open && !atomic_load(&s_voice_allowed) && pet_worker.admitted);
    brain_unavailable = false;
    clock_us += 16000000;
    independent_updates(clock_us / 1000);
    assert(socket_state.open && pet_worker.admitted);
    gateway_hello(true);
    requests = brain_requests;
    clock_us = ((uint64_t)INT32_MAX + 1000) * 1000;
    session_error("BRAIN_AUTH_REQUIRED", "expired", false);
    assert(!atomic_load(&s_brain_expires_ms));
    assert(connect_brain(&pet_worker.cloud.context));
    assert(brain_requests == requests + 1);
    atomic_store(&s_revalidate, false);
}

/* A swipe while the pet listens, thinks or speaks stops the audio and leaves
 * the face idle, never LISTENING or SPEAKING without its audio: pet_diagnostics
 * reads that as a stall and its recovery replaces the socket. On the Pocket a
 * swipe 6.8 s into a reply did that, so the switch needed a new socket. */


/* Switches from the deck's pet picker while a cloud step holds the control
 * task. On 1 Oct a step's TLS handshake took 13 s while the owner kept
 * swiping: nothing moved, then the swipes played back as eight switches in
 * seven seconds. Now a switch after the step's first second says why nothing
 * moves, and the step's end moves one pet, the way the last one pointed. A
 * switch while control waits moves at once and says nothing more. */



/* The story screens never cut the pet off: opening and closing each one while
 * it listens, thinks or speaks cancels nothing, stops no audio and queues no
 * control step, and the turn finishes as if nothing happened. */
static void story_screens_keep_the_turn(void)
{
    static const pet_face_swipe_t swipes[]={PET_FACE_SWIPE_RIGHT,PET_FACE_SWIPE_LEFT,PET_FACE_SWIPE_UP};
    boot(true);screens_opened=0;
    unsigned commands_before=command_count;
    tap();cue_done();assert(capturing&&face_state==PET_FACE_LISTENING);
    for(unsigned i=0;i<3;++i){face_swiped(swipes[i]);return_requested();}
    assert(capturing&&face_state==PET_FACE_LISTENING&&screens_opened==3);
    deliver((audio_event_t){.kind=AUDIO_CAPTURE_DONE,.stream=s_input_stream,.sequence=9,.duration=2000,.result=ESP_OK,
        .generation=atomic_load(&s_generation)});
    capturing=false;assert(face_state==PET_FACE_THINKING);
    for(unsigned i=0;i<3;++i){face_swiped(swipes[i]);return_requested();}
    assert(face_state==PET_FACE_THINKING&&atomic_load(&s_turn_active));
    assert(audio_start(105,24000)==ESP_OK&&playing&&face_state==PET_FACE_SPEAKING);
    for(unsigned i=0;i<3;++i){
        face_swiped(swipes[i]);assert(playing&&face_state==PET_FACE_SPEAKING);
        assert(audio_chunk(105,i,(const uint8_t *)"\0\0",2)==ESP_OK);
        return_requested();assert(playing&&face_state==PET_FACE_SPEAKING);
    }
    assert(screens_opened==9&&!cancels&&!freezes&&!error_cues&&command_count==commands_before);
    assert(atomic_load(&s_voice_allowed)&&!atomic_load(&s_voice_paused)&&!atomic_load(&s_revalidate));
    audio_end(105,2);playing=false;
    deliver((audio_event_t){.kind=AUDIO_PLAYBACK_DONE,.stream=105,.generation=atomic_load(&s_generation)});
    assert(face_state==PET_FACE_IDLE&&!atomic_load(&s_turn_active)&&!pet_runtime_owns(&s_resources,PET_RESOURCE_VOICE));
    assert(!cancels&&!freezes&&!error_cues);
}

/* In story mode a long press pauses the story without a turn: a reply that
 * is playing goes on to its end, and nothing is cancelled or stopped. */
static void a_story_pause_lets_the_reply_play(void)
{
    boot(true);story_mode=true;pauses=0;
    tap();cue_done();
    deliver((audio_event_t){.kind=AUDIO_CAPTURE_DONE,.stream=s_input_stream,.sequence=9,.duration=2000,.result=ESP_OK,
        .generation=atomic_load(&s_generation)});
    capturing=false;
    assert(audio_start(106,24000)==ESP_OK&&playing&&face_state==PET_FACE_SPEAKING);
    deliver((audio_event_t){.kind=AUDIO_PET,.generation=atomic_load(&s_generation)});
    assert(pauses==1&&playing&&face_state==PET_FACE_SPEAKING&&atomic_load(&s_turn_active)&&!cancels);
    assert(audio_chunk(106,0,(const uint8_t *)"\0\0",2)==ESP_OK);
    audio_end(106,0);playing=false;
    deliver((audio_event_t){.kind=AUDIO_PLAYBACK_DONE,.stream=106,.generation=atomic_load(&s_generation)});
    assert(face_state==PET_FACE_IDLE&&!atomic_load(&s_turn_active)&&!cancels&&!error_cues&&pauses==1);
    story_mode=false;
}
/* The story frame shows while the pet on screen is the session's pet in story
 * mode: it goes when another pet comes on screen and returns with this one,
 * and a hello without story mode ends it. */


/* ---- Story events ------------------------------------------------------ */

/* story.events naming the event pack under each of `ids`, `bytes` long,
 * with the stinger `event_sound`. */
static const char *event_sound = "";
static void event_list(const char *const *ids, size_t count, uint32_t bytes)
{
    pet_story_events_t events = {.count = (uint8_t)count};
    for (size_t i = 0; i < count; ++i)
    {
        strcpy(events.events[i].id, ids[i]);
        strcpy(events.events[i].sha256, event_sha);
        strcpy(events.events[i].sound, event_sound);
        events.events[i].bytes = bytes;
    }
    pet_story_events_want(&events);
}
/* Nothing listed, cued or shown, and nothing counted. */
static void no_events(void)
{
    const pet_control_http_t http = {0};
    pet_story_events_done();
    event_shown = NULL;
    pet_story_events_want(&(const pet_story_events_t){0});
    pet_story_events_step(&http, 0);
    event_damage = SIZE_MAX;
    events_shown = events_stopped = event_downloads = ends_sent = 0;
}
/* Four seconds of control passes: time for every download due. */
static void control_passes(void)
{
    for (int i = 0; i < 40; ++i)
    {
        clock_us += 100000;
        pocket_tick((int64_t)clock_us);
    }
}
/* The gateway's story.event, as pet_network delivers it. */
static void story_cue(const char *id){story_event(id);drain();}
static const char *const wolves[] = {"wolves"};
/* A pack's room in the copy's tail. */
static size_t pack_room(void)
{
    return (event_length + 15) & ~(size_t)15;
}
/* Where the ready pack of the listed event `id` is kept: it is cued, taken
 * and let go. */
static const uint8_t *kept_at(const char *id)
{
    const void *pack = NULL;
    size_t bytes = 0;
    char sound[PET_SOUND_ID_BYTES + 1];
    assert(pet_story_events_cue(id) && pet_story_events_take(&pack, &bytes, sound) && bytes == event_length);
    pet_story_events_done();
    return pack;
}

/* Packs download between turns, one request each, in the list's order, into
 * the unused tail of the pet's copy past the check's workspace, never the
 * heap, each in the first gap that holds it: a list keeps what it still names,
 * forgets the rest and adds the new. All of a list's 16 are kept when they fit
 * (story-events-all-v1), and one with no room waits while the next downloads. */
static void story_events_download_between_turns(void)
{
    static const char *const first[] = {"a", "b", "c", "d", "e"}, *const next[] = {"e", "f", "g"};
    boot(true);
    no_events();
    atomic_store(&s_turn_active, true);
    event_list(first, 5, (uint32_t)event_length);
    control_passes();
    assert(!event_downloads);
    end_turn();
    control_passes();
    assert(event_downloads == 5);
    for (int i = 0; i < 5; ++i)
        assert(kept_at(first[i]) == copy_tail() + i * pack_room());
    /* a-d no longer listed: forgotten, and f and g take the first gaps. */
    event_list(next, 3, (uint32_t)event_length);
    assert(!pet_story_events_cue("a"));
    control_passes();
    assert(event_downloads == 7 && kept_at("f") == copy_tail() && kept_at("g") == copy_tail() + pack_room());
    assert(kept_at("e") == copy_tail() + 4 * pack_room());
    event_list(next, 3, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == 7);
    char names[PET_STORY_EVENTS][8];
    const char *all[PET_STORY_EVENTS];
    for (int i = 0; i < PET_STORY_EVENTS; ++i)
    {
        snprintf(names[i], sizeof(names[i]), "all-%d", i);
        all[i] = names[i];
    }
    no_events();
    event_list(all, PET_STORY_EVENTS, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == PET_STORY_EVENTS && kept_at(all[15]) == copy_tail() + 15 * pack_room());
    /* In the list's order, whichever entries the cache gave the events (here
     * while nothing fits): the event the story heads for downloads first. */
    static const char *const ahead[] = {"p", "q"}, *const turned[] = {"q", "p"};
    no_events();
    uint8_t *tail = copy_tail() - fp_validation_workspace_size();
    pet_story_events_region(NULL, 0);
    event_list(ahead, 2, (uint32_t)event_length);
    clock_us += 100000;
    pocket_tick((int64_t)clock_us);
    pet_story_events_region(tail, fp_validation_workspace_size() + pack_room());
    event_list(turned, 2, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == 1 && kept_at("q") == copy_tail() && !pet_story_events_cue("p"));
}
/* The packs live in the tail of the copy of the pet on screen: a tail with no
 * room for one keeps its event narration, ended at once (story.event.ended),
 * and the pack downloads once there is room. Another pet on screen forgets
 * every pack, which downloads again into its tail. */

/* A pack that is not exactly the one named is refused at once and for good,
 * its milestone staying narration, even when story.events names it again: a
 * valid event pack whose sha256 is another, one byte off, another size. */
static void a_wrong_event_pack_is_refused(void)
{
    char named[65];
    boot(true);
    no_events();
    strcpy(named, event_sha);
    memset(event_sha, '0', 64);
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == 1 && !pet_story_events_cue("wolves"));
    clock_us += 120000000;
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == 1 && !pet_story_events_cue("wolves"));
    strcpy(event_sha, named);
    story_cue("wolves");
    assert(!events_shown);
    no_events();
    event_damage = event_length / 2;
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == 1 && !pet_story_events_cue("wolves"));
    no_events();
    event_list(wolves, 1, (uint32_t)event_length + 16);
    control_passes();
    assert(event_downloads == 1 && !pet_story_events_cue("wolves"));
}
/* A download cut off halfway keeps nothing and is tried again from the start
 * after the server's retry delay; three failures give up until story.events
 * names the event again. */
static void a_failed_download_is_tried_again(void)
{
    boot(true);
    no_events();
    cut_downloads = 1;
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == 1 && !pet_story_events_cue("wolves"));
    clock_us += 30000000;
    control_passes();
    assert(event_downloads == 2 && pet_story_events_cue("wolves"));
    pet_story_events_uncue();
    no_events();
    cut_downloads = 3;
    event_list(wolves, 1, (uint32_t)event_length);
    for (int i = 0; i < 5; ++i)
    {
        control_passes();
        clock_us += 30000000;
    }
    assert(event_downloads == 3 && !cut_downloads && !pet_story_events_cue("wolves"));
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    assert(event_downloads == 4 && pet_story_events_cue("wolves"));
    pet_story_events_uncue();
    /* An abandoned first event releases the only available reservation. */
    no_events();
    uint8_t *tail = copy_tail() - fp_validation_workspace_size();
    pet_story_events_region(tail, fp_validation_workspace_size() + pack_room());
    static const char *const two[] = {"first", "second"};
    event_list(two, 2, (uint32_t)event_length);
    cut_downloads = 3;
    for (int i = 0; i < 4; ++i)
    {
        control_passes();
        clock_us += 30000000;
    }
    assert(!pet_story_events_cue("first") && pet_story_events_cue("second"));
    pet_story_events_uncue();
}
/* story.event follows the milestone's output.audio.end while the speaker
 * still plays: the event shows once the speech has drained, with its stinger
 * (sound-bank-v1), and the pet's idle after it changes nothing;
 * story.event.ended follows once it has played.
 * The unapproved event pack shows as an event and is refused as a pet. An id
 * not listed, or one whose pack is not ready, is narration only, ended at
 * once. */
static void a_story_event_waits_for_the_speech(void)
{
    boot(true);
    no_events();
    event_sound = "danger-ridge";
    event_list(wolves, 1, (uint32_t)event_length);
    event_sound = "";
    stingers = 0;
    story_cue("wolves");
    assert(!events_shown && ends_sent == 1 && !strcmp(end_sent, "wolves") && !stingers);
    control_passes();
    assert(audio_start(201, 24000) == ESP_OK && playing);
    assert(audio_chunk(201, 0, (const uint8_t *)"\0\0", 2) == ESP_OK);
    audio_end(201, 0);
    story_cue("wolves");
    assert(!events_shown && face_state == PET_FACE_SPEAKING && !stingers);
    playing = false;
    deliver((audio_event_t){.kind = AUDIO_PLAYBACK_DONE, .stream = 201, .generation = atomic_load(&s_generation)});
    assert(events_shown == 1 && event_shown && face_state == PET_FACE_IDLE);
    assert(stingers == 1 && !strcmp(stinger, "danger-ridge"));
    state_changed("idle");
    drain();
    assert(event_shown && !events_stopped && ends_sent == 1);
    event_ends();
    assert(ends_sent == 2 && !strcmp(end_sent, "wolves"));
    story_cue("wolves");
    assert(events_shown == 2 && event_shown && stingers == 2);
    event_ends();
    story_cue("mirra-rises");
    assert(events_shown == 2 && !event_shown && ends_sent == 4 && !strcmp(end_sent, "mirra-rises"));
    pet_pack_verified_manifest_t manifest = {.bytes = (uint32_t)event_length};
    strcpy(manifest.face_id, "event-wolves");
    strcpy(manifest.version, "fixture-240-v2");
    assert(!prepare_pack(1, event_pack, event_length, &manifest, false, false));
}
/* A tap or a long press gives the screen back to the pet and does nothing
 * else; the next tap listens. A new reply ends it too. Each ends it with
 * story.event.ended. */
static void a_tap_ends_the_story_event(void)
{
    boot(true);
    no_events();
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    story_cue("wolves");
    assert(event_shown);
    tap();
    assert(!event_shown && events_stopped == 1 && !listen_cues && face_state == PET_FACE_IDLE && ends_sent == 1);
    tap();
    assert(listen_cues == 1 && face_state == PET_FACE_LISTENING);
    tap(); /* cancels the listen during its cue */
    story_cue("wolves");
    deliver((audio_event_t){.kind = AUDIO_PET, .generation = atomic_load(&s_generation)});
    assert(!event_shown && events_stopped == 2 && face_state == PET_FACE_IDLE && ends_sent == 2);
    story_cue("wolves");
    assert(audio_start(202, 24000) == ESP_OK && !event_shown && events_stopped == 3 && face_state == PET_FACE_SPEAKING);
    audio_end(202, 0);
    playing = false;
    deliver((audio_event_t){.kind = AUDIO_PLAYBACK_DONE, .stream = 202, .generation = atomic_load(&s_generation)});
    assert(events_shown == 3 && !event_shown && ends_sent == 3);
}
/* The cue arrives while a tap waits to listen: it never shows over the
 * listen, and the microphone drops it. One on screen ends with a rebind (the
 * voice grant's renewal over the open session) and with install detachment. */
static void the_microphone_or_a_rebind_ends_it(void)
{
    boot(true);
    no_events();
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    reply_heard(203);
    assert(pet_runtime_claim(&s_resources, PET_RESOURCE_CONTROL));
    tap();
    assert(atomic_load(&s_tap_pending));
    story_cue("wolves");
    assert(!event_shown);
    pet_runtime_release(&s_resources, PET_RESOURCE_CONTROL);
    retry();
    cue_done();
    assert(capturing && face_state == PET_FACE_LISTENING && !event_shown);
    end_turn();
    deliver((audio_event_t){.kind = AUDIO_STORY_EVENT, .generation = atomic_load(&s_generation)});
    assert(!events_shown);
    story_cue("wolves");
    assert(event_shown);
    clock_us += 9000000; /* past the conversation grace */
    atomic_store(&s_brain_expires_ms, (uint32_t)(clock_us / 1000) + 30000);
    independent_updates(clock_us / 1000);
    assert(!event_shown && events_stopped == 1 && rebinds == 1);
    rebind_accepted();
    story_cue("wolves");
    assert(event_shown);
    assert(hold_shown(false));
    assert(!event_shown && events_stopped == 2 && ends_sent == 3);
}

/* The pet answers on `stream`: a question heard, then its speech. */
static void answering(uint32_t stream)
{
    tap();
    cue_done();
    deliver((audio_event_t){.kind = AUDIO_CAPTURE_DONE, .stream = s_input_stream, .sequence = 9, .duration = 2000,
                            .result = ESP_OK, .generation = atomic_load(&s_generation)});
    capturing = false;
    assert(audio_start(stream, 24000) == ESP_OK && playing && face_state == PET_FACE_SPEAKING);
}
/* In story mode, with a gateway that holds speech (speech-pause-v1), a tap
 * holds the pet's answer at once and sends output.pause, and the next lets it
 * play on with output.resume; nothing is cancelled. Frames still in flight
 * never wait for the speaker's room while it is held. The milestone's event
 * cued meanwhile is kept, and shows once the speech has drained. */
static void a_story_tap_holds_the_reply(void)
{
    boot(true);
    no_events();
    story_mode = speech_pause_offered = true;
    output_pauses = output_resumes = 0;
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    answering(301);
    clock_us += 1000000;
    tap();
    assert(held && output_pauses == 1 && output_stream == 301 && !strcmp(announced, "> paused"));
    assert(playing && face_state == PET_FACE_SPEAKING && atomic_load(&s_turn_active) && !cancels);
    queue_full = true;
    unsigned waits = delays;
    assert(audio_chunk(301, 0, (const uint8_t *)"\0\0", 2) == ESP_OK && delays == waits);
    queue_full = false;
    audio_end(301, 0);
    story_cue("wolves");
    clock_us += 2000000;
    tap();
    assert(!held && output_resumes == 1 && output_stream == 301 && !strcmp(announced, "> playing"));
    assert(playing && !cancels && !events_shown && !ends_sent);
    playing = false;
    deliver((audio_event_t){.kind = AUDIO_PLAYBACK_DONE, .stream = 301, .generation = atomic_load(&s_generation)});
    assert(events_shown == 1 && event_shown && face_state == PET_FACE_IDLE);
    event_ends();
    assert(ends_sent == 1 && !strcmp(end_sent, "wolves"));
    story_mode = speech_pause_offered = false;
}
/* A tap within 350 ms of the one before stops the answer, as a tap did before:
 * a double tap holds it, then stops it with input.cancel, and an event the
 * answer cued ends after the cancel, so the gateway never speaks a reaction to
 * it. Outside story mode, without the gateway's speech-pause-v1, or once the
 * answer's last audio drains, a tap stops it at once. */
static void a_double_tap_stops_the_reply(void)
{
    boot(true);
    no_events();
    story_mode = speech_pause_offered = true;
    output_pauses = output_resumes = 0;
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    answering(302);
    const uint32_t asked = s_input_stream;
    audio_end(302, 0);
    story_cue("wolves");
    clock_us += 1000000;
    tap();
    assert(held && output_pauses == 1 && !ends_sent && !cancels);
    clock_us += 200000;
    tap();
    assert(!playing && !held && cancels == 1 && last_cancel == asked && face_state == PET_FACE_IDLE);
    assert(!atomic_load(&s_turn_active) && ends_sent == 1 && cancels_before_end == 1 && !events_shown);
    for (unsigned i = 0; i < 3; ++i)
    {
        story_mode = i != 0;
        speech_pause_offered = i != 1;
        draining = i == 2;
        clock_us += 1000000;
        answering(303 + i);
        clock_us += 1000000;
        tap();
        assert(!playing && !held && cancels == 2 + i && output_pauses == 1);
    }
    story_mode = speech_pause_offered = draining = false;
}

/* The capture of the current listen has ended with what the owner said. */
static void listen_sent(void)
{
    deliver((audio_event_t){.kind = AUDIO_CAPTURE_DONE, .stream = s_input_stream, .sequence = 99, .duration = 3000,
                            .result = ESP_OK, .generation = atomic_load(&s_generation)});
}
/* With a gateway that keeps a held reply while the owner talks over it
 * (speech-pause-listen-v1), a tap in story mode holds the answer and listens
 * at once, as a tap does when idle: output.pause, the listening face and cue,
 * then the microphone. A tap sends what was said, and one while the gateway
 * hears it is ignored. Its reply's start drops the held answer, which the
 * gateway has dropped too: nothing is cancelled or played on. */
static void a_story_tap_listens_over_the_reply(void)
{
    boot(true);
    no_events();
    story_mode = speech_pause_offered = listen_offered = true;
    output_pauses = output_resumes = 0;
    answering(401);
    const unsigned listens = listen_cues, starts = input_starts;
    clock_us += 1000000;
    tap();
    assert(held && playing && output_pauses == 1 && output_stream == 401 && !cancels);
    assert(face_state == PET_FACE_LISTENING && listen_cues == listens + 1 && input_starts == starts);
    cue_done();
    assert(capturing && input_starts == starts + 1);
    clock_us += 3000000;
    tap();
    assert(!capturing);
    listen_sent();
    assert(face_state == PET_FACE_THINKING && held && !cancels);
    clock_us += 1000000;
    tap();
    assert(face_state == PET_FACE_THINKING && held && !cancels && !output_resumes);
    assert(audio_start(402, 16000) == ESP_OK && playing && !held && face_state == PET_FACE_SPEAKING);
    assert(!s_held_answer && s_output_stream == 402 && !cancels && !output_resumes);
    story_mode = speech_pause_offered = listen_offered = false;
}
/* Nothing said over the held answer: the gateway hears no speech (NO_SPEECH)
 * and the answer, its turn again, plays on from where it stopped
 * (output.resume), with no error cue; the gateway's idle after it changes
 * nothing. A tap during the listening cue cancels the listen before its
 * microphone opens, and the answer plays on too. The milestone's event the
 * answer cued waits through both, and shows once its speech has drained. */
static void a_silent_listen_plays_the_reply_on(void)
{
    boot(true);
    no_events();
    story_mode = speech_pause_offered = listen_offered = true;
    output_pauses = output_resumes = 0;
    event_list(wolves, 1, (uint32_t)event_length);
    control_passes();
    answering(411);
    const uint32_t asked = s_input_stream;
    audio_end(411, 0);
    story_cue("wolves");
    clock_us += 1000000;
    tap();
    cue_done();
    assert(held && capturing && !ends_sent);
    clock_us += 3000000;
    tap();
    listen_sent();
    const unsigned cues = sounds;
    session_error("NO_SPEECH", "I didn't catch that", true);
    drain();
    assert(!held && playing && output_resumes == 1 && output_stream == 411 && face_state == PET_FACE_SPEAKING);
    assert(s_input_stream == asked && !s_held_answer && !cancels && sounds == cues);
    state_changed("idle");
    drain();
    assert(face_state == PET_FACE_SPEAKING && atomic_load(&s_turn_active));
    clock_us += 1000000;
    tap();
    assert(held && output_pauses == 2 && face_state == PET_FACE_LISTENING && s_listen_token);
    clock_us += 1000000;
    tap();
    assert(!held && output_resumes == 2 && face_state == PET_FACE_SPEAKING && !s_listen_token && !capturing);
    assert(s_input_stream == asked && !cancels && !ends_sent && !events_shown);
    playing = false;
    deliver((audio_event_t){.kind = AUDIO_PLAYBACK_DONE, .stream = 411, .generation = atomic_load(&s_generation)});
    assert(events_shown == 1 && event_shown && face_state == PET_FACE_IDLE);
    event_ends();
    assert(ends_sent == 1 && !strcmp(end_sent, "wolves"));
    story_mode = speech_pause_offered = listen_offered = false;
}
/* A double tap still skips the answer: within 350 ms of the tap that held it
 * and began the listen, it stops the answer and ends the listen. During the
 * listening cue only the answer is cancelled; once the microphone is open,
 * the listen's input is cancelled first. */
static void a_double_tap_skips_the_held_reply(void)
{
    boot(true);
    no_events();
    story_mode = speech_pause_offered = listen_offered = true;
    answering(421);
    const uint32_t asked = s_input_stream;
    clock_us += 1000000;
    tap();
    assert(held && s_listen_token);
    clock_us += 200000;
    tap();
    assert(!playing && !held && cancels == 1 && last_cancel == asked && face_state == PET_FACE_IDLE);
    assert(!s_listen_token && !s_held_answer && !atomic_load(&s_turn_active));
    answering(422);
    const uint32_t second = s_input_stream;
    clock_us += 1000000;
    tap();
    cue_done();
    const uint32_t listen = s_input_stream;
    clock_us += 200000;
    tap();
    assert(!capturing && !playing && !held && cancels == 3 && previous_cancel == listen && last_cancel == second);
    assert(!s_held_answer && !atomic_load(&s_turn_active) && face_state == PET_FACE_IDLE);
    story_mode = speech_pause_offered = listen_offered = false;
}

/* Game cues (story.sound) wait for the next reply, which holds its speech
 * while they play: the gateway is asked to pause it, then to send it on once
 * they have played, unless the owner holds it then. Nothing is queued while
 * the voice is off. */
static void cues_hold_the_reply(void)
{
    boot(true);
    speech_pause_offered = true;
    output_pauses = output_resumes = cues_queued = 0;
    story_sound("dice-wood-1");
    assert(cues_queued == 1);
    assert(audio_start(205, 16000) == ESP_OK && held && output_pauses == 1 && output_stream == 205);
    held = false; /* The cues have played. */
    sounds_played(205);
    assert(output_resumes == 1 && output_stream == 205);
    assert(audio_start(206, 16000) == ESP_OK && !held && output_pauses == 1);
    story_sound("hit-stab");
    assert(audio_start(207, 16000) == ESP_OK && held && output_pauses == 2);
    sounds_played(207); /* Held by the owner as they end. */
    assert(output_resumes == 1);
    atomic_store(&s_voice_allowed, false);
    story_sound("miss-swish");
    assert(cues_queued == 2);
    speech_pause_offered = held = false;
}

/* The BOOT button talks: held, the pet listens as a tap makes it, once per
 * hold; let go, the microphone stops, which sends. Let go during the
 * listening cue and nothing starts. Pressed while the pet speaks, the answer
 * stops and the hold listens after. While the session connects a press waits
 * as a tap does, and letting go withdraws it: the microphone never opens. */
static void talk(bool held)
{deliver((audio_event_t){.kind=AUDIO_TALK,.stream=held,.generation=atomic_load(&s_generation)});}
static void the_boot_button_talks(void)
{
    boot(false);
    talk(true);
    assert(atomic_load(&s_tap_pending)&&face_state==PET_FACE_CONNECTING&&!strcmp(announced,"> connecting..."));
    talk(false);
    assert(!atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&!strcmp(announced,"> cancelled"));
    gateway_hello(true);retry();assert(!listen_cues&&!capture_starts&&!input_starts);

    talk(true);assert(listen_cues==1&&face_state==PET_FACE_LISTENING&&s_listen_token&&!capture_starts);
    talk(true);assert(listen_cues==1);
    cue_done();assert(capture_starts==1&&input_starts==1&&capturing);
    talk(true);assert(capturing);
    talk(false);assert(!capturing);
    end_turn();

    talk(true);assert(listen_cues==2&&s_listen_token);
    talk(false);
    assert(!s_listen_token&&face_state==PET_FACE_IDLE&&!atomic_load(&s_turn_active)&&capture_starts==1);

    answering(401);
    const unsigned cues=listen_cues,before=cancels;
    talk(true);assert(cancels==before+1&&!playing&&face_state==PET_FACE_IDLE&&listen_cues==cues);
    talk(true);assert(listen_cues==cues+1&&face_state==PET_FACE_LISTENING);
    talk(false);assert(!s_listen_token&&!capturing);
}
static void partial_story_event(void)
{
    assert(event_length > 32768u);
    boot(true);
    no_events();
    event_list(wolves, 1, (uint32_t)event_length);
    pet_control_http_t http = {.origin = s_origin, .device_id = s_id, .credential = s_credential};
    assert(pet_story_events_step(&http, 1000));
    assert(!pet_story_events_cue("wolves"));
    cut_downloads = 1;
    assert(pet_story_events_step(&http, 1001));
    assert(!pet_story_events_step(&http, 1002) && !pet_story_events_cue("wolves"));
    unsigned chunks = (unsigned)((event_length + 32767u) / 32768u);
    for (unsigned i = 1; i < chunks; i++)
    {
        assert(!pet_story_events_cue("wolves"));
        assert(pet_story_events_step(&http, 32000 + i));
    }
    assert(event_downloads == chunks + 1 && pet_story_events_cue("wolves"));
    pet_story_events_uncue();
}

static const struct {const char *name;void (*run)(void);} scenarios[]={

    {"tap_waits_for_the_session",tap_waits_for_the_session},




    {"management_answer_before_the_session_waits",management_answer_before_the_session_waits},

    {"tap_waits_for_a_control_step",tap_waits_for_a_control_step},
    {"control_yields_to_a_waiting_tap",control_yields_to_a_waiting_tap},
    {"a_follow_up_tap_listens_at_once",a_follow_up_tap_listens_at_once},
    {"a_stalled_listen_says_so",a_stalled_listen_says_so},
    {"brain_disconnect_releases_voice",brain_disconnect_releases_voice},
    {"brain_failure_and_renewal",brain_failure_and_renewal},
    {"story_screens_keep_the_turn",story_screens_keep_the_turn},


    {"a_story_pause_lets_the_reply_play",a_story_pause_lets_the_reply_play},

    {"story_events_download_between_turns",story_events_download_between_turns},

    {"a_wrong_event_pack_is_refused",a_wrong_event_pack_is_refused},
    {"a_failed_download_is_tried_again",a_failed_download_is_tried_again},
    {"a_story_event_waits_for_the_speech",a_story_event_waits_for_the_speech},
    {"a_tap_ends_the_story_event",a_tap_ends_the_story_event},
    {"the_microphone_or_a_rebind_ends_it",the_microphone_or_a_rebind_ends_it},
    {"a_story_tap_holds_the_reply",a_story_tap_holds_the_reply},
    {"a_double_tap_stops_the_reply",a_double_tap_stops_the_reply},
    {"a_story_tap_listens_over_the_reply",a_story_tap_listens_over_the_reply},
    {"a_silent_listen_plays_the_reply_on",a_silent_listen_plays_the_reply_on},
    {"a_double_tap_skips_the_held_reply",a_double_tap_skips_the_held_reply},
    {"cues_hold_the_reply",cues_hold_the_reply},
    {"the_boot_button_talks",the_boot_button_talks},
};
int main(int argc,char **argv)
{
    pack_length=fread(pack_bytes,1,sizeof(pack_bytes),stdin);assert(pack_length&&pack_length<sizeof(pack_bytes));
    FILE *event_file=fopen(STORY_EVENT_PACK,"rb");assert(event_file);
    event_length=fread(event_pack,1,sizeof(event_pack),event_file);fclose(event_file);
    assert(event_length&&event_length<sizeof(event_pack));hex_sha256(event_pack,event_length,event_sha);
    assert(pet_story_events_init(pet_network_story_event_ended));
    s_audio_lock=xSemaphoreCreateMutex();s_audio_events=(QueueHandle_t)2;s_commands=(QueueHandle_t)3;
    atomic_store(&s_identity_ready,true);s_boot_health_decided=true;s_audio_started=s_face_started=true;
    strcpy(s_id,"00000000-0000-4000-8000-000000000001");s_origin="https://fixture.invalid";
    strcpy(signed_release.account_id,"00000000-0000-4000-8000-000000000003");
    strcpy(signed_release.project_id,"00000000-0000-4000-8000-000000000004");
    strcpy(signed_release.face_id,"big-sal");strcpy(signed_release.version,"fixture-240-v2");
    signed_release.imported=false;s_wifi.volume=40;s_wifi.brightness=80;
    if (argc > 1 && !strcmp(argv[1], "partial-story-event"))
    {
        partial_story_event();
        return 0;
    }
    /* Every scenario by default; one by name to see where each one stands. */
    unsigned ran=0;
    for(size_t i=0;i<sizeof(scenarios)/sizeof(scenarios[0]);++i)
        if(argc<2||!strcmp(argv[1],scenarios[i].name)){scenarios[i].run();printf("ok %s\n",scenarios[i].name);++ran;}
    assert(ran);
    s_stream = NULL;
    free(s_shown_pack);
    s_shown_pack = NULL;
    return 0;
}
