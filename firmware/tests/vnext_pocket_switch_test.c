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
static uint32_t last_cancel;
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
esp_err_t pet_network_cancel(uint32_t stream){++cancels;last_cancel=stream;return ESP_OK;}
static esp_err_t input_start_result=ESP_OK;
esp_err_t pet_network_input_start(uint32_t stream,const char *ai_pet_id){(void)stream;(void)ai_pet_id;++input_starts;return input_start_result;}
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
static void rebind_refused(void)
{
    assert(socket_state.pending);socket_state.pending=false;
    session_error(PET_SESSION_REBIND_REFUSED,"Reconnecting to switch pets",true);
    socket_state.open=socket_state.hello=false;connected(false); /* The gateway closes the socket. */
}

/* The store: every slot holds the fixture pack and, unless unsigned, its signed record. */
esp_err_t pet_slot_store_open(pet_slot_store_t *s)
{
    memset(s,0,sizeof(*s));s->open=true;s->inventory.ready=true;s->inventory.state=stored;s->machine.state=stored.operation;
    assert(pet_flash_layout_known(PET_LAYOUT_THREE_3P5M,&s->layout));memset(s->partition_sha256,'c',64);return ESP_OK;
}
esp_err_t pet_slot_store_load(pet_slot_store_t *s,unsigned slot,void *buffer,size_t capacity,size_t *bytes)
{
    assert(s->open&&slot<PET_SLOT_COUNT&&buffer==s_shown_pack&&capacity==PET_LAYOUT_THREE_SLOT_PACK_BYTES&&!drawing_copy);
    if(s->inventory.state.slots[slot].state!=PET_SLOT_READY)return ESP_ERR_INVALID_STATE;
    face_during_copy=face_state;
    memcpy(buffer,pack_bytes,pack_length);*bytes=pack_length;return ESP_OK;
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
void pet_face_pack_hold_external(void){drawing_copy=false;}
void pet_face_pack_release_external(void){drawing_copy=false;}
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
bool pet_audio_playback_queue_healthy(void){return true;}
void pet_audio_playback_cancel(void){playing=false;}
esp_err_t pet_audio_playback_start(uint32_t stream,uint32_t rate,pet_realtime_boost_t boost,pet_speech_mouth_mode_t mouth)
{(void)stream;(void)rate;(void)boost;(void)mouth;playing=true;return ESP_OK;}
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
int64_t esp_timer_get_time(void){return (int64_t)clock_us;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out){(void)args;*out=NULL;return ESP_FAIL;}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t timeout){(void)timer;(void)timeout;return ESP_OK;}
uint32_t esp_random(void){return 7;}
void esp_fill_random(void *buffer,size_t bytes){memset(buffer,7,bytes);}
void *heap_caps_aligned_alloc(size_t alignment,size_t size,unsigned caps)
{(void)caps;return aligned_alloc(alignment,(size+alignment-1)/alignment*alignment);}
void heap_caps_free(void *memory){free(memory);}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps){(void)caps;return calloc(count,size);}
static bool display_locked;
esp_err_t pet_display_lock(int timeout){(void)timeout;assert(!display_locked);display_locked=true;return ESP_OK;}
void pet_display_unlock(void){assert(display_locked);display_locked=false;}
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
void vTaskDelay(TickType_t ticks){(void)ticks;}

/* One audio-task pass: the event, then the send it asks for. */
static void deliver(audio_event_t event)
{
    audio_send_t send={.kind=SEND_NONE};
    if(!audio_event_current(&event))return;
    handle_audio(&event,&send);finish_send(&send,&event);
}
static void drain(void){while(queued_count){audio_event_t e=queued[0];memmove(queued,queued+1,--queued_count*sizeof(*queued));deliver(e);}}
static void tap(void){deliver((audio_event_t){.kind=AUDIO_TAP,.generation=atomic_load(&s_generation)});}
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
static const char *builds[PET_SLOT_COUNT]={"00000000-0000-4000-8000-000000000040","00000000-0000-4000-8000-000000000041",
    "00000000-0000-4000-8000-000000000042"};
/* Three signed cloud pets; slot 1 shows and is bound, and its session is open:
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
    three.active=1;three.selection_revision=3;
    assert(pet_slot_inventory_rebind(&three,1,"binding-41","00000000-0000-4000-8000-000000000050","20"));
    stored=three;memset(&socket_state,0,sizeof(socket_state));
    free(s_shown_pack);s_shown_pack=NULL;s_shown_slot=-1;s_selection_save_at=0;drawing_copy=false;
    memset(&s_prepared,0,sizeof(s_prepared));
    s_select_moment=SELECT_NONE;s_select_refused_build[0]=0;
    pocket_control_start();assert(s_shown_slot==1&&s_slots.inventory.state.bound==1);
    s_pet=&pet_worker;s_firmware=&firmware_worker;memset(&pet_worker,0,sizeof(pet_worker));
    pet_worker.has_context=pet_worker.authenticated=true;
    pet_control_context_t *c=&pet_worker.cloud.context;strcpy(c->device_id,s_id);strcpy(c->account_id,signed_release.account_id);
    c->binding=(pet_control_binding_t){.assigned=true,.revision="binding-41",.relationship_id="00000000-0000-4000-8000-000000000050"};
    strcpy(c->binding.build_id,builds[1]);strcpy(c->binding.sha256,three.slots[1].pack.sha256);
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
static void settle(void){clock_us+=3000000;pocket_tick((int64_t)clock_us);}

/* A swipe keeps the session and says CONNECTING, not NOT LINKED; the
 * previous pet's late answer is dropped quietly. */
static void swipe_keeps_session(void)
{
    boot(true);
    atomic_store(&s_turn_active,true);s_input_stream=77;capturing=true;
    swipe(1);
    assert(s_shown_slot==2&&!freezes&&socket_state.open&&!capturing&&cancels==1&&last_cancel==77);
    assert(!atomic_load(&s_voice_allowed)&&atomic_load(&s_voice_paused));
    assert(!strcmp(tile(),"CONNECTING")&&visible_status.pet_linked&&atomic_load(&s_voice_block)==VOICE_BLOCK_CONNECTING);
    assert(audio_start(91,24000)!=ESP_OK&&audio_chunk(91,0,(const uint8_t *)"\0\0",2)==ESP_OK);audio_end(91,0);
    assert(!atomic_load(&s_revalidate));
    // An answer from that session does not give the new pet a voice.
    connected(true);assert(!atomic_load(&s_voice_allowed));
}
/* The message a tap types while the open session moves to the pet on screen. */
static bool switching_message(void)
{
    char expected[PET_FACE_TERMINAL_MAX+1];
    strlcpy(expected,"> switching to ",sizeof(expected));strlcat(expected,s_shown_name,sizeof(expected));
    return s_shown_name[0]&&!strcmp(announced,expected);
}
/* The swipe settles: the cloud selects the pet and the session moves to it
 * with a rebind, not a new socket. The pet stays idle on screen, never the
 * connecting face. A tap meanwhile waits calmly, with the microphone closed,
 * says it is switching, and listens once the rebind is accepted. */
static void selection_rebinds(void)
{
    boot(true);swipe(1);settle();
    assert(selects==1&&s_slots.inventory.state.bound==2&&!pet_worker.admitted&&!freezes);
    mount();
    assert(rebinds==1&&!starts&&!freezes&&socket_state.pending&&same_context(&socket_state.asked,&pet_worker.cloud.context));
    assert(!strcmp(socket_state.asked.binding.build_id,builds[2])&&!atomic_load(&s_voice_allowed));
    assert(atomic_load(&s_handshake_pending)&&s_handshake_deadline==(int64_t)clock_us+REBIND_TIMEOUT_US);
    assert(!strcmp(tile(),"CONNECTING")&&face_state==PET_FACE_IDLE&&atomic_load(&s_rebinding));
    unsigned before_sounds=sounds;
    tap();
    assert(atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&switching_message()&&sounds==before_sounds);
    retry();assert(atomic_load(&s_tap_pending)&&!capture_starts&&!input_starts&&!listen_cues);
    rebind_accepted();
    assert(atomic_load(&s_voice_allowed)&&!atomic_load(&s_handshake_pending)&&face_state==PET_FACE_IDLE&&!atomic_load(&s_rebinding));
    assert(!strcmp(tile(),"READY"));
    retry();assert(!atomic_load(&s_tap_pending)&&listen_cues==1&&face_state==PET_FACE_LISTENING&&!capture_starts);
    cue_done();assert(capture_starts==1&&input_starts==1&&capturing);
    assert(!error_cues&&!starts&&!freezes&&rebinds==1);
    end_turn();
}
/* A tap 2.4 s after the swipe, while the swipe settles and the cloud selects
 * (the 26 Sep capture): it waits for the whole switch however long, past the
 * 5 s a connecting tap waits, and listens once the rebind is accepted. The
 * face never shows connecting. */
static void tap_waits_for_the_whole_switch(void)
{
    boot(true);swipe(1);
    clock_us+=900000;tap();
    assert(atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&switching_message()&&!listen_cues);
    // Tapping again only says to hold on: the tap still waits for the switch.
    tap();assert(atomic_load(&s_tap_pending)&&!strcmp(announced,"> hold on, almost ready")&&face_state==PET_FACE_IDLE);
    for(int t=0;t<60;++t){clock_us+=100000;retry();assert(face_state==PET_FACE_IDLE&&atomic_load(&s_tap_pending));}
    settle(); /* 3 s more: the selection */
    assert(selects==1&&atomic_load(&s_tap_pending));
    mount();assert(rebinds==1&&face_state==PET_FACE_IDLE&&atomic_load(&s_tap_pending));
    retry();assert(atomic_load(&s_tap_pending)&&!listen_cues);
    rebind_accepted();retry();
    assert(!atomic_load(&s_tap_pending)&&listen_cues==1&&face_state==PET_FACE_LISTENING);
    cue_done();assert(capture_starts==1&&!starts&&!freezes&&!error_cues);
    end_turn();
}
/* A pet the cloud refuses to select: a tap that waits for the switch hears
 * why as soon as it is known, not after its wait. */
static void a_refused_switch_says_so(void)
{
    boot(true);swipe(1);tap();assert(atomic_load(&s_tap_pending)&&switching_message());
    select_answer=PET_REPLACE_SELECT_REFUSED;settle();select_answer=PET_REPLACE_SELECT_CHOSEN;
    assert(!strcmp(tile(),"NOT LINKED")); /* publish_status sets the reason */
    retry();
    assert(!atomic_load(&s_tap_pending)&&!strcmp(announced,"> not linked: no voice")&&last_sound==PET_SFX_NO_VOICE);
    assert(face_state==PET_FACE_IDLE&&!capture_starts&&!listen_cues);
}
/* Back on the pet the open session talks as: voice returns at once. */
static void swipe_back_resumes(void)
{
    boot(true);
    swipe(1);assert(s_shown_slot==2&&!atomic_load(&s_voice_allowed));
    swipe(-1);
    assert(s_shown_slot==1&&atomic_load(&s_voice_allowed)&&!rebinds&&!starts&&!freezes&&!disconnects);
    assert(!strcmp(tile(),"READY"));
}
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
static void refusal_reconnects(void)
{
    boot(true);swipe(1);settle();mount();assert(socket_state.pending&&rebinds==1);
    tap();assert(atomic_load(&s_tap_pending)&&switching_message()&&face_state==PET_FACE_IDLE);
    rebind_refused();drain();
    assert(!error_cues&&atomic_load(&s_revalidate)&&atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE);
    pocket_tick((int64_t)clock_us); /* Revalidation: freeze, then the worker reconciles. */
    assert(freezes>=1&&disconnects==1&&!pet_worker.admitted&&atomic_load(&s_tap_pending));
    mount();assert(starts==1&&rebinds==1); /* A new socket: a reconnect, which says so. */
    assert(face_state==PET_FACE_CONNECTING);
    gateway_hello(true);assert(atomic_load(&s_voice_allowed));
    retry();cue_done();assert(capture_starts==1&&input_starts==1&&!error_cues);
    end_turn();
}
/* No answer within the rebind window: reconnect. */
static void rebind_timeout_reconnects(void)
{
    boot(true);swipe(1);settle();mount();assert(socket_state.pending&&rebinds==1);
    clock_us+=REBIND_TIMEOUT_US-1000;pocket_tick((int64_t)clock_us);assert(!disconnects&&socket_state.open);
    clock_us+=1000;pocket_tick((int64_t)clock_us);
    assert(freezes>=1&&disconnects==1&&!pet_worker.admitted&&!socket_state.open);
}
/* An older gateway (no session-rebind-v1): the swipe still keeps the socket,
 * and the selection reconnects as today. */
static void older_gateway_reconnects(void)
{
    boot(true);socket_state.rebind=false;swipe(1);assert(!freezes&&socket_state.open);
    settle();mount();assert(!rebinds&&starts==1&&freezes==1);
}
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
static void rebind_applies_the_new_configuration(void)
{
    boot(true);
    unsigned answers=config_answers,stores=mouth_offset_stores;
    // After the hello: the first pet's configuration, mouth timing +20 ms.
    const pet_control_context_t first=pet_worker.cloud.context;
    desired_config(&first,true,20);
    assert(config_answers==answers+1&&config_applied&&config_echo.has_speech_mouth_offset&&config_echo.speech_mouth_offset_ms==20);
    assert(mouth_offset==20&&mouth_offset_stores==stores+1&&mouth_offset_kept&&mouth_offset_kept_ms==20);
    // The selection moves the open session to the next pet.
    swipe(1);settle();mount();assert(socket_state.pending&&rebinds==1);
    rebind_accepted();assert(atomic_load(&s_voice_allowed)&&strcmp(first.config.version,pet_worker.cloud.context.config.version));
    // Its configuration after the acceptance, mouth timing -30 ms: the same path.
    desired_config(&pet_worker.cloud.context,true,-30);
    assert(config_answers==answers+2&&config_applied&&config_echo.has_speech_mouth_offset&&config_echo.speech_mouth_offset_ms==-30);
    assert(mouth_offset==-30&&mouth_offset_stores==stores+2&&mouth_offset_kept&&mouth_offset_kept_ms==-30);
    assert(!atomic_load(&s_revalidate)&&!starts&&!freezes&&rebinds==1);
    // Without the field it means 0, as after a hello: applied, and NVS forgets it.
    desired_config(&pet_worker.cloud.context,false,0);
    assert(config_answers==answers+3&&config_applied&&!config_echo.has_speech_mouth_offset);
    assert(mouth_offset==0&&mouth_offset_stores==stores+3&&!mouth_offset_kept);
    // The previous pet's configuration no longer matches the session: nothing applies.
    desired_config(&first,true,40);
    assert(config_answers==answers+4&&!config_applied&&mouth_offset==0&&mouth_offset_stores==stores+3);
    assert(atomic_load(&s_revalidate));
}
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
static void management_answer_mid_switch_keeps_session(void)
{
    boot(true);
    unsigned answers=config_answers;
    const pet_control_context_t first=pet_worker.cloud.context;
    swipe(1);assert(atomic_load(&s_voice_paused));
    desired_config(&first,true,20);
    assert(!atomic_load(&s_revalidate)&&config_answers==answers&&!freezes&&socket_state.open);
    settle();mount();rebind_accepted();assert(atomic_load(&s_voice_allowed));
    desired_config(&first,true,20);
    assert(atomic_load(&s_revalidate));
}
/* A pet placed over USB has no cloud release: NOT LINKED once known, and a
 * tap says so with the no-voice cue. Until then it is CONNECTING. */
static void usb_pet_not_linked(void)
{
    boot(true);unsigned_slots=1u<<2;swipe(1);assert(!strcmp(tile(),"CONNECTING"));
    settle();assert(!selects&&!strcmp(tile(),"NOT LINKED")&&atomic_load(&s_voice_block)==VOICE_BLOCK_NOT_LINKED);
    tap();assert(!strcmp(announced,"> not linked: no voice")&&last_sound==PET_SFX_NO_VOICE&&!atomic_load(&s_tap_pending));
    unsigned_slots=0;
}

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
    // A swipe right after a reply switches at once: it needs control.
    reply_heard(103);pets=pet_steps;
    swipe(1);pocket_tick((int64_t)clock_us);assert(pet_steps==pets+1);
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
static void a_swipe_stops_the_voice_calmly(void)
{
    boot(true);
    tap();cue_done();assert(capture_starts==1&&capturing&&face_state==PET_FACE_LISTENING);
    clock_us+=4000000;swipe(1);
    assert(!capturing&&face_during_copy==PET_FACE_IDLE&&face_state==PET_FACE_IDLE);
    assert(!freezes&&socket_state.open&&!atomic_load(&s_revalidate));
    boot(true);
    tap();cue_done();
    deliver((audio_event_t){.kind=AUDIO_CAPTURE_DONE,.stream=s_input_stream,.sequence=9,.duration=2000,.result=ESP_OK,
        .generation=atomic_load(&s_generation)});
    capturing=false;assert(face_state==PET_FACE_THINKING);
    assert(audio_start(104,24000)==ESP_OK&&playing&&face_state==PET_FACE_SPEAKING);
    clock_us+=6800000;swipe(1);
    assert(!playing&&face_during_copy==PET_FACE_IDLE&&face_state==PET_FACE_IDLE&&cancels==1);
    assert(!freezes&&socket_state.open&&!atomic_load(&s_revalidate)&&atomic_load(&s_voice_paused));
}

/* Swipes while a cloud step holds the control task. On 1 Oct a step's TLS
 * handshake took 13 s while the owner kept swiping: nothing moved, then the
 * swipes played back as eight switches in seven seconds. Now a swipe after the
 * step's first second says why nothing moves, and the step's end moves one
 * pet, the way the last swipe pointed. A swipe while control waits moves at
 * once and says nothing more. */
static void swipes_during_the_step(void)
{
    static const int burst[]={1,1,-1,1,1,-1,1,-1};
    for(size_t i=0;i<sizeof(burst)/sizeof(burst[0]);++i){
        clock_us+=1500000;pet_swiped(burst[i]);
        assert(!strcmp(announced,"> busy, can't switch yet")&&command_count==1&&s_shown_slot==1);
    }
}
static void a_burst_of_swipes_moves_once(void)
{
    boot(true);
    during_step=swipes_during_the_step;pocket_control_pass();during_step=NULL;
    assert(s_shown_slot==0&&!command_count&&switch_sounds==1&&strstr(announced," online"));
    // Control waits for a command: the swipe moves, without the notice.
    pet_swiped(1);assert(command_count==1&&strcmp(announced,"> busy, can't switch yet"));
    pocket_control_pass();assert(s_shown_slot==1&&!command_count&&switch_sounds==2);
}

static const struct {const char *name;void (*run)(void);} scenarios[]={
    {"swipe_keeps_session",swipe_keeps_session},{"selection_rebinds",selection_rebinds},
    {"swipe_back_resumes",swipe_back_resumes},{"tap_waits_for_the_session",tap_waits_for_the_session},
    {"refusal_reconnects",refusal_reconnects},{"rebind_timeout_reconnects",rebind_timeout_reconnects},
    {"older_gateway_reconnects",older_gateway_reconnects},{"usb_pet_not_linked",usb_pet_not_linked},
    {"rebind_applies_the_new_configuration",rebind_applies_the_new_configuration},
    {"management_answer_mid_switch_keeps_session",management_answer_mid_switch_keeps_session},
    {"management_answer_before_the_session_waits",management_answer_before_the_session_waits},
    {"tap_waits_for_the_whole_switch",tap_waits_for_the_whole_switch},{"a_refused_switch_says_so",a_refused_switch_says_so},
    {"tap_waits_for_a_control_step",tap_waits_for_a_control_step},
    {"control_yields_to_a_waiting_tap",control_yields_to_a_waiting_tap},
    {"a_follow_up_tap_listens_at_once",a_follow_up_tap_listens_at_once},
    {"a_stalled_listen_says_so",a_stalled_listen_says_so},
    {"brain_disconnect_releases_voice",brain_disconnect_releases_voice},
    {"brain_failure_and_renewal",brain_failure_and_renewal},
    {"a_swipe_stops_the_voice_calmly",a_swipe_stops_the_voice_calmly},
    {"a_burst_of_swipes_moves_once",a_burst_of_swipes_moves_once},
};
int main(int argc,char **argv)
{
    pack_length=fread(pack_bytes,1,sizeof(pack_bytes),stdin);assert(pack_length&&pack_length<sizeof(pack_bytes));
    s_audio_lock=xSemaphoreCreateMutex();s_audio_events=(QueueHandle_t)2;s_commands=(QueueHandle_t)3;
    atomic_store(&s_identity_ready,true);s_boot_health_decided=true;s_audio_started=s_face_started=true;
    strcpy(s_id,"00000000-0000-4000-8000-000000000001");s_origin="https://fixture.invalid";
    strcpy(signed_release.account_id,"00000000-0000-4000-8000-000000000003");
    strcpy(signed_release.project_id,"00000000-0000-4000-8000-000000000004");
    strcpy(signed_release.face_id,"big-sal");strcpy(signed_release.version,"fixture-240-v2");
    signed_release.imported=false;s_wifi.volume=40;s_wifi.brightness=80;
    /* Every scenario by default; one by name to see where each one stands. */
    unsigned ran=0;
    for(size_t i=0;i<sizeof(scenarios)/sizeof(scenarios[0]);++i)
        if(argc<2||!strcmp(argv[1],scenarios[i].name)){scenarios[i].run();printf("ok %s\n",scenarios[i].name);++ran;}
    assert(ran);
    free(s_shown_pack);s_shown_pack=NULL;
    return 0;
}
