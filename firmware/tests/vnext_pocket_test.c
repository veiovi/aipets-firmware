/* The Pocket Terminal runtime with a simulated three-pet store: which pet
 * shows at boot, without network; which one a swipe shows and how it greets;
 * what a long press does; what is saved, and when a pack is validated again;
 * that the renderer draws only from the verified RAM copy, which is never
 * replaced while it draws. The frame player and inventory are real. */
#include <assert.h>
#include "../main/pet_vnext.c"

static uint8_t pack_bytes[200000];
static size_t pack_length;
static pet_slot_inventory_t stored;
static esp_err_t load_result[PET_SLOT_COUNT];
static unsigned commits, full_validations, face_inits, audio_inits, shows, sounds, freezes, binds, loads, holds, releases;
static bool drawing_copy, no_psram;
static uint8_t volume_seen;
static char bound_id[PET_FACE_ID_MAX];
static bool face_init_fails;
static unsigned touches;
static char announced[PET_FACE_TERMINAL_MAX+1];
static pet_sfx_t last_sound;
static pet_face_callbacks_t face_callbacks;
static bool display_locked;

/* Simulated store: the inventory lives in `stored`; every slot holds the pack. */
esp_err_t pet_slot_store_open(pet_slot_store_t *s)
{memset(s,0,sizeof(*s));s->open=true;s->inventory.ready=true;s->inventory.state=stored;return ESP_OK;}
esp_err_t pet_slot_store_load(pet_slot_store_t *s,unsigned slot,void *buffer,size_t capacity,size_t *bytes)
{
    assert(s->open&&slot<PET_SLOT_COUNT&&buffer==s_shown_pack&&capacity==PET_LAYOUT_THREE_SLOT_PACK_BYTES);
    assert(!drawing_copy); /* Never overwritten under the renderer. */
    ++loads;
    if(s->inventory.state.slots[slot].state!=PET_SLOT_READY)return ESP_ERR_INVALID_STATE;
    /* A copy that fails its hash leaves bytes nobody may draw. */
    if(load_result[slot]!=ESP_OK){memset(buffer,0xff,pack_length);return load_result[slot];}
    memcpy(buffer,pack_bytes,pack_length);*bytes=pack_length;return ESP_OK;
}
esp_err_t pet_slot_store_commit(pet_slot_store_t *s,const pet_slot_inventory_t *next)
{assert(pet_slot_inventory_valid(next));++commits;s->inventory.state=stored=*next;return ESP_OK;}

fp_error_t pet_face_pack_validate(const void *bytes,uint32_t length,fp_pack_info_t *info)
{
    ++full_validations;
    uint32_t size=fp_validation_workspace_size();void *workspace=malloc(size);assert(workspace);
    fp_error_t result=fp_validate_with_workspace(bytes,length,info,workspace,size);free(workspace);return result;
}
esp_err_t pet_face_pack_use_external(const char *id,const void *pack,size_t bytes)
{
    assert(pack==s_shown_pack&&bytes==pack_length&&!memcmp(pack,pack_bytes,bytes));
    ++binds;drawing_copy=true;strlcpy(bound_id,id,sizeof(bound_id));return ESP_OK;
}
void pet_face_pack_hold_external(void){assert(display_locked);++holds;drawing_copy=false;}
void pet_face_pack_release_external(void){assert(display_locked);++releases;drawing_copy=false;}
void *heap_caps_aligned_alloc(size_t alignment,size_t size,unsigned caps)
{assert(caps&MALLOC_CAP_SPIRAM);return no_psram?NULL:aligned_alloc(alignment,size);}
esp_err_t pet_face_init(const pet_face_callbacks_t *callbacks,uint8_t volume,uint8_t brightness,uint8_t shake,
    const char *ssid,const char *face_id,pet_voice_t voice,uint16_t timeout,pet_animation_profile_t profile,pet_ai_mode_t mode,
    pet_realtime_model_t model,pet_speech_profile_t speech,pet_realtime_voice_t realtime,pet_realtime_boost_t boost,
    pet_cartesia_voice_gender_t cartesia,pet_face_gender_t gender,pet_speech_mouth_mode_t mouth)
{
    (void)brightness;(void)shake;(void)ssid;(void)voice;(void)timeout;(void)profile;(void)mode;(void)model;(void)speech;
    (void)realtime;(void)boost;(void)cartesia;(void)gender;(void)mouth;
    assert(callbacks->tapped&&callbacks->pet_swiped&&callbacks->gesture_requested&&!strcmp(face_id,bound_id));
    face_callbacks=*callbacks;++face_inits;volume_seen=volume;return face_init_fails?ESP_FAIL:ESP_OK;
}
esp_err_t pet_face_set_id(const char *face_id){assert(!strcmp(face_id,bound_id));return ESP_OK;}
esp_err_t pet_audio_init(pet_audio_capture_chunk_cb_t chunk,pet_audio_capture_done_cb_t done,pet_audio_playback_done_cb_t played)
{(void)chunk;(void)done;(void)played;++audio_inits;return ESP_OK;}
void pet_face_show(void){++shows;}
esp_err_t pet_sfx_play(pet_sfx_t effect){last_sound=effect;++sounds;return ESP_OK;}
bool pet_face_touch_reaction(void){++touches;return true;}
void pet_face_announce(const char *text){strlcpy(announced,text,sizeof(announced));}
void pet_network_freeze_bound(void){++freezes;}
bool pet_control_http_json(const pet_control_http_t *http, const char *path, const char *body,
                          char *out, size_t capacity, pet_control_http_result_t *result)
{
    (void)http;
    (void)path;
    (void)body;
    (void)out;
    (void)capacity;
    (void)result;
    assert(!"offline pet must not request a Brain grant");
    return false;
}
void pet_network_set_brain_token(const char *token)
{
    (void)token;
}
/* No voice session runs offline: nothing to reuse, rebind or cancel. */
bool pet_network_bound_to(const pet_control_context_t *context){(void)context;return false;}
bool pet_network_can_rebind(void){return false;}
/* A gateway that tells stories, when a test offers one. */
static bool story_offered;static esp_err_t story_result=ESP_OK;static uint32_t story_stream;static char story_pet[64];
bool pet_network_can_story(void){return story_offered;}
esp_err_t pet_network_story(uint32_t stream,const char *pet)
{story_stream=stream;strlcpy(story_pet,pet,sizeof(story_pet));return story_result;}
/* finish_send()'s other requests; only the story is sent here. */
esp_err_t pet_network_input_start(uint32_t stream,const char *pet){(void)stream;(void)pet;assert(!"no microphone");return ESP_FAIL;}
esp_err_t pet_network_input_end(uint32_t stream,uint32_t sequence,uint32_t duration)
{(void)stream;(void)sequence;(void)duration;assert(!"no recording");return ESP_FAIL;}
esp_err_t pet_audio_capture_start(uint32_t stream){(void)stream;assert(!"no microphone");return ESP_FAIL;}
esp_err_t pet_network_config_v2_result(const pet_synced_config_v2_t *settings,bool applied,const char *code,const char *message)
{(void)settings;(void)applied;(void)code;(void)message;assert(!"no settings");return ESP_FAIL;}
esp_err_t pet_network_speech_profile_result(const pet_speech_preference_t *preference,bool applied)
{(void)preference;(void)applied;assert(!"no speech profile");return ESP_FAIL;}
esp_err_t pet_network_rebind(const pet_control_context_t *context){(void)context;assert(!"no rebind offline");return ESP_FAIL;}
esp_err_t pet_network_cancel(uint32_t stream){(void)stream;return ESP_OK;}
esp_err_t pet_ota_confirm_health_stage(pet_ota_health_stage_t stage,const pet_ota_health_evidence_t *evidence)
{(void)stage;(void)evidence;return ESP_OK;}
esp_err_t pet_slot_store_read_record(pet_slot_store_t *s,unsigned slot,void *record,size_t bytes)
{(void)s;(void)slot;(void)record;(void)bytes;assert(!"no signed record is read offline");return ESP_FAIL;}
bool pet_release_v2_record_verify(const void *record,size_t bytes,const char *account,const pet_replace_pack_t *expected,
                                  const pet_pack_trust_key_t *keys,size_t count,pet_release_v2_t *out)
{(void)record;(void)bytes;(void)account;(void)expected;(void)keys;(void)count;(void)out;return false;}
/* No pet worker runs offline; installation has its own test (vnext_pocket_install_test.c). */
void pet_replace_control_disconnect(pet_replace_control_t *worker){(void)worker;assert(!"no worker offline");}
/* Hardware seams that show no behavior of their own here. */
int64_t esp_timer_get_time(void){return 1000000;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out){(void)args;*out=NULL;return ESP_FAIL;}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t timeout){(void)timer;(void)timeout;return ESP_OK;}
esp_err_t pet_display_lock(int timeout){(void)timeout;assert(!display_locked);display_locked=true;return ESP_OK;}
void pet_display_unlock(void){assert(display_locked);display_locked=false;}
QueueHandle_t xQueueCreate(unsigned count,unsigned size){(void)count;(void)size;return (QueueHandle_t)1;}
/* The next audio event queued, when a test asks for it. */
static bool capture_audio_event;static audio_event_t queued_audio;
int xQueueSend(QueueHandle_t queue,const void *item,TickType_t ticks){(void)queue;(void)ticks;
    if(capture_audio_event){memcpy(&queued_audio,item,sizeof(queued_audio));capture_audio_event=false;}return pdTRUE;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (SemaphoreHandle_t)1;}
int xSemaphoreTake(SemaphoreHandle_t handle,TickType_t ticks){(void)handle;(void)ticks;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t handle){(void)handle;return pdTRUE;}
void pet_audio_capture_stop(void){}
void pet_audio_playback_cancel(void){}
void pet_audio_set_speech_mouth_offset(int16_t offset_ms){(void)offset_ms;}
esp_err_t pet_config_store_speech_mouth_offset(bool present,int16_t offset_ms){(void)present;(void)offset_ms;return ESP_OK;}
esp_err_t pet_audio_playback_enqueue(uint32_t stream,uint32_t sequence,const uint8_t *pcm,size_t length)
{(void)stream;(void)sequence;(void)pcm;(void)length;return ESP_OK;}
void pet_audio_playback_finish(uint32_t stream){(void)stream;}
esp_err_t pet_audio_playback_start(uint32_t stream,uint32_t rate,pet_realtime_boost_t boost,pet_speech_mouth_mode_t mouth)
{(void)stream;(void)rate;(void)boost;(void)mouth;return ESP_OK;}
esp_err_t pet_audio_set_capture_timeout(uint16_t seconds){(void)seconds;return ESP_OK;}
static int volume_level=-1,brightness_level=-1,stored_volume=-1,stored_brightness=-1;
static bool wifi_ready;
void pet_audio_set_volume(uint8_t volume){volume_level=volume;}
bool pet_network_wifi_is_ready(void){return wifi_ready;}
esp_err_t pet_config_store_volume(uint8_t volume){stored_volume=volume;return ESP_OK;}
esp_err_t pet_config_store_brightness(uint8_t brightness){stored_brightness=brightness;return ESP_OK;}
esp_err_t bsp_display_brightness_set(int brightness){(void)brightness;assert(!"the face sets brightness");return ESP_FAIL;}
/* Reached only through pocket_command's other commands, never run here. */
void esp_restart(void){assert(!"no restart");abort();}
void vTaskDelay(TickType_t ticks){(void)ticks;}
esp_err_t pet_network_ota_status(const char *release,const char *status,unsigned progress,const char *error)
{(void)release;(void)status;(void)progress;(void)error;return ESP_OK;}
void pet_battery_set_enabled(bool enabled){(void)enabled;}
esp_err_t pet_battery_start(pet_battery_callback_t callback){(void)callback;return ESP_OK;}
void pet_diagnostics_note_dropped_app_event(void){}
void pet_diagnostics_note_app_event(int32_t event,const char *name,uint32_t depth){(void)event;(void)name;(void)depth;}
void pet_diagnostics_note_streams(uint32_t input,uint32_t output){(void)input;(void)output;}
void pet_enrollment_clear(void *data,size_t bytes){memset(data,0,bytes);}
bool pet_expression_from_wire(const char *name,pet_expression_t *value){(void)name;(void)value;return false;}
esp_err_t pet_face_set_animation_profile(pet_animation_profile_t profile){(void)profile;return ESP_OK;}
void pet_face_set_battery(const pet_battery_snapshot_t *snapshot){(void)snapshot;}
esp_err_t pet_face_set_brightness(uint8_t brightness){brightness_level=brightness;return ESP_OK;}
void pet_face_set_expression(pet_expression_t expression){(void)expression;}
esp_err_t pet_face_set_recording_timeout(uint16_t seconds){(void)seconds;return ESP_OK;}
esp_err_t pet_face_set_shake_sensitivity(uint8_t sensitivity){(void)sensitivity;return ESP_OK;}
esp_err_t pet_face_set_speech_profile(pet_speech_profile_t mode,bool batch,bool realtime,bool fish)
{(void)mode;(void)batch;(void)realtime;(void)fish;return ESP_OK;}
static pet_face_state_t face_state=PET_FACE_BOOTING;
void pet_face_set_state(pet_face_state_t state){face_state=state;}
pet_face_state_t pet_face_get_state(void){return face_state;}
bool pet_face_shake_detection_enabled(void){return false;}
uint8_t pet_face_trigger_gesture(uint8_t gesture){(void)gesture;return 0;}
void pet_motion_set_sensitivity(uint8_t sensitivity){(void)sensitivity;}
esp_err_t pet_motion_start(pet_motion_gesture_callback_t callback,pet_motion_enabled_callback_t enabled,uint8_t sensitivity)
{(void)callback;(void)enabled;(void)sensitivity;return ESP_OK;}
static bool network_ready,mic_sent;
bool pet_network_is_ready(void){return network_ready;}
esp_err_t pet_network_send_microphone(uint32_t stream,uint32_t sequence,const int16_t *pcm,size_t samples)
{(void)stream;(void)sequence;(void)pcm;(void)samples;mic_sent=true;return ESP_OK;}
/* Reachable once the network is up; nothing here starts listening. */
uint32_t esp_random(void){return 4;}
bool pet_audio_is_capturing(void){return false;}
bool pet_audio_is_playing(void){return false;}
esp_err_t pet_sfx_play_listen(uint32_t token){(void)token;return ESP_OK;}
void pet_network_set_battery_snapshot(const pet_battery_snapshot_t *snapshot){(void)snapshot;}
/* Offline showing never starts a voice session. */
esp_err_t pet_network_start_bound(const pet_config_t *config,const pet_network_callbacks_t *callbacks,
    const char *device,const pet_control_context_t *context)
{(void)config;(void)callbacks;(void)device;(void)context;assert(!"no network while showing a pet");return ESP_FAIL;}
void pet_onboarding_open_from_ui(void){}
bool pet_ota_busy(void){return false;}
void pet_sfx_cancel(void){}
pet_sfx_t pet_sfx_for_face_swipe(uint8_t index){(void)index;return PET_SFX_FACE_SWIPE_0;}
esp_err_t pet_sfx_init(pet_sfx_listen_done_t done){(void)done;return ESP_OK;}

static void make_ready(pet_slot_inventory_t *inventory,unsigned slot)
{
    pet_slot_t *t=&inventory->slots[slot];
    t->state=PET_SLOT_READY;t->pack.bytes=(uint32_t)pack_length;t->validator_revision=FP_VALIDATOR_REVISION;
    snprintf(t->pack.build_id,sizeof(t->pack.build_id),"00000000-0000-4000-8000-00000000000%u",slot);
    memset(t->pack.sha256,'a',64);t->pack.sha256[64]=0;
}
static void boot(const pet_slot_inventory_t *inventory)
{
    stored=*inventory;s_shown_slot=-1;s_selection_save_at=0;
    free(s_shown_pack);s_shown_pack=NULL;drawing_copy=false;memset(&s_prepared,0,sizeof(s_prepared));
    memset(load_result,0,sizeof(load_result));commits=full_validations=binds=shows=sounds=touches=loads=holds=releases=0;announced[0]=0;
    pocket_show_active();
}

int main(void)
{
    pack_length=fread(pack_bytes,1,sizeof(pack_bytes),stdin);assert(pack_length&&pack_length<sizeof(pack_bytes));
    s_commands=xQueueCreate(8,sizeof(control_command_t));s_audio_lock=xSemaphoreCreateMutex();
    s_wifi.volume=37;s_wifi.brightness=80;s_wifi.recording_timeout_seconds=12;

    // A new device has no pet: nothing is shown and nothing is written.
    pet_slot_inventory_t inventory;assert(pet_slot_inventory_empty(&inventory));
    boot(&inventory);assert(s_shown_slot==-1&&!binds&&!commits&&!face_inits);

    // The recorded pet shows from its verified copy, before any network, with
    // local settings and no frame validation; the face and audio start once.
    make_ready(&inventory,0);make_ready(&inventory,2);inventory.active=2;
    boot(&inventory);
    assert(s_shown_slot==2&&loads==1&&binds==1&&drawing_copy&&shows==1&&face_inits==1&&audio_inits==1&&volume_seen==37);
    assert(face_state==PET_FACE_IDLE); /* at home offline: idles, not the offline pose */
    assert(!strcmp(announced,"> 2 pets on board")&&!touches);
    assert(!full_validations&&!commits&&!sounds&&!strcmp(s_prepared[0].manifest.face_id,bound_id));

    // A damaged slot is skipped; the next installed pet shows and becomes the selection.
    boot(&inventory);load_result[2]=ESP_ERR_INVALID_CRC;s_shown_slot=-1;pocket_show_active();
    assert(s_shown_slot==0&&stored.active==0&&commits==1&&drawing_copy);

    // A pack proved by an older validator is checked in full once, then recorded.
    inventory=stored;inventory.slots[2].validator_revision=0xffff;inventory.active=2;
    boot(&inventory);
    assert(s_shown_slot==2&&full_validations==1&&commits==1&&stored.slots[2].validator_revision==FP_VALIDATOR_REVISION);
    boot(&stored);assert(s_shown_slot==2&&!full_validations);

    // A swipe shows the next installed pet with a sound; the selection is saved
    // once the swipes settle, not on every swipe.
    // The renderer lets go of the copy before another pet is copied into it.
    // The voice session stays open: the swipe pauses voice, it does not close it.
    unsigned before_freezes=freezes;
    pocket_switch(1);assert(s_shown_slot==0&&sounds==1&&!commits&&s_selection_save_at&&freezes==before_freezes);
    assert(atomic_load(&s_voice_paused)&&!atomic_load(&s_voice_allowed));
    assert(holds==loads&&loads==2&&binds==2&&drawing_copy);
    // The pet that arrives says hello and the terminal types its name.
    assert(touches==1&&!strcmp(announced,"> EXAMPLE online"));
    pocket_switch(1);assert(s_shown_slot==2&&sounds==2&&!commits);
    pocket_switch(-1);assert(s_shown_slot==0&&sounds==3);
    persist_selection();assert(commits==1&&stored.active==0);
    persist_selection();assert(commits==1);

    // With one pet a swipe changes nothing and makes no sound.
    assert(pet_slot_inventory_empty(&inventory));make_ready(&inventory,1);inventory.active=1;
    boot(&inventory);unsigned before_binds=binds;
    pocket_switch(1);pocket_switch(-1);assert(s_shown_slot==1&&binds==before_binds&&!sounds&&!touches&&!strcmp(announced,"> 1 pet on board"));

    // A long press pets the pet: its touch reaction and a pop. Without a
    // gateway that tells stories it starts no turn, and it waits while one runs.
    face_callbacks.gesture_requested(FC_GESTURE_SPIN_CW);
    const audio_event_t petted={.kind=AUDIO_PET};audio_send_t send={.kind=SEND_NONE};
    handle_audio(&petted,&send);
    assert(touches==1&&sounds==1&&last_sound==PET_SFX_GESTURE_POP&&send.kind==SEND_NONE&&!atomic_load(&s_turn_active));
    atomic_store(&s_turn_active,true);handle_audio(&petted,&send);atomic_store(&s_turn_active,false);
    assert(touches==1&&sounds==1);

    // Tap to talk without a voice says why, with the no-voice cue at most
    // every 1.5 s, and starts nothing.
    const audio_event_t tap={.kind=AUDIO_TAP};
    handle_audio(&tap,&send);
    assert(!strcmp(announced,"> no wifi: can't talk")&&sounds==2&&last_sound==PET_SFX_NO_VOICE);
    assert(send.kind==SEND_NONE&&!atomic_load(&s_turn_active));
    announced[0]=0;handle_audio(&tap,&send);
    assert(!strcmp(announced,"> no wifi: can't talk")&&sounds==2);
    // The message follows the reason the pet cannot talk.
    wifi_ready=true;
    atomic_store(&s_voice_block,VOICE_BLOCK_NOT_PAIRED);handle_audio(&tap,&send);
    assert(!strcmp(announced,"> not paired: no voice"));
    atomic_store(&s_voice_block,VOICE_BLOCK_NOT_LINKED);handle_audio(&tap,&send);
    assert(!strcmp(announced,"> not linked: no voice"));
    // While the session connects, a tap waits calmly, with no cue; a second tap cancels it.
    atomic_store(&s_voice_block,VOICE_BLOCK_CONNECTING);handle_audio(&tap,&send);
    assert(!strcmp(announced,"> connecting...")&&sounds==2&&atomic_load(&s_tap_pending)&&face_state==PET_FACE_CONNECTING);
    handle_audio(&tap,&send);
    assert(!strcmp(announced,"> cancelled")&&sounds==2&&!atomic_load(&s_tap_pending)&&face_state==PET_FACE_IDLE&&send.kind==SEND_NONE);
    wifi_ready=false;
    // A level chosen in the menu is heard at once, as a pop at that level.
    const audio_event_t level={.kind=AUDIO_VOLUME,.stream=30};
    handle_audio(&level,&send);
    assert(volume_level==30&&sounds==3&&last_sound==PET_SFX_GESTURE_POP&&send.kind==SEND_NONE);
    // A volume change outlives a conversation session; its events do not.
    audio_event_t stale={.kind=AUDIO_TAP,.generation=atomic_load(&s_generation)+1};
    assert(!audio_event_current(&stale));
    stale.kind=AUDIO_VOLUME;assert(audio_event_current(&stale));
    // The menu's levels apply at once, the next pet starts at them even
    // before they are kept, and the control task keeps and re-applies them.
    s_audio_events=(QueueHandle_t)1;s_commands=(QueueHandle_t)1;
    assert(volume_requested(42,false)&&atomic_load(&s_volume_now)==42);
    assert(!brightness_requested(PET_BRIGHTNESS_MIN-1,false)&&brightness_requested(60,false));
    assert(atomic_load(&s_brightness_now)==60&&brightness_level==60);
    volume_level=brightness_level=-1;boot(&inventory);
    assert(volume_level==42&&brightness_level==60);
    control_command_t keep={.kind=CONTROL_KEEP_VOLUME,.value=42};
    volume_level=-1;pocket_command(&keep);
    assert(s_wifi.volume==42&&stored_volume==42&&volume_level==42);
    keep=(control_command_t){.kind=CONTROL_KEEP_BRIGHTNESS,.value=60};
    brightness_level=-1;pocket_command(&keep);
    assert(s_wifi.brightness==60&&stored_brightness==60&&brightness_level==60);
    // Leaving the menu shows the pet at once, with the closing cue.
    unsigned before_shows=shows;return_requested();
    assert(shows==before_shows+1&&last_sound==PET_SFX_SETTINGS_CLOSE);

    // A pet that cannot show is skipped; if none can, the current one returns.
    make_ready(&inventory,0);boot(&inventory);assert(s_shown_slot==1);
    inventory.slots[0].state=PET_SLOT_FREE;boot(&inventory);assert(!strcmp(announced,"> 1 pet on board"));
    make_ready(&inventory,0);boot(&inventory);
    load_result[0]=ESP_ERR_INVALID_CRC;pocket_switch(1);assert(s_shown_slot==1&&!sounds&&drawing_copy&&loads==3&&!releases);
    // When the current pet cannot be copied back either, nothing draws from
    // the damaged copy: the emergency face replaces the held frame.
    load_result[1]=ESP_ERR_INVALID_CRC;pocket_switch(1);
    assert(!drawing_copy&&releases==1&&!s_prepared[0].bytes&&!sounds);

    // Without PSRAM for the copy no pet is shown, and the reason is reported.
    no_psram=true;boot(&inventory);no_psram=false;
    assert(!s_shown_pack&&!loads&&!binds&&!strcmp(s_local_error,"PET_MEMORY"));
    s_local_error[0]=0;free(s_shown_pack);s_shown_pack=NULL;
    // A turn the gateway refuses says why on the face and stops its microphone
    // at once, instead of dropping back from listening with no reason.
    network_ready=true;atomic_store(&s_voice_allowed,true);s_audio_started=true;atomic_store(&s_turn_active,true);
    atomic_store(&s_input_stream,77);
    capture_audio_event=true;session_error("OWNER_PET_UNAVAILABLE","I'm not in your account anymore",false);
    assert(queued_audio.kind==AUDIO_ERROR&&!strcmp(queued_audio.line,"> update pet at aipets.com"));
    int16_t frame[320]={0};mic_sent=false;
    assert(capture_chunk(77,1,frame,320)==ESP_OK&&!mic_sent);
    announced[0]=0;handle_audio(&queued_audio,&send);
    assert(!strcmp(announced,"> update pet at aipets.com")&&face_state==PET_FACE_ERROR);
    // Its frames still in flight are refused too, with no second error.
    capture_audio_event=true;session_error("AUDIO_STREAM","late frame",true);assert(capture_audio_event);
    // Another refusal gives a general reason; a recoverable error gives none.
    session_error("SOMETHING_NEW","",false);assert(!strcmp(queued_audio.line,"> can't talk right now"));
    atomic_store(&s_input_stream,78);capture_audio_event=true;session_error("PIPELINE_ERROR","",true);
    assert(!queued_audio.line);

    // With a gateway that tells stories, a long press also asks the pet for one
    // from its world: a turn on a new stream, without the microphone.
    atomic_store(&s_brain_expires_ms,1000u+900000u);atomic_store(&s_input_stream,0);
    strlcpy(s_applied.config.ai_pet_id,"example-pet-000000a1",sizeof(s_applied.config.ai_pet_id));
    story_offered=true;touches=sounds=0;mic_sent=false;face_state=PET_FACE_IDLE;send=(audio_send_t){.kind=SEND_NONE};
    handle_audio(&petted,&send);
    assert(touches==1&&sounds==1&&last_sound==PET_SFX_GESTURE_POP&&face_state==PET_FACE_IDLE);
    assert(send.kind==SEND_STORY&&send.stream&&send.stream==atomic_load(&s_input_stream)&&atomic_load(&s_turn_active));
    finish_send(&send,&petted);
    assert(story_stream==send.stream&&!strcmp(story_pet,"example-pet-000000a1")&&!mic_sent&&atomic_load(&s_turn_active));
    // The gateway's thinking, speech and idle then run it like a spoken turn.
    const audio_event_t thinking={.kind=AUDIO_STATE,.stream=1},idle={.kind=AUDIO_STATE};
    handle_audio(&thinking,&send);assert(face_state==PET_FACE_THINKING&&atomic_load(&s_turn_active));
    handle_audio(&idle,&send);assert(face_state==PET_FACE_IDLE&&!atomic_load(&s_turn_active));
    // A request the socket does not take says so and frees the voice.
    story_result=ESP_ERR_INVALID_STATE;send=(audio_send_t){.kind=SEND_NONE};handle_audio(&petted,&send);
    assert(send.kind==SEND_STORY);finish_send(&send,&petted);
    assert(!atomic_load(&s_turn_active)&&face_state==PET_FACE_ERROR&&!strcmp(announced,"> offline, try again"));
    assert(pet_runtime_claim(&s_resources,PET_RESOURCE_VOICE));pet_runtime_release(&s_resources,PET_RESOURCE_VOICE);
    // While a cloud step holds the voice, petting stays petting and says so.
    story_result=ESP_OK;face_state=PET_FACE_IDLE;assert(pet_runtime_claim(&s_resources,PET_RESOURCE_CONTROL));
    send=(audio_send_t){.kind=SEND_NONE};handle_audio(&petted,&send);
    assert(send.kind==SEND_NONE&&!atomic_load(&s_turn_active)&&!strcmp(announced,"> busy, try again"));
    pet_runtime_release(&s_resources,PET_RESOURCE_CONTROL);
    puts("pocket runtime: offline boot from a verified copy, damaged-slot fallback, one revalidation per validator, swipes with settled saves and a greeting, petting, a tap that says why it cannot talk, a refused turn that says why and stops its microphone, the menu's volume, single pet, no drawing from a failed copy, long-press stories");
    return 0;
}
