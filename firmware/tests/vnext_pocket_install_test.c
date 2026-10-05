/* Pocket Terminal installation and voice with a simulated three-pet store:
 * the v2 worker's callbacks on the inventory's `operation`. The inventory,
 * release compatibility, pack validation and frame player are real; signed
 * records, flash and the network are seams. */
#include <assert.h>
#include "../main/pet_vnext.c"
#include "mbedtls/sha256.h"

static uint8_t pack_bytes[200000],target_bytes[200000];
static size_t pack_length;
static pet_slot_inventory_t stored;
/* Signed cloud pets the slots' records hold; any other slot's record is erased. */
static pet_release_v2_t signed_release,other_release;
static unsigned unsigned_slots;
static pet_firmware_boot_state_t observed;
static pet_replace_control_t pet_worker;
static pet_firmware_control_t firmware_worker;
static pet_onboarding_status_t visible_status;
static bool drawing_copy,target_ok=true,record_ok=true,signature_ok=true,wifi_ok=true,setup_ok=true,ui_ok=true;
static bool simulate_commit, commit_fails, firmware_reserved;
static unsigned setup_shows;
static unsigned load_fail_mask,opens;
static bool open_fails;
static void commit_installation(const pet_replace_pack_t *pack,const char *binding);
static unsigned loads,target_loads,record_reads,commits,binds,holds,releases,online_starts,offline_shows,disconnects;
static unsigned fw_steps,pet_steps,freezes;
static uint32_t clock_ms=100000;
static char online_build[37];

int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
uint32_t esp_random(void){return 7;}
void esp_fill_random(void *buffer,size_t bytes){memset(buffer,7,bytes);}
void *heap_caps_aligned_alloc(size_t alignment,size_t size,unsigned caps)
{(void)caps;return aligned_alloc(alignment,(size+alignment-1)/alignment*alignment);}
void heap_caps_free(void *memory){free(memory);}

/* The store: slots hold copies of the fixture; the downloaded target is `target_bytes`. */
esp_err_t pet_slot_store_open(pet_slot_store_t *s)
{
    memset(s,0,sizeof(*s));++opens;if(open_fails)return ESP_ERR_INVALID_CRC;
    s->open=true;s->inventory.ready=true;s->inventory.state=stored;
    s->machine.state=stored.operation;
    assert(pet_flash_layout_known(PET_LAYOUT_THREE_3P5M,&s->layout));memset(s->partition_sha256,'c',64);
    return ESP_OK;
}
esp_err_t pet_slot_store_load(pet_slot_store_t *s,unsigned slot,void *buffer,size_t capacity,size_t *bytes)
{
    assert(s->open&&slot<PET_SLOT_COUNT&&buffer==s_shown_pack&&capacity==PET_LAYOUT_THREE_SLOT_PACK_BYTES);
    assert(!drawing_copy); /* Never overwritten under the renderer. */
    ++loads;
    if(s->inventory.state.slots[slot].state!=PET_SLOT_READY)return ESP_ERR_INVALID_STATE;
    if(load_fail_mask&(1u<<slot)){memset(buffer,0xff,pack_length);return ESP_ERR_INVALID_CRC;}
    memcpy(buffer,pack_bytes,pack_length);*bytes=pack_length;return ESP_OK;
}
esp_err_t pet_slot_store_load_target(pet_slot_store_t *s,void *buffer,size_t capacity,size_t *bytes)
{
    assert(s==&s_slots&&buffer==s_shown_pack&&capacity==PET_LAYOUT_THREE_SLOT_PACK_BYTES&&!drawing_copy);
    ++target_loads;if(!target_ok)return ESP_ERR_INVALID_CRC;
    memcpy(buffer,target_bytes,pack_length);*bytes=pack_length;return ESP_OK;
}
/* The pack whose record a slot holds, when a test names it: the installer
 * writes its target's record before the pack's first byte. */
static pet_replace_pack_t record_of[PET_SLOT_COUNT];
esp_err_t pet_slot_store_read_record(pet_slot_store_t *s,unsigned slot,void *record,size_t bytes)
{
    assert(s==&s_slots&&slot<PET_SLOT_COUNT&&bytes==PET_REPLACE_MANIFEST_BYTES);++record_reads;
    if(!record_ok||(unsigned_slots&(1u<<slot))){memset(record,0xff,bytes);return ESP_OK;} /* Erased: placed over USB. */
    memset(record,0x5a,bytes);((uint8_t *)record)[1]=(uint8_t)slot;return ESP_OK;
}
esp_err_t pet_slot_store_commit(pet_slot_store_t *s,const pet_slot_inventory_t *next)
{
    assert(pet_slot_inventory_valid(next));
    ++commits;
    if (commit_fails)
        return ESP_FAIL;
    s->inventory.state = stored = *next;
    s->machine.state = next->operation;
    return ESP_OK;
}
esp_err_t pet_slot_store_attach_installer(pet_slot_store_t *s,const pet_slot_readers_t *readers)
{s->installer=true;s->readers=*readers;return ESP_OK;}
/* These installations add pets; none names a pack to replace. */
bool pet_slot_store_prepare_replacement(pet_slot_store_t *s,const pet_replace_pack_t *incoming,
                                        const pet_replace_pack_t *replacement)
{
    (void)s;
    (void)incoming;
    return replacement==NULL;
}
bool pet_release_v2_record_verify(const void *record,size_t bytes,const char *account,const pet_replace_pack_t *expected,
                                  const pet_pack_trust_key_t *keys,size_t count,pet_release_v2_t *out)
{
    assert(record&&bytes==PET_REPLACE_MANIFEST_BYTES&&keys==&s_trust&&count==s_trust_count);
    const uint8_t *raw=record;
    if(!signature_ok||raw[0]==0xff)return false;
    /* A slot whose record_of is set holds a record for exactly that pack. */
    const pet_replace_pack_t *held=raw[1]<PET_SLOT_COUNT&&record_of[raw[1]].bytes?&record_of[raw[1]]:NULL;
    if(held){
        if(strcmp(account,signed_release.account_id)||expected->bytes!=held->bytes||strcmp(expected->build_id,held->build_id)||
           strcmp(expected->sha256,held->sha256))return false;
        *out=signed_release;out->pack=*held;out->requirements.bytes=held->bytes;return true;
    }
    for(const pet_release_v2_t *r=&signed_release;r;r=r==&signed_release?&other_release:NULL)
        if(!strcmp(account,r->account_id)&&expected->bytes==r->pack.bytes&&
           !strcmp(expected->build_id,r->pack.build_id)&&!strcmp(expected->sha256,r->pack.sha256)){*out=*r;return true;}
    return false;
}
/* The partition table read back from flash. */
static char physical_hash[65];
esp_err_t pet_flash_layout_read(pet_flash_layout_t *layout,char sha[65])
{*layout=observed.layout;strcpy(sha,physical_hash);return ESP_OK;}

/* The renderer draws only from the copy. */
fp_error_t pet_face_pack_validate(const void *bytes,uint32_t length,fp_pack_info_t *info)
{
    uint32_t size=fp_validation_workspace_size();void *workspace=malloc(size);assert(workspace);
    fp_error_t result=fp_validate_with_workspace(bytes,length,info,workspace,size);free(workspace);return result;
}
/* Registered under the face ID activation passes, which must name these
 * bytes as the renderer's own check requires (pet_face_pack.c). */
static char bound_face_id[PET_FACE_ID_MAX];
esp_err_t pet_face_pack_use_external(const char *id,const void *pack,size_t bytes)
{
    assert(pack==s_shown_pack&&bytes==pack_length);
    fp_pack_info_t info={0};assert(fp_inspect_prevalidated(pack,(uint32_t)bytes,&info)==FP_OK);
    assert(pet_face_id_matches(id,info.id,info.id_len,true));
    strlcpy(bound_face_id,id,sizeof(bound_face_id));++binds;drawing_copy=true;return ESP_OK;
}
void pet_face_pack_hold_external(void){++holds;drawing_copy=false;}
void pet_face_pack_release_external(void){++releases;drawing_copy=false;}
bool pet_face_pack_ready(void){return drawing_copy;}
esp_err_t pet_face_init(const pet_face_callbacks_t *callbacks,uint8_t volume,uint8_t brightness,uint8_t shake,
    const char *ssid,const char *face_id,pet_voice_t voice,uint16_t timeout,pet_animation_profile_t profile,pet_ai_mode_t mode,
    pet_realtime_model_t model,pet_speech_profile_t speech,pet_realtime_voice_t realtime,pet_realtime_boost_t boost,
    pet_cartesia_voice_gender_t cartesia,pet_face_gender_t gender,pet_speech_mouth_mode_t mouth)
{
    (void)callbacks;(void)volume;(void)brightness;(void)shake;(void)ssid;(void)face_id;(void)voice;(void)timeout;(void)profile;
    (void)mode;(void)model;(void)speech;(void)realtime;(void)boost;(void)cartesia;(void)gender;(void)mouth;return ESP_OK;
}
esp_err_t pet_face_set_id(const char *face_id){(void)face_id;return ESP_OK;}
esp_err_t pet_audio_init(pet_audio_capture_chunk_cb_t chunk,pet_audio_capture_done_cb_t done,pet_audio_playback_done_cb_t played)
{(void)chunk;(void)done;(void)played;return ESP_OK;}
void pet_face_show(void){}
esp_err_t pet_sfx_play(pet_sfx_t effect){(void)effect;return ESP_OK;}
bool pet_face_touch_reaction(void){return true;}
void pet_face_announce(const char *text){(void)text;}
static pet_face_state_t face_state=PET_FACE_BOOTING;
void pet_face_set_state(pet_face_state_t state){face_state=state;if(state==PET_FACE_IDLE)++offline_shows;}
pet_face_state_t pet_face_get_state(void){return face_state;}
/* Voice: the bound pet's session. */
esp_err_t pet_network_start_bound(const pet_config_t *config,const pet_network_callbacks_t *callbacks,
    const char *device,const pet_control_context_t *context)
{
    (void)config;(void)callbacks;assert(!strcmp(device,s_id));
    ++online_starts;strlcpy(online_build,context->binding.build_id,sizeof(online_build));return ESP_OK;
}
void pet_network_freeze_bound(void){++freezes;}
bool pet_control_http_json(const pet_control_http_t *http, const char *path, const char *body,
                          char *out, size_t capacity, pet_control_http_result_t *result)
{
    assert(http && !strcmp(path, "/v1/device/brain"));
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
    assert(pet_session_wire_brain_token(token));
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
pet_control_http_timing_t pet_control_http_last_timing(void){return (pet_control_http_timing_t){0};}
void pet_control_http_close_kept(void){}
/* No open session to reuse here: every admission starts a new socket
 * (the session-rebind flow has its own test, vnext_pocket_switch_test.c). */
bool pet_network_bound_to(const pet_control_context_t *context){(void)context;return false;}
bool pet_network_can_rebind(void){return false;}
bool pet_network_can_story(void){return false;}
esp_err_t pet_network_story(uint32_t stream,const char *pet){(void)stream;(void)pet;assert(!"no story here");return ESP_FAIL;}
esp_err_t pet_network_rebind(const pet_control_context_t *context){(void)context;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t pet_network_cancel(uint32_t stream){(void)stream;return ESP_OK;}
esp_err_t pet_ota_confirm_health_stage(pet_ota_health_stage_t stage,const pet_ota_health_evidence_t *evidence)
{(void)stage;(void)evidence;return ESP_OK;}
void pet_replace_control_disconnect(pet_replace_control_t *worker)
{
    assert(worker==s_pet);++disconnects;
    if(worker->admitted)freeze(NULL);
    worker->admitted=worker->authenticated=false;worker->next_poll_ms=0;
}
/* Control: the workers' steps, and an installation that commits. */
bool pet_firmware_control_blocks_pet(const pet_firmware_control_t *worker){(void)worker;return firmware_reserved;}
bool pet_firmware_control_health_deadline(pet_firmware_control_t *worker,uint64_t now){(void)worker;(void)now;return false;}
pet_firmware_control_status_t pet_firmware_control_step(pet_firmware_control_t *worker,uint64_t now)
{(void)now;assert(worker==s_firmware);++fw_steps;return PET_FW_CONTROL_WORKING;}
pet_replace_control_status_t pet_replace_control_step(pet_replace_control_t *worker,uint64_t now,bool allow)
{
    (void)now;assert(worker==s_pet&&allow);++pet_steps;
    if(simulate_commit){simulate_commit=false;commit_installation(&signed_release.pack,"binding-2");}
    return PET_REPLACE_CONTROL_WORKING;
}
/* Selection: the cloud's answer for the pet on screen. A choice is adopted as
 * the worker does, binding exactly that pet with its relationship and config;
 * no answer starts the worker's 30 s backoff, as the worker does. */
static pet_replace_select_t select_answer=PET_REPLACE_SELECT_CHOSEN;
static unsigned selects,select_revision;
static pet_replace_pack_t selected_pack;
static const char *select_relationship="00000000-0000-4000-8000-000000000051";
pet_replace_select_t pet_replace_control_select(pet_replace_control_t *worker,uint64_t now,const pet_replace_pack_t *pack,int *http)
{
    assert(worker==s_pet&&worker->has_context&&worker->authenticated&&now>=worker->retry_at_ms&&pack&&http);
    ++selects;selected_pack=*pack;
    if(select_answer!=PET_REPLACE_SELECT_CHOSEN){
        if(select_answer==PET_REPLACE_SELECT_UNAVAILABLE){*http=0;worker->retry_at_ms=now+30000;}else *http=409;
        return select_answer;
    }
    pet_control_context_t *c=&worker->cloud.context;
    const pet_release_v2_t *release=!strcmp(pack->build_id,other_release.pack.build_id)?&other_release:&signed_release;
    c->binding.assigned=true;strcpy(c->binding.build_id,pack->build_id);strcpy(c->binding.sha256,pack->sha256);
    snprintf(c->binding.revision,sizeof(c->binding.revision),"selected-%u",++select_revision);
    strcpy(c->binding.relationship_id,select_relationship);
    strcpy(c->config.face_id,release->face_id);strcpy(c->config.ai_pet_id,release->face_id);
    snprintf(c->config.version,sizeof(c->config.version),"%u",100+select_revision);
    if(worker->admitted)freeze(NULL);
    worker->admitted=false;*http=200;return PET_REPLACE_SELECT_CHOSEN;
}
bool pet_firmware_control_init(pet_firmware_control_t *worker,const pet_firmware_control_config_t *config,
                               pet_firmware_receipt_store_t *store)
{(void)worker;(void)config;(void)store;assert(!"workers are preset");return false;}
bool pet_replace_control_init(pet_replace_control_t *worker,const pet_replace_control_config_t *config,
                              pet_replace_journal_t *store,pet_replace_writer_t *writer)
{(void)worker;(void)config;(void)store;(void)writer;assert(!"workers are preset");return false;}
bool pet_setup_identity(char id[37],char credential[44]){(void)id;(void)credential;return true;}
/* Boot evidence and status. */
bool pet_firmware_image_bootloader_matches(const pet_firmware_bootloader_t *profile){assert(profile==&s_boot_profile);return true;}
bool pet_firmware_image_boot_state(pet_firmware_boot_state_t *out){*out=observed;return true;}
bool pet_setup_control_ready(void){return setup_ok;}
bool pet_network_wifi_is_ready(void){return wifi_ok;}
bool pet_onboarding_ui_ready(void){return ui_ok;}
bool pet_onboarding_setup_ready(void){return setup_ok;}
void pet_onboarding_open_from_ui(void){++setup_shows;}
void pet_onboarding_update_status(const pet_onboarding_status_t *status){visible_status=*status;}
const esp_app_desc_t *esp_app_get_description(void){static const esp_app_desc_t app={.version="host-fixture"};return &app;}
bool pet_audio_is_capturing(void){return false;}
bool pet_audio_is_playing(void){return false;}
/* Hardware seams that show no behavior of their own here. */
static bool display_locked;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out){(void)args;*out=NULL;return ESP_FAIL;}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t timeout){(void)timer;(void)timeout;return ESP_OK;}
esp_err_t pet_display_lock(int timeout){(void)timeout;assert(!display_locked);display_locked=true;return ESP_OK;}
void pet_display_unlock(void){assert(display_locked);display_locked=false;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (SemaphoreHandle_t)1;}
int xSemaphoreTake(SemaphoreHandle_t handle,TickType_t ticks){(void)handle;(void)ticks;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t handle){(void)handle;return pdTRUE;}
int xQueueSend(QueueHandle_t queue,const void *item,TickType_t ticks){(void)queue;(void)item;(void)ticks;return pdTRUE;}
void pet_audio_capture_stop(void){}
void pet_audio_playback_cancel(void){}
void pet_audio_set_speech_mouth_offset(int16_t offset_ms){(void)offset_ms;}
esp_err_t pet_config_store_speech_mouth_offset(bool present,int16_t offset_ms){(void)present;(void)offset_ms;return ESP_OK;}
esp_err_t pet_audio_set_capture_timeout(uint16_t seconds){(void)seconds;return ESP_OK;}
void pet_audio_set_volume(uint8_t volume){(void)volume;}
void pet_battery_set_enabled(bool enabled){(void)enabled;}
esp_err_t pet_battery_start(pet_battery_callback_t callback){(void)callback;return ESP_OK;}
void pet_diagnostics_note_dropped_app_event(void){}
void pet_enrollment_clear(void *data,size_t bytes){memset(data,0,bytes);}
esp_err_t pet_face_set_animation_profile(pet_animation_profile_t profile){(void)profile;return ESP_OK;}
esp_err_t pet_face_set_brightness(uint8_t brightness){(void)brightness;return ESP_OK;}
esp_err_t pet_face_set_recording_timeout(uint16_t seconds){(void)seconds;return ESP_OK;}
esp_err_t pet_face_set_shake_sensitivity(uint8_t sensitivity){(void)sensitivity;return ESP_OK;}
esp_err_t pet_face_set_speech_profile(pet_speech_profile_t mode,bool batch,bool realtime,bool fish)
{(void)mode;(void)batch;(void)realtime;(void)fish;return ESP_OK;}
void pet_motion_set_sensitivity(uint8_t sensitivity){(void)sensitivity;}
esp_err_t pet_motion_start(pet_motion_gesture_callback_t callback,pet_motion_enabled_callback_t enabled,uint8_t sensitivity)
{(void)callback;(void)enabled;(void)sensitivity;return ESP_OK;}
esp_err_t pet_sfx_init(pet_sfx_listen_done_t done){(void)done;return ESP_OK;}
void pet_sfx_cancel(void){}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps){(void)caps;return calloc(count,size);}
bool pet_firmware_receipt_open_nvs(pet_firmware_receipt_store_t *store){(void)store;return false;}
void esp_restart(void){assert(!"no restart");abort();}
pet_sfx_t pet_sfx_for_face_swipe(uint8_t index){(void)index;return PET_SFX_FACE_SWIPE_0;}

static void hex_sha256(const uint8_t *bytes,size_t length,char out[65])
{
    uint8_t digest[32];assert(!mbedtls_sha256(bytes,length,digest,0));
    for(size_t i=0;i<32;++i)snprintf(out+2*i,3,"%02x",digest[i]);
}
/* Two pets placed over USB in slots 0 and 2, Pablo's slot 0 on screen. */
static void usb_pets(pet_slot_inventory_t *inventory)
{
    assert(pet_slot_inventory_empty(inventory));
    for(unsigned slot=0;slot<PET_SLOT_COUNT;slot+=2){
        pet_slot_t *t=&inventory->slots[slot];
        t->state=PET_SLOT_READY;t->pack.bytes=(uint32_t)pack_length;t->validator_revision=FP_VALIDATOR_REVISION;
        snprintf(t->pack.build_id,sizeof(t->pack.build_id),"00000000-0000-4000-8000-00000000000%u",slot);
        hex_sha256(pack_bytes,pack_length,t->pack.sha256);
    }
    inventory->active=0;
}
static void boot(const pet_slot_inventory_t *inventory)
{
    stored=*inventory;s_shown_slot=-1;s_selection_save_at=0;drawing_copy=false;
    free(s_shown_pack);s_shown_pack=NULL;memset(&s_prepared,0,sizeof(s_prepared));
    pocket_show_active();s_slots.machine.state=s_slots.inventory.state.operation;s_slots.installer=true;
}
/* A website installation through the real inventory: it lands in the free slot. */
static void step(pet_replace_t *next)
{
    assert(pet_slot_inventory_apply(&s_slots.inventory.state,next,FP_VALIDATOR_REVISION));
    stored=s_slots.inventory.state;s_slots.machine.state=*next;
}
static void request_installation(const pet_replace_pack_t *pack,const char *request,const char *expected)
{
    pet_replace_t next=s_slots.inventory.state.operation;
    assert(pet_replace_request(&next,request,pack,expected));step(&next);
}
static void download(const pet_replace_pack_t *pack)
{
    pet_replace_t next=s_slots.inventory.state.operation;
    assert(pet_replace_fenced(&next,"00000000-0000-4000-8000-000000000012","00000000-0000-4000-8000-000000000013",pack));step(&next);
    assert(pet_replace_invalidate(&next));step(&next);
    assert(pet_replace_begin_download(&next));step(&next);
    assert(pet_replace_progress(&next,pack->bytes,pack->sha256));step(&next);
}
static void to_activating(const pet_replace_pack_t *pack)
{
    pet_replace_t next=s_slots.inventory.state.operation;
    assert(pet_replace_verified(&next,pack->sha256));step(&next);
    assert(pet_replace_activate(&next));step(&next);
}
/* Committed: the cloud's binding names this pet. One worker step. */
static void commit_installation(const pet_replace_pack_t *pack,const char *binding)
{
    pet_replace_t next=s_slots.inventory.state.operation;
    assert(pet_replace_commit(&next,next.operation_id,next.fence_id,pack,binding,"00000000-0000-4000-8000-000000000014","7"));
    step(&next);
    pet_control_context_t *c=&pet_worker.cloud.context;
    c->binding.assigned=true;strcpy(c->binding.build_id,next.active.build_id);strcpy(c->binding.sha256,next.active.sha256);
    strcpy(c->binding.revision,next.binding_revision);strcpy(c->binding.relationship_id,next.relationship_id);
    strcpy(c->config.face_id,signed_release.face_id);strcpy(c->config.version,"7");
}
static void finish_installation(const pet_replace_pack_t *pack,const char *binding)
{to_activating(pack);commit_installation(pack,binding);}
/* Control authenticating again: a pass offline, then a pass online. */
static void reconnect(void)
{
    pet_worker.authenticated=false;clock_ms+=1000;pocket_tick(esp_timer_get_time());
    pet_worker.authenticated=true;clock_ms+=1000;pocket_tick(esp_timer_get_time());
}
/* A swipe to the next installed pet, and the pass after it settles. */
static void swipe_and_settle(void)
{
    pocket_switch(1);
    pet_worker.authenticated=true; /* Landing on the bound pet re-polls: the worker's next step. */
    clock_ms+=3000;pocket_tick(esp_timer_get_time());
}

static void same(const pet_firmware_pack_identity_t *a,const pet_replace_pack_t *b)
{assert(a->requirements.present&&a->requirements.bytes==b->bytes&&!strcmp(a->build_id,b->build_id)&&!strcmp(a->sha256,b->sha256));}
/* Firmware updates protect every installed pet, each proved by the signed
 * record in its slot: the pet on screen (else the bound one) is active, a slot
 * being written is interrupted, the others are installed. */
static void firmware_protection_checks(void)
{
    pet_slot_inventory_t pets;assert(pet_slot_inventory_empty(&pets));
    pet_replace_pack_t pack[PET_SLOT_COUNT];
    for(unsigned slot=0;slot<PET_SLOT_COUNT;++slot){
        pack[slot]=signed_release.pack;snprintf(pack[slot].build_id,sizeof(pack[slot].build_id),"00000000-0000-4000-8000-00000000020%u",slot);
        pets.slots[slot]=(pet_slot_t){.state=PET_SLOT_READY,.pack=pack[slot],.validator_revision=FP_VALIDATOR_REVISION,.shown_at=slot+1};
        record_of[slot]=pack[slot];
    }
    pets.active=1;pets.selection_revision=3;
    assert(pet_slot_inventory_rebind(&pets,2,"binding-9","00000000-0000-4000-8000-000000000014","7"));
    s_slots.open=s_slots.inventory.ready=true;s_slots.inventory.state=stored=pets;s_slots.machine.state=pets.operation;
    pet_firmware_protection_t p;
    pocket_protection(NULL,true,&p);
    assert(p.known&&!p.interrupted.requirements.present&&p.installed_count==2);
    same(&p.active,&pack[1]);same(&p.installed[0],&pack[0]);same(&p.installed[1],&pack[2]);
    assert(p.active.requirements.imported_release&&p.active.requirements.layout_id==PET_LAYOUT_THREE_3P5M);
    s_slots.inventory.state.active=-1;pocket_protection(NULL,true,&p);
    assert(p.known&&p.installed_count==2);same(&p.active,&pack[2]);same(&p.installed[0],&pack[0]);same(&p.installed[1],&pack[1]);
    s_slots.inventory.state.active=1;
    // One slot it cannot prove, such as a pet placed over USB, makes it unknown;
    // so do a record that is unreadable or unsigned, no authenticated account,
    // a store that stopped committing, and another partition table.
    unsigned_slots=1u<<0;pocket_protection(NULL,true,&p);assert(!p.known&&!p.installed_count&&!p.active.requirements.present);unsigned_slots=0;
    record_of[0]=pack[2];pocket_protection(NULL,true,&p);assert(!p.known);record_of[0]=pack[0];
    record_ok=false;pocket_protection(NULL,true,&p);assert(!p.known);record_ok=true;
    signature_ok=false;pocket_protection(NULL,true,&p);assert(!p.known);signature_ok=true;
    pet_worker.has_context=false;pocket_protection(NULL,true,&p);assert(!p.known);pet_worker.has_context=true;
    s_slots.inventory.ready=false;pocket_protection(NULL,true,&p);assert(!p.known);s_slots.inventory.ready=true;
    physical_hash[0]='e';pocket_protection(NULL,true,&p);assert(!p.known);physical_hash[0]='c';
    pocket_protection(NULL,true,&p);assert(p.known);
    // Polls and checks reuse the last successful proof until the inventory
    // commits again, the account or the store's partition table differs:
    // nothing is read. Flash actions ask for a fresh proof.
    unsigned reads=record_reads;unsigned_slots=1u<<0;
    pocket_protection(NULL,false,&p);assert(p.known&&record_reads==reads);
    pocket_protection(NULL,true,&p);assert(!p.known&&record_reads>reads);
    // A failure is not kept: the next poll proves again, so a transient read
    // or signature error clears itself.
    reads=record_reads;pocket_protection(NULL,false,&p);assert(!p.known&&record_reads>reads);
    unsigned_slots=0;pocket_protection(NULL,false,&p);assert(p.known);
    reads=record_reads;pocket_protection(NULL,false,&p);assert(p.known&&record_reads==reads);
    ++s_slots.inventory.journal.generation;pocket_protection(NULL,false,&p);assert(p.known&&record_reads>reads);
    reads=record_reads;pocket_protection(NULL,false,&p);assert(p.known&&record_reads==reads);
    strcpy(pet_worker.cloud.context.account_id,"00000000-0000-4000-8000-000000000099");
    pocket_protection(NULL,false,&p);assert(!p.known&&record_reads>reads);
    strcpy(pet_worker.cloud.context.account_id,signed_release.account_id);pocket_protection(NULL,false,&p);assert(p.known);
    s_slots.partition_sha256[0]='e';pocket_protection(NULL,false,&p);assert(!p.known);
    s_slots.partition_sha256[0]='c';pocket_protection(NULL,false,&p);assert(p.known);
    s_slots.inventory.ready=false;pocket_protection(NULL,false,&p);assert(!p.known);s_slots.inventory.ready=true;
    // Mid-replacement: the stalest pet neither shown nor bound is replaced.
    pet_replace_pack_t fourth=signed_release.pack;strcpy(fourth.build_id,"00000000-0000-4000-8000-000000000041");
    request_installation(&fourth,"00000000-0000-4000-8000-000000000042","binding-9");
    assert(s_slots.inventory.state.target==0);
    pocket_protection(NULL,true,&p);assert(!p.known); /* Requested: one record cannot prove both pets. */
    pet_replace_t next=s_slots.inventory.state.operation;
    assert(pet_replace_fenced(&next,"00000000-0000-4000-8000-000000000043","00000000-0000-4000-8000-000000000044",&fourth));step(&next);
    pocket_protection(NULL,true,&p);assert(!p.known);
    assert(pet_replace_invalidate(&next));step(&next);
    pocket_protection(NULL,true,&p);assert(!p.known); /* The slot still holds the old pet's record. */
    assert(pet_replace_begin_download(&next));step(&next);record_of[0]=fourth;
    pocket_protection(NULL,true,&p);
    assert(p.known&&p.installed_count==1);same(&p.interrupted,&fourth);same(&p.active,&pack[1]);same(&p.installed[0],&pack[2]);
    assert(pet_replace_cancel(&next)&&next.phase==PET_REPLACE_RECOVERY);step(&next);
    pocket_protection(NULL,true,&p);assert(p.known&&p.installed_count==1);same(&p.interrupted,&fourth);
    // The same protection feeds the firmware's own acceptance check.
    pet_firmware_release_t candidate={.bytes=2228192,.requirements={.layout=observed.layout,.firmware_epoch=3,.formats=6,.codecs=0xff,
        .imported_release=true,.bootloader=s_boot_profile}};
    strcpy(candidate.requirements.partition_sha256,observed.partition_sha256);
    assert(pet_firmware_release_compatible(&candidate,&observed.layout,observed.partition_sha256,&s_boot_profile,&p));
    candidate.requirements.imported_release=false;
    assert(!pet_firmware_release_compatible(&candidate,&observed.layout,observed.partition_sha256,&s_boot_profile,&p));
    memset(record_of,0,sizeof(record_of));
}
/* The updater confirms new firmware, ending the automatic rollback, only when
 * a pet was drawn with audio this boot: with any pet ready a store that never
 * showed one is unhealthy. The menu may cover the pet afterwards. Installing
 * a pet still accepts recovery (pocket_health is unchanged). */
static void firmware_health_checks(void)
{
    pet_slot_inventory_t pets;assert(pet_slot_inventory_empty(&pets));
    for(unsigned slot=0;slot<PET_SLOT_COUNT;++slot){
        pet_replace_pack_t pack=signed_release.pack;snprintf(pack.build_id,sizeof(pack.build_id),"00000000-0000-4000-8000-00000000030%u",slot);
        pets.slots[slot]=(pet_slot_t){.state=PET_SLOT_READY,.pack=pack,.validator_revision=FP_VALIDATOR_REVISION,.shown_at=slot+1};
    }
    pets.active=0;s_slots.open=s_slots.inventory.ready=true;s_slots.inventory.state=stored=pets;s_slots.machine.state=pets.operation;
    setup_ok=wifi_ok=ui_ok=true;s_audio_started=true;s_pet_drawn=false;atomic_store(&s_runtime_fault,false);
    // Every slot ready, but this image shows no pet (no copy buffer, or its
    // validator refused the packs): the deadline must roll it back.
    s_shown_slot=-1;drawing_copy=false;
    assert(pocket_health(NULL)==PET_FW_STORAGE_RECOVERY&&pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);
    s_shown_slot=0;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE); /* Nothing drawn. */
    drawing_copy=true;s_audio_started=false;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);
    pocket_tick(esp_timer_get_time());assert(!s_pet_drawn); /* A pet drawn without audio proves nothing. */
    s_audio_started=true;assert(pocket_health(NULL)==PET_FW_STORAGE_PACK&&pocket_firmware_health(NULL)==PET_FW_STORAGE_PACK);
    wifi_ok=false;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);wifi_ok=true;
    // The owner opens the menu over the pet after the update's reboot: the pet
    // was drawn with audio this boot, so the update still confirms.
    pocket_tick(esp_timer_get_time());assert(s_pet_drawn);
    drawing_copy=false; /* The menu screen replaced the pet screen; it stays live. */
    assert(pocket_health(NULL)==PET_FW_STORAGE_RECOVERY&&pocket_firmware_health(NULL)==PET_FW_STORAGE_PACK);
    atomic_store(&s_runtime_fault,true);assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);
    atomic_store(&s_runtime_fault,false);
    ui_ok=false;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);ui_ok=true; /* No live screen at all. */
    s_pet_drawn=false;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);
    // Only a slot mid-installation, or no pet at all: recovery or setup.
    s_shown_slot=-1;drawing_copy=false;
    s_slots.inventory.state.slots[0].state=s_slots.inventory.state.slots[2].state=PET_SLOT_FREE;
    s_slots.inventory.state.slots[1]=(pet_slot_t){.state=PET_SLOT_INSTALLING};
    assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_RECOVERY);
    s_slots.inventory.state.slots[1].state=PET_SLOT_FREE;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_SETUP);
    // A store that did not open is no proof of an empty one.
    s_slots.open=false;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);s_slots.open=true;
    s_slots.inventory.ready=false;assert(pocket_firmware_health(NULL)==PET_FW_STORAGE_NONE);s_slots.inventory.ready=true;
    s_slots.inventory.state=stored=pets;
    // The updater uses this check and the cached protection.
    pocket_control_start();
    assert(s_pocket_firmware.health==pocket_firmware_health&&s_pocket_firmware.protection==pocket_protection);
    assert(s_pocket_pet.compatible_healthy==pocket_compatible);
}
static void library_removal(void)
{
    pet_slot_inventory_t pets;
    usb_pets(&pets);
    pets.slots[1] = pets.slots[0];
    pets.slots[1].pack.build_id[35] = '1';
    assert(pet_slot_inventory_rebind(&pets, 2, "other-binding", select_relationship, "9"));
    boot(&pets);
    pet_worker.has_context = pet_worker.authenticated = pet_worker.admitted = true;
    pet_worker.cloud.has_operation = false;
    memset(&pet_worker.cloud.context.binding, 0, sizeof(pet_worker.cloud.context.binding));
    atomic_store(&s_resources.owner, PET_RESOURCE_VOICE);
    assert(!pocket_remove(NULL, &pets.slots[1].pack));
    atomic_store(&s_resources.owner, PET_RESOURCE_CONTROL);
    firmware_reserved = true;
    assert(!pocket_remove(NULL, &pets.slots[1].pack));
    firmware_reserved = false;
    pet_worker.cloud.has_operation = true;
    assert(!pocket_remove(NULL, &pets.slots[1].pack));
    pet_worker.cloud.has_operation = false;
    unsigned writes = commits, stopped = freezes;
    pet_replace_pack_t absent = pets.slots[1].pack;
    ++absent.bytes;
    assert(pocket_remove(NULL, &absent) && commits == writes && freezes == stopped);
    assert(pocket_remove(NULL, &pets.slots[1].pack));
    assert(s_shown_slot == 0 && drawing_copy && freezes == stopped && pet_worker.admitted);
    assert(s_slots.inventory.state.bound == 2 &&
           !memcmp(&s_slots.inventory.state.operation, &pets.operation, sizeof(pets.operation)));

    /* Failed commit leaves the installed target; successful commit followed
     * by failed display loading does not acknowledge it until replay detaches. */
    commit_fails = true;
    assert(!pocket_remove(NULL, &pets.slots[0].pack));
    assert(s_slots.inventory.state.slots[0].state == PET_SLOT_READY);
    commit_fails = false;
    load_fail_mask = 1u << 2;
    assert(!pocket_remove(NULL, &pets.slots[0].pack));
    assert(s_slots.inventory.state.slots[0].state == PET_SLOT_FREE && s_shown_slot == 0);
    writes = commits;
    load_fail_mask = 0;
    assert(pocket_remove(NULL, &pets.slots[0].pack));
    assert(commits == writes && s_shown_slot == 2 && drawing_copy && s_slots.inventory.state.bound == 2);
    assert(s_select_moment == SELECT_AFTER_SWIPE && !pet_worker.admitted);
    pet_worker.removal_ack_count = 1;
    unsigned asked = selects;
    pocket_select(clock_ms);
    assert(selects == asked); /* Do not rebind before the removal batch is acknowledged. */
    pet_worker.removal_ack_count = 0;

    /* The last pet becomes empty setup. Lost ack/reboot is an absence replay. */
    setup_ok = false;
    assert(!pocket_remove(NULL, &pets.slots[2].pack));
    assert(s_slots.inventory.state.active == -1 && s_shown_slot == 2);
    setup_ok = true;
    assert(pocket_remove(NULL, &pets.slots[2].pack));
    assert(s_shown_slot == -1 && s_copy_slot == -1 && !drawing_copy && setup_shows == 1);
    assert(!s_prepared[0].bytes && s_slots.inventory.state.operation.phase == PET_REPLACE_EMPTY);
    const pet_slot_inventory_t empty = stored;
    boot(&empty);
    writes = commits;
    assert(pocket_remove(NULL, &pets.slots[2].pack) && commits == writes && setup_shows == 1);
    /* A different pet was already on screen when the bound one was deleted:
     * retain its pixels and request its voice binding after the ack. */
    boot(&pets);
    s_select_moment = SELECT_NONE;
    assert(pocket_remove(NULL, &pets.slots[2].pack));
    assert(s_shown_slot == 0 && drawing_copy && s_slots.inventory.state.bound == -1);
    assert(s_select_moment == SELECT_AFTER_SWIPE);
    atomic_store(&s_resources.owner, PET_RESOURCE_FREE);
}

int main(void)
{
    pack_length=fread(pack_bytes,1,sizeof(pack_bytes),stdin);assert(pack_length&&pack_length<sizeof(pack_bytes));
    memcpy(target_bytes,pack_bytes,pack_length);
    s_audio_lock=xSemaphoreCreateMutex();s_identity_ready=true;s_boot_health_decided=true;
    strcpy(s_id,"00000000-0000-4000-8000-000000000001");s_origin="https://fixture.invalid";
    s_boot_profile.bytes=4096;memset(s_boot_profile.sha256,'d',64);memset(physical_hash,'c',64);
    assert(pet_flash_layout_known(PET_LAYOUT_THREE_3P5M,&observed.layout));memset(observed.partition_sha256,'c',64);
    observed.running_slot=0;observed.selection.active=0;
    observed.selection.records[0]=(pet_ota_record_t){.sequence=1,.state=PET_OTA_STATE_VALID,.slot=0,.crc_valid=true,.bootable=true};
    /* The release the website installs: the fixture's exact bytes, signed for this layout. */
    signed_release.pack.bytes=(uint32_t)pack_length;hex_sha256(pack_bytes,pack_length,signed_release.pack.sha256);
    strcpy(signed_release.pack.build_id,"00000000-0000-4000-8000-000000000002");
    strcpy(signed_release.account_id,"00000000-0000-4000-8000-000000000003");
    strcpy(signed_release.project_id,"00000000-0000-4000-8000-000000000004");
    strcpy(signed_release.face_id,"big-sal");strcpy(signed_release.version,"fixture-240-v2");
    signed_release.requirements=(pet_firmware_protected_pack_t){.present=true,.layout_id=PET_LAYOUT_THREE_3P5M,
        .minimum_firmware_epoch=3,.bytes=(uint32_t)pack_length,.format=2,.resolution_divisor=2,.codecs=32};
    strcpy(signed_release.requirements.partition_sha256,observed.partition_sha256);
    s_pet=&pet_worker;s_firmware=&firmware_worker;
    select_answer=PET_REPLACE_SELECT_REFUSED; /* The cloud refuses selection until its section below. */
    pet_worker.has_context=pet_worker.authenticated=true;
    strcpy(pet_worker.cloud.context.account_id,signed_release.account_id);strcpy(pet_worker.cloud.context.device_id,s_id);
    firmware_worker.authenticated=true;firmware_worker.next_poll_ms=clock_ms+15000;

    pet_slot_inventory_t inventory;usb_pets(&inventory);boot(&inventory);
    assert(s_shown_slot==0&&drawing_copy&&!online_starts);

    // Firmware updates are refused: pets placed over USB have no signed record.
    pet_firmware_protection_t protection;memset(&protection,0xff,sizeof(protection));
    pocket_protection(NULL,true,&protection);assert(!protection.known&&!protection.installed_count&&!protection.active.requirements.present);

    // A release is admitted only on this healthy, qualified three-pet boot.
    wifi_ok=false;assert(!pocket_compatible(NULL,&signed_release));wifi_ok=true;
    firmware_worker.authenticated=false;assert(!pocket_compatible(NULL,&signed_release));firmware_worker.authenticated=true;
    firmware_worker.next_poll_ms=clock_ms;assert(!pocket_compatible(NULL,&signed_release));firmware_worker.next_poll_ms=clock_ms+15000;
    observed.selection.records[0].state=PET_OTA_STATE_PENDING;assert(!pocket_compatible(NULL,&signed_release));
    observed.selection.records[0].state=PET_OTA_STATE_VALID;
    observed.partition_sha256[0]='e';assert(!pocket_compatible(NULL,&signed_release));observed.partition_sha256[0]='c';
    signed_release.requirements.layout_id=PET_LAYOUT_SINGLE_2M;assert(!pocket_compatible(NULL,&signed_release));
    signed_release.requirements.layout_id=PET_LAYOUT_THREE_3P5M;
    assert(!s_pocket_boot_healthy&&pocket_compatible(NULL,&signed_release)&&s_pocket_boot_healthy);
    // Codex pets are imported releases, which this firmware verifies; the
    // rest of this installation is one, named as the cloud names it:
    // `<pack ID>-<first 8 hex of its project>` (importedFaceId).
    signed_release.requirements.imported_release=signed_release.imported=true;
    strcpy(signed_release.face_id,"big-sal-7c4253d1");assert(pocket_compatible(NULL,&signed_release));

    // The download lands in the free slot, never the pet on screen.
    request_installation(&signed_release.pack,"00000000-0000-4000-8000-000000000011","r0");download(&signed_release.pack);
    assert(s_slots.inventory.state.target==1&&s_slots.inventory.state.slots[1].state==PET_SLOT_INSTALLING);
    assert(pocket_detach(NULL)&&s_shown_slot==0);
    publish_status(0);assert(visible_status.installation==PET_ONBOARDING_INSTALL_VERIFYING&&visible_status.installation_percent==100);

    // Verification checks the downloaded pet in the screen's copy, with the
    // pet on screen holding its frame, and copies that pet back after.
    unsigned before_holds=holds,before_loads=loads;
    assert(pocket_verify(NULL,&signed_release));
    assert(target_loads==1&&holds==before_holds+2&&loads==before_loads+1&&drawing_copy&&s_shown_slot==0);
    target_bytes[pack_length/2]^=1;assert(!pocket_verify(NULL,&signed_release));target_bytes[pack_length/2]^=1;
    assert(drawing_copy&&s_shown_slot==0);
    target_ok=false;assert(!pocket_verify(NULL,&signed_release));target_ok=true;assert(drawing_copy&&s_shown_slot==0);
    // The derived face ID is accepted only from a signed imported release.
    signed_release.imported=false;assert(!pocket_verify(NULL,&signed_release));signed_release.imported=true;
    assert(s_copy_slot==0);
    firmware_worker.authenticated=false;before_holds=holds;
    assert(!pocket_verify(NULL,&signed_release)&&holds==before_holds);firmware_worker.authenticated=true;

    // Activation shows the new pet and binds the conversation to it.
    finish_installation(&signed_release.pack,"binding-1");
    assert(s_slots.inventory.state.active==1&&s_slots.inventory.state.bound==1&&s_shown_slot==0);
    assert(pocket_activate(NULL,&pet_worker.cloud.context));
    assert(s_shown_slot==1&&online_starts==1&&!strcmp(online_build,signed_release.pack.build_id));
    // The imported pet is prepared, registered and shown under its derived face ID.
    assert(!strcmp(s_prepared[0].manifest.face_id,"big-sal-7c4253d1")&&!strcmp(bound_face_id,"big-sal-7c4253d1"));
    assert(!strcmp(s_prepared[0].manifest.account_id,signed_release.account_id)&&s_prepared[0].bytes==s_shown_pack);
    publish_status(0);assert(visible_status.installation==PET_ONBOARDING_INSTALL_READY&&visible_status.has_pet);
    // Its signed record proves the new pet, but the USB pets still block updates.
    pocket_protection(NULL,true,&protection);assert(!protection.known);
    /* The menu sees three slots, the new pet on screen, and that it can talk. */
    assert(visible_status.pet_capacity==3&&visible_status.pet_index>=1&&visible_status.pet_index<=visible_status.pet_count);
    atomic_store(&s_volume_now,55);atomic_store(&s_brightness_now,70);publish_status(0);
    assert(visible_status.pet_linked&&visible_status.volume_valid&&visible_status.volume==55&&visible_status.brightness==70);
    assert(atomic_load(&s_voice_block)==VOICE_BLOCK_CONNECTING);

    // Only the exact bound, signed pet talks.
    pet_control_context_t wrong=pet_worker.cloud.context;
    wrong.binding.sha256[0]^=1;assert(!pocket_activate(NULL,&wrong));
    wrong=pet_worker.cloud.context;wrong.binding.revision[0]^=1;assert(!pocket_activate(NULL,&wrong));
    wrong=pet_worker.cloud.context;strcpy(wrong.config.face_id,"another-pet");assert(!pocket_activate(NULL,&wrong));
    wrong=pet_worker.cloud.context;wrong.account_id[0]^=1;assert(!pocket_activate(NULL,&wrong));
    signature_ok=false;assert(!pocket_activate(NULL,&pet_worker.cloud.context));signature_ok=true;
    record_ok=false;assert(!pocket_activate(NULL,&pet_worker.cloud.context));record_ok=true;
    assert(online_starts==1);

    // Another pet the owner swipes to stays on screen, offline.
    pocket_switch(1);assert(s_shown_slot==2&&s_selection_save_at);
    assert(pocket_activate(NULL,&pet_worker.cloud.context)&&s_shown_slot==2&&online_starts==1);
    persist_selection();s_selection_save_at=0;assert(stored.active==2);
    assert(pocket_activate(NULL,&pet_worker.cloud.context)&&s_shown_slot==2&&online_starts==1);
    // Back on the bound pet, the worker reconciles and it talks again.
    pet_worker.admitted=true;unsigned before_disconnects=disconnects;
    pocket_switch(1);pocket_switch(1);
    assert(s_shown_slot==1&&disconnects==before_disconnects+1&&!pet_worker.admitted&&!pet_worker.authenticated);
    pet_worker.authenticated=true; /* The worker's next poll, then its mount. */
    assert(pocket_activate(NULL,&pet_worker.cloud.context)&&s_shown_slot==1&&online_starts==2);
    // The copy is reloaded whenever it may not hold the bound pet's bytes,
    // e.g. after a verification whose pet could not be copied back.
    unsigned before_reload=loads;s_copy_slot=-1;pet_worker.authenticated=true;
    assert(pocket_activate(NULL,&pet_worker.cloud.context)&&s_copy_slot==1&&loads==before_reload+1&&online_starts==3);
    before_reload=loads;assert(pocket_activate(NULL,&pet_worker.cloud.context)&&loads==before_reload&&online_starts==4);

    // A pet placed over USB has no signed record, so it cannot be the binding.
    wrong=pet_worker.cloud.context;
    s_slots.inventory.state.bound=0;record_ok=false;assert(!pocket_activate(NULL,&wrong));
    s_slots.inventory.state.bound=1;record_ok=true;

    // The pet being replaced is never on screen while it is written.
    pet_slot_inventory_t *live=&s_slots.inventory.state;
    live->target=2;live->slots[2].state=PET_SLOT_INSTALLING;live->active=1;s_shown_slot=2;
    assert(pocket_detach(NULL)&&s_shown_slot==1);
    // Updating the only pet it could show holds the display instead.
    s_shown_slot=2;live->active=2;assert(pocket_detach(NULL)&&s_shown_slot==-1);
    live->target=-1;assert(!pocket_detach(NULL));
    live->active=1;live->slots[2].state=PET_SLOT_READY;assert(show_slot(1,false));

    // Every inventory commit advances the journal generation the installer's
    // receipts carry, so a swipe is saved only between installations. Before
    // a queued installation picks its target it is saved at once.
    unsigned before_commits=commits;
    pet_worker.cloud.has_operation=true;pet_worker.cloud.operation.phase=PET_CLOUD_QUEUED;
    pocket_switch(1);assert(s_shown_slot==2&&s_selection_save_at);
    pocket_tick(esp_timer_get_time());assert(!s_selection_save_at&&commits==before_commits+1&&stored.active==2);
    // The next installation replaces the stalest pet neither on screen nor bound.
    pet_replace_pack_t second=signed_release.pack;strcpy(second.build_id,"00000000-0000-4000-8000-000000000021");
    request_installation(&second,"00000000-0000-4000-8000-000000000022","binding-1");
    assert(live->target==0&&s_shown_slot==2&&live->bound==1);
    pet_worker.cloud.operation.phase=PET_CLOUD_FENCED;
    uint64_t generation=s_slots.inventory.journal.generation;before_commits=commits;
    pocket_switch(-1);assert(s_shown_slot==1&&s_selection_save_at);
    clock_ms+=3000;pocket_tick(esp_timer_get_time());
    assert(s_selection_save_at&&commits==before_commits&&s_slots.inventory.journal.generation==generation);
    download(&second);clock_ms+=3000;pocket_tick(esp_timer_get_time());assert(s_selection_save_at&&commits==before_commits);
    // A pack proved by an older validator is shown, but its proof waits too.
    live->slots[2].validator_revision=0xffff;pocket_switch(1);
    assert(s_shown_slot==2&&commits==before_commits&&live->slots[2].validator_revision==0xffff);
    // The commit makes the new pet the selection; the swipe made during the
    // installation is older and is dropped. Activation shows the new pet.
    to_activating(&second);signed_release.pack=second;simulate_commit=true;pocket_tick(esp_timer_get_time());
    assert(live->target==-1&&live->active==0&&live->bound==0&&!s_selection_save_at&&commits==before_commits);
    pet_worker.authenticated=true;
    assert(pocket_activate(NULL,&pet_worker.cloud.context)&&s_shown_slot==0&&online_starts==5);
    assert(!strcmp(online_build,second.build_id));
    // While the cloud still reports the finished operation, a swipe waits;
    // once it is gone the swipe is saved after it settles.
    pet_worker.cloud.operation.phase=PET_CLOUD_INSTALLED;
    pocket_switch(1);assert(s_selection_save_at);unsigned installed_commits=commits;
    clock_ms+=3000;pocket_tick(esp_timer_get_time());assert(s_selection_save_at&&commits==installed_commits);
    pet_worker.cloud.has_operation=false;pocket_tick(esp_timer_get_time());
    assert(!s_selection_save_at&&commits==installed_commits+1&&stored.active==s_shown_slot);

    // Swiping away from the talking bound pet pauses its voice; when no other
    // pet can show, the bound pet returns and, with no open session to reuse,
    // is readmitted.
    assert(show_slot(0,false));pet_worker.admitted=true;unsigned before_freezes=freezes;before_disconnects=disconnects;
    load_fail_mask=(1u<<1)|(1u<<2);pocket_switch(1);load_fail_mask=0;
    assert(s_shown_slot==0&&freezes>before_freezes&&disconnects==before_disconnects+1&&!pet_worker.admitted);

    // A failed journal write leaves the store refusing commits: it reopens,
    // with the installer attached again and the pet still on screen.
    unsigned before_opens=opens;s_slots.inventory.ready=false;pet_worker.verified=true;
    clock_ms+=16000;pocket_tick(esp_timer_get_time());
    assert(opens==before_opens+1&&s_slots.inventory.ready&&s_slots.installer&&!pet_worker.verified&&drawing_copy);
    // If reopening fails too, the store is retried until it opens; meanwhile
    // the pet stays on screen and the status reports the storage problem.
    s_slots.inventory.ready=false;open_fails=true;clock_ms+=16000;pocket_tick(esp_timer_get_time());
    assert(!s_slots.open&&s_store_lost&&drawing_copy&&installation_state()==PET_ONBOARDING_INSTALL_FAILED);
    unsigned lost_shows=binds;pocket_switch(1);assert(binds==lost_shows);
    open_fails=false;clock_ms+=16000;pocket_tick(esp_timer_get_time());
    assert(s_slots.open&&!s_store_lost&&s_slots.installer&&opens==before_opens+3);

    // An installation cancelled mid-download leaves its slot in recovery, and
    // the inventory stays as the cloud last saw it until that is resolved.
    pet_replace_pack_t third=signed_release.pack;strcpy(third.build_id,"00000000-0000-4000-8000-000000000031");
    request_installation(&third,"00000000-0000-4000-8000-000000000032",live->operation.binding_revision);
    pet_replace_t next=live->operation;
    assert(pet_replace_fenced(&next,"00000000-0000-4000-8000-000000000033","00000000-0000-4000-8000-000000000034",&third));step(&next);
    assert(pet_replace_invalidate(&next));step(&next);
    assert(pet_replace_cancel(&next)&&next.phase==PET_REPLACE_RECOVERY);step(&next);
    assert(live->target>=0&&live->bound==-1);
    pet_worker.cloud.has_operation=true;pet_worker.cloud.operation.phase=PET_CLOUD_FENCED;
    unsigned recovery_commits=commits;pocket_switch(1);assert(s_selection_save_at);
    clock_ms+=3000;pocket_tick(esp_timer_get_time());assert(s_selection_save_at&&commits==recovery_commits);
    pet_worker.cloud.has_operation=false;clock_ms+=3000;pocket_tick(esp_timer_get_time());
    assert(s_selection_save_at&&commits==recovery_commits);

    // ---- The voice follows the pet on screen (the three-slot contract's Selection).
    // Pablo placed over USB in slot 0 (no signed record), and two signed cloud
    // pets: slot 1 bound and on screen, slot 2 installed earlier.
    const char *builds[PET_SLOT_COUNT]={"00000000-0000-4000-8000-000000000040","00000000-0000-4000-8000-000000000041",
        "00000000-0000-4000-8000-000000000042"};
    pet_slot_inventory_t three;assert(pet_slot_inventory_empty(&three));
    for(unsigned slot=0;slot<PET_SLOT_COUNT;++slot){
        pet_slot_t *t=&three.slots[slot];t->state=PET_SLOT_READY;t->pack.bytes=(uint32_t)pack_length;
        t->validator_revision=FP_VALIDATOR_REVISION;t->shown_at=slot+1;strcpy(t->pack.build_id,builds[slot]);
        hex_sha256(pack_bytes,pack_length,t->pack.sha256);
    }
    three.active=1;three.selection_revision=3;
    assert(pet_slot_inventory_rebind(&three,1,"binding-41","00000000-0000-4000-8000-000000000050","20"));
    signed_release.pack=three.slots[1].pack;other_release=signed_release;other_release.pack=three.slots[2].pack;
    unsigned_slots=1u<<0;
    pet_control_context_t *cloud=&pet_worker.cloud.context;
    pet_worker.cloud.has_operation=pet_worker.admitted=false;pet_worker.has_context=pet_worker.authenticated=true;
    cloud->binding=(pet_control_binding_t){.assigned=true,.revision="binding-41",.relationship_id="00000000-0000-4000-8000-000000000050"};
    strcpy(cloud->binding.build_id,builds[1]);strcpy(cloud->binding.sha256,three.slots[1].pack.sha256);strcpy(cloud->config.version,"20");
    boot(&three);assert(s_shown_slot==1&&live->bound==1);
    s_select_moment=SELECT_NONE;s_select_authenticated=false;s_select_refused_build[0]=0;
    select_answer=PET_REPLACE_SELECT_CHOSEN;const unsigned asked=selects;
    // At a reconnect the bound pet on screen needs nothing.
    reconnect();assert(selects==asked&&s_select_moment==SELECT_NONE);
    // Swiping to the other signed pet asks once the swipe settles, and the
    // inventory rebinds that slot to the cloud's binding.
    pocket_switch(1);assert(s_shown_slot==2);pocket_tick(esp_timer_get_time());assert(selects==asked);
    clock_ms+=3000;pocket_tick(esp_timer_get_time());
    assert(selects==asked+1&&!strcmp(selected_pack.build_id,builds[2])&&live->bound==2&&live->active==2);
    assert(!strcmp(live->operation.active.build_id,builds[2])&&!strcmp(live->operation.binding_revision,cloud->binding.revision)&&
        !strcmp(live->operation.relationship_id,select_relationship)&&!strcmp(live->operation.config_version,cloud->config.version));
    // The worker's next step mounts it: the slot's signed record matches the binding, and it talks.
    unsigned started=online_starts;
    assert(pocket_activate(NULL,cloud)&&online_starts==started+1&&!strcmp(online_build,builds[2]));
    atomic_store(&s_handshake_pending,false);
    publish_status(0);assert(visible_status.pet_linked&&atomic_load(&s_voice_block)==VOICE_BLOCK_CONNECTING);
    // A pet placed over USB has no cloud identity: never asked for, and
    // remembered like a refusal, so a reconnect does not read its record again.
    swipe_and_settle();assert(s_shown_slot==0&&selects==asked+1&&live->bound==2&&!strcmp(s_select_refused_build,builds[0]));
    unsigned reads=record_reads;reconnect();assert(selects==asked+1&&record_reads==reads);
    publish_status(0);assert(!visible_status.pet_linked&&atomic_load(&s_voice_block)==VOICE_BLOCK_NOT_LINKED);
    // A final refusal keeps the pet on screen without voice. Nothing retries on
    // a timer, and a reconnect asks again only once something changed.
    select_answer=PET_REPLACE_SELECT_REFUSED;
    swipe_and_settle();assert(s_shown_slot==1&&selects==asked+2&&!strcmp(selected_pack.build_id,builds[1])&&live->bound==2);
    for(unsigned pass=0;pass<30;++pass){clock_ms+=1000;pocket_tick(esp_timer_get_time());}
    assert(selects==asked+2);reconnect();assert(selects==asked+2);
    strcpy(cloud->binding.revision,"binding-elsewhere");reconnect();assert(selects==asked+3);
    strcpy(cloud->binding.revision,live->operation.binding_revision);
    // The bound pet and the USB pet need nothing; back on the refused pet, a swipe always asks.
    swipe_and_settle();assert(s_shown_slot==2&&selects==asked+3);
    swipe_and_settle();assert(s_shown_slot==0&&selects==asked+3);
    swipe_and_settle();assert(s_shown_slot==1&&selects==asked+4);
    // Without an answer (or with a 429 or 5xx) the question stays, unremembered,
    // with control still connected. Nothing is asked or read during the
    // worker's backoff, nor while a voice turn holds the gate after it.
    select_answer=PET_REPLACE_SELECT_UNAVAILABLE;
    swipe_and_settle();swipe_and_settle();swipe_and_settle();
    assert(s_shown_slot==1&&selects==asked+5&&s_select_moment==SELECT_AFTER_SWIPE&&strcmp(s_select_refused_build,builds[1]));
    reads=record_reads;
    for(unsigned pass=0;pass<29;++pass){clock_ms+=1000;pocket_tick(esp_timer_get_time());}
    assert(selects==asked+5&&record_reads==reads&&pet_worker.authenticated);
    atomic_store(&s_resources.owner,PET_RESOURCE_VOICE);
    for(unsigned pass=0;pass<3;++pass){clock_ms+=1000;pocket_tick(esp_timer_get_time());}
    assert(selects==asked+5&&record_reads==reads);
    // Once the gate is free it asks again, with no reconnect, and a choice binds it.
    atomic_store(&s_resources.owner,PET_RESOURCE_FREE);select_answer=PET_REPLACE_SELECT_CHOSEN;
    clock_ms+=1000;pocket_tick(esp_timer_get_time());
    assert(selects==asked+6&&record_reads==reads+1&&live->bound==1&&s_select_moment==SELECT_NONE);
    assert(!strcmp(live->operation.binding_revision,cloud->binding.revision));
    started=online_starts;assert(pocket_activate(NULL,cloud)&&online_starts==started+1&&!strcmp(online_build,builds[1]));
    atomic_store(&s_handshake_pending,false);
    // While an installation owns the binding, a settled swipe waits; it asks
    // once the installation is gone.
    pet_worker.cloud.has_operation=true;pet_worker.cloud.operation.phase=PET_CLOUD_QUEUED;
    pocket_switch(1);pocket_tick(esp_timer_get_time());
    assert(s_shown_slot==2&&!s_selection_save_at&&s_select_moment==SELECT_AFTER_SWIPE&&selects==asked+6);
    for(unsigned pass=0;pass<5;++pass){clock_ms+=1000;pocket_tick(esp_timer_get_time());}
    assert(selects==asked+6);
    pet_worker.cloud.has_operation=false;pocket_tick(esp_timer_get_time());
    assert(selects==asked+7&&live->bound==2&&s_select_moment==SELECT_NONE);
    unsigned_slots=0;

    firmware_protection_checks();
    firmware_health_checks();
    assert(s_pocket_pet.remove == pocket_remove);
    library_removal();
    free(s_shown_pack);s_shown_pack=NULL;
    puts("pocket installation: three-pet compatibility, verification in the screen's copy, activation shows and binds the new pet, only the bound pet talks, swipes and saves, selection moves the voice to the pet on screen, firmware protection of every slot, updater health");
    return 0;
}
