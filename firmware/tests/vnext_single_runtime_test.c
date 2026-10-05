/* Actual runtime adapters, shared frame validator/player and layout/receipt
 * rules. Signed-record and hardware I/O seams are mocked; their native suites
 * independently exercise real signature verification and flash checkpoints. */
#include <assert.h>
#include "../main/pet_vnext.c"
#include "mbedtls/sha256.h"

static pet_release_v2_t signed_release;
static pet_firmware_boot_state_t observed;
static uint8_t pack_bytes[200000];
static size_t pack_length;
static bool boot_ok=true,qualified=true,layout_ok=true,manifest_ok=true,map_ok=true,signature_ok=true;
static bool allocation_fail,setup_ok=true,wifi_ok=true,ui_ok=true,pack_ui_ok=true;
static bool capturing,playing,firmware_busy,queue_firmware,pet_transition;
static unsigned manifest_reads,maps,fw_steps,pet_steps,freezes,clock_ms=10000;
static unsigned reopened,aborted,store_writes,full_validations;
static bool abort_ok=true,reopen_ok=true,lock_held;
static pet_onboarding_status_t visible_status;
static pet_replace_control_t pet_worker;
static pet_firmware_control_t firmware_worker;

int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
uint32_t esp_random(void){return 7;}
void *heap_caps_aligned_alloc(size_t alignment,size_t bytes,unsigned caps)
{(void)alignment;(void)caps;if(bytes==fp_validation_workspace_size())++full_validations;return allocation_fail?NULL:malloc(bytes);}
uint32_t pet_single_store_writes(void){return store_writes;}
void heap_caps_free(void *memory){free(memory);}
int xSemaphoreTake(SemaphoreHandle_t handle,TickType_t ticks){(void)handle;(void)ticks;lock_held=true;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t handle){(void)handle;lock_held=false;return pdTRUE;}
void pet_audio_capture_stop(void){capturing=false;}
void pet_audio_playback_cancel(void){playing=false;}
void pet_network_freeze_bound(void){++freezes;}
void pet_control_http_close_kept(void){}
static pet_face_state_t face_state=PET_FACE_IDLE;
void pet_face_set_state(pet_face_state_t state){face_state=state;}
pet_face_state_t pet_face_get_state(void){return face_state;}
/* Conversation seams: the audio task's decisions run for real. */
static bool network_ready=true,input_start_ok=true,input_end_ok=true,capture_ok=true;
static unsigned input_starts,input_ends,cancels,captures,config_results,speech_results;
static uint32_t cancelled_stream;
bool pet_network_is_ready(void){return network_ready;}
esp_err_t pet_network_input_start(uint32_t stream,const char *ai_pet_id)
{assert(!lock_held&&stream&&ai_pet_id);++input_starts;return input_start_ok?ESP_OK:ESP_FAIL;}
esp_err_t pet_network_input_end(uint32_t stream,uint32_t sequence,uint32_t duration)
{assert(!lock_held&&stream);(void)sequence;(void)duration;++input_ends;return input_end_ok?ESP_OK:ESP_FAIL;}
esp_err_t pet_network_cancel(uint32_t stream){assert(!lock_held);++cancels;cancelled_stream=stream;return ESP_OK;}
esp_err_t pet_network_config_v2_result(const pet_synced_config_v2_t *settings,bool applied,const char *error,const char *field)
{assert(!lock_held&&settings);(void)applied;(void)error;(void)field;++config_results;return ESP_OK;}
esp_err_t pet_network_speech_profile_result(const pet_speech_preference_t *preference,bool applied)
{assert(!lock_held&&preference);(void)applied;++speech_results;return ESP_OK;}
/* Managed Brain: the scheduler asks the cloud for the admitted pet's grant,
 * which voice needs, and opens the session with it. Management has nothing to send. */
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
    (void)token;
}
esp_err_t pet_network_start_bound(const pet_config_t *config, const pet_network_callbacks_t *callbacks,
                                  const char *device, const pet_control_context_t *context)
{
    (void)config;
    (void)callbacks;
    (void)device;
    (void)context;
    return ESP_OK;
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
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}
void pet_enrollment_clear(void *data, size_t bytes)
{
    memset(data, 0, bytes);
}
/* Reached only through the session's callbacks, which no socket calls here. */
bool pet_ota_busy(void)
{
    return false;
}
esp_err_t pet_ota_confirm_health_stage(pet_ota_health_stage_t stage, const pet_ota_health_evidence_t *evidence)
{
    (void)stage;
    (void)evidence;
    return ESP_OK;
}
bool pet_expression_from_wire(const char *name, pet_expression_t *value)
{
    (void)name;
    (void)value;
    return false;
}
void pet_face_set_expression(pet_expression_t expression)
{
    (void)expression;
}
/* Mouth timing: applied at once, kept in NVS outside the audio lock. */
static int16_t audio_mouth_offset=12345;
static unsigned mouth_offset_stores;
static bool stored_mouth_present;
static int16_t stored_mouth_offset;
void pet_audio_set_speech_mouth_offset(int16_t offset_ms){audio_mouth_offset=offset_ms;}
esp_err_t pet_config_store_speech_mouth_offset(bool present,int16_t offset_ms)
{assert(!lock_held);++mouth_offset_stores;stored_mouth_present=present;stored_mouth_offset=offset_ms;return ESP_OK;}
esp_err_t pet_audio_capture_start(uint32_t stream)
{(void)stream;if(!capture_ok)return ESP_ERR_INVALID_STATE;capturing=true;++captures;return ESP_OK;}
void pet_diagnostics_note_app_event(int32_t event,const char *name,uint32_t depth){(void)event;(void)name;(void)depth;}
void pet_diagnostics_note_streams(uint32_t input,uint32_t output){(void)input;(void)output;}
uint8_t pet_face_trigger_gesture(uint8_t gesture){return gesture;}
static unsigned retries_queued,dropped_events;
int xQueueSend(QueueHandle_t queue,const void *item,TickType_t ticks)
{(void)ticks;assert(queue==s_audio_events);if(((const audio_event_t *)item)->kind==AUDIO_TAP_RETRY)++retries_queued;return pdTRUE;}
void pet_diagnostics_note_dropped_app_event(void){++dropped_events;}
static unsigned enqueued,finished;
/* The speaker: pet_audio.c's queue of ten 40 ms frames that never waits,
 * drained in real time while the network task waits (vTaskDelay). */
#define SPEAKER_SLOTS 10u
static unsigned speaker_queued,speaker_waited_ms;
static bool speaker_stuck;
esp_err_t pet_audio_playback_start(uint32_t stream,uint32_t rate,pet_realtime_boost_t boost,pet_speech_mouth_mode_t mode)
{(void)stream;(void)rate;(void)boost;(void)mode;playing=true;speaker_queued=0;return ESP_OK;}
esp_err_t pet_audio_playback_enqueue(uint32_t stream,uint32_t sequence,const uint8_t *pcm,size_t length)
{
    (void)stream;(void)sequence;(void)pcm;(void)length;
    if(!playing)return ESP_ERR_INVALID_STATE;
    if(speaker_queued>=SPEAKER_SLOTS)return ESP_ERR_NO_MEM;
    ++speaker_queued;++enqueued;return ESP_OK;
}
bool pet_audio_playback_queue_healthy(void){return !playing||SPEAKER_SLOTS-speaker_queued>1u;}
void vTaskDelay(TickType_t ticks)
{
    static unsigned elapsed;speaker_waited_ms+=ticks;elapsed+=ticks; /* One tick is 1 ms here. */
    for(;elapsed>=40;elapsed-=40)if(!speaker_stuck&&speaker_queued)--speaker_queued;
}
void pet_audio_playback_finish(uint32_t stream){(void)stream;++finished;}
bool pet_audio_is_capturing(void){return capturing;}
bool pet_audio_is_playing(void){return playing;}
bool pet_setup_control_ready(void){return setup_ok;}
bool pet_network_wifi_is_ready(void){return wifi_ok;}
bool pet_onboarding_setup_ready(void){return setup_ok;}
bool pet_onboarding_ui_ready(void){return ui_ok;}
bool pet_face_pack_ready(void){return pack_ui_ok;}
const esp_app_desc_t *esp_app_get_description(void)
{static const esp_app_desc_t app={.version="host-fixture"};return &app;}
void pet_onboarding_update_status(const pet_onboarding_status_t *status){visible_status=*status;}
bool pet_firmware_image_bootloader_matches(const pet_firmware_bootloader_t *p)
{assert(p==&s_boot_profile);return qualified;}
bool pet_firmware_image_boot_state(pet_firmware_boot_state_t *out)
{if(!boot_ok)return false;*out=observed;return true;}
bool pet_firmware_image_abort(pet_firmware_image_t *image)
{++aborted;if(!abort_ok)return false;image->active=false;return true;}
esp_err_t pet_display_lock(int timeout){(void)timeout;return ESP_OK;}
void pet_display_unlock(void){}
void pet_face_pack_release_external(void){}
void pet_onboarding_open_from_ui(void){}
bool pet_single_store_close(pet_single_store_t *store)
{assert(store==&s_single&&!s_firmware->image.active&&!s_firmware->writer_fault);return true;}
esp_err_t pet_single_store_open(pet_single_store_t *store,const pet_single_readers_t *readers)
{
    assert(store==&s_single&&readers->detach==single_readers_detached&&!s_firmware->image.active&&!s_firmware->writer_fault);
    ++reopened;store->state.ready=reopen_ok;return reopen_ok?ESP_OK:ESP_FAIL;
}
esp_err_t pet_flash_layout_read(pet_flash_layout_t *layout,char hash[65])
{if(!layout_ok)return ESP_FAIL;*layout=observed.layout;strcpy(hash,observed.partition_sha256);return ESP_OK;}
esp_err_t pet_single_store_map(pet_single_store_t *store,const void **bytes,size_t *length)
{assert(store==&s_single);++maps;if(!map_ok)return ESP_FAIL;*bytes=pack_bytes;*length=pack_length;return ESP_OK;}
esp_err_t pet_single_store_read_manifest(pet_single_store_t *store,void *record,size_t bytes)
{
    assert(store==&s_single&&bytes==PET_REPLACE_MANIFEST_BYTES);++manifest_reads;
    if(!manifest_ok)return ESP_FAIL;
    memset(record,0x5a,bytes);return ESP_OK;
}
bool pet_release_v2_record_verify(const void *record,size_t bytes,const char *account,const pet_replace_pack_t *expected,
                                  const pet_pack_trust_key_t *keys,size_t count,pet_release_v2_t *out)
{
    assert(record&&bytes==PET_REPLACE_MANIFEST_BYTES&&keys==&s_trust&&count==s_trust_count);
    if(!signature_ok||strcmp(account,signed_release.account_id)||expected->bytes!=signed_release.pack.bytes||
       strcmp(expected->build_id,signed_release.pack.build_id)||strcmp(expected->sha256,signed_release.pack.sha256))return false;
    *out=signed_release;return true;
}
fp_error_t pet_face_pack_validate(const void *bytes,uint32_t length,fp_pack_info_t *info)
{
    ++full_validations;
    uint32_t size=fp_validation_workspace_size();void *workspace=malloc(size);assert(workspace);
    fp_error_t result=fp_validate_with_workspace(bytes,length,info,workspace,size);free(workspace);return result;
}
bool pet_firmware_control_blocks_pet(const pet_firmware_control_t *worker)
{return !worker||firmware_busy;}
bool pet_firmware_control_health_deadline(pet_firmware_control_t *worker,uint64_t now)
{assert(worker==s_firmware&&now==clock_ms);return false;}
pet_firmware_control_status_t pet_firmware_control_step(pet_firmware_control_t *worker,uint64_t now)
{
    assert(worker==s_firmware&&now==clock_ms&&pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));
    ++fw_steps;worker->authenticated=true;
    if(queue_firmware)firmware_busy=true;
    return PET_FW_CONTROL_WORKING;
}
void pet_replace_control_disconnect(pet_replace_control_t *worker)
{
    if(!worker)return;
    if(worker->admitted)freeze(NULL);
    worker->admitted=worker->authenticated=false;worker->next_poll_ms=0;
}
pet_replace_control_status_t pet_replace_control_step(pet_replace_control_t *worker,uint64_t now,bool allow)
{
    assert(worker==s_pet&&now==clock_ms&&pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));
    assert(allow==!firmware_busy&&s_fw_reserved==firmware_busy);++pet_steps;
    if(!allow)pet_replace_control_disconnect(worker);
    worker->authenticated=worker->has_context=true;
    if(allow&&pet_transition)s_single.state.state.phase=PET_REPLACE_INVALIDATED;
    return PET_REPLACE_CONTROL_WORKING;
}
static void initialize(void)
{
    s_pet=&pet_worker;s_firmware=&firmware_worker;s_fw_lifecycle=true;s_identity_ready=true;
    firmware_worker.authenticated=true;firmware_worker.next_poll_ms=clock_ms+15000;
    strcpy(s_id,"00000000-0000-4000-8000-000000000001");
    s_origin="https://fixture.invalid";
    s_single.initialized=s_single.layout_valid=s_single.state.ready=s_single.state.journal.loaded=true;
    s_single.state.journal.generation=2;
    assert(pet_flash_layout_known(PET_LAYOUT_SINGLE_2M,&s_single.layout));
    observed.layout=s_single.layout;
    memset(observed.partition_sha256,'c',64);strcpy(s_single.partition_sha256,observed.partition_sha256);
    s_boot_profile.bytes=4096;memset(s_boot_profile.sha256,'d',64);
    observed.running_slot=0;observed.selection.active=0;
    observed.selection.records[0]=(pet_ota_record_t){.sequence=3,.state=PET_OTA_STATE_VALID,.slot=0,.crc_valid=true,.bootable=true};
    assert(pet_replace_empty(&s_single.state.state,s_single.layout.pack_capacity_bytes));
    signed_release.pack.bytes=(uint32_t)pack_length;
    strcpy(signed_release.pack.build_id,"00000000-0000-4000-8000-000000000002");
    uint8_t digest[32];assert(!mbedtls_sha256(pack_bytes,pack_length,digest,0));
    for(size_t i=0;i<32;++i)snprintf(signed_release.pack.sha256+2*i,3,"%02x",digest[i]);
    strcpy(signed_release.account_id,"00000000-0000-4000-8000-000000000003");
    strcpy(signed_release.project_id,"00000000-0000-4000-8000-000000000004");
    strcpy(signed_release.face_id,"big-sal");strcpy(signed_release.version,"fixture-240-v2");
    signed_release.requirements=(pet_firmware_protected_pack_t){.present=true,.layout_id=PET_LAYOUT_SINGLE_2M,
        .minimum_firmware_epoch=3,.bytes=(uint32_t)pack_length,.format=2,.resolution_divisor=2,.codecs=32};
    strcpy(signed_release.requirements.partition_sha256,observed.partition_sha256);
    pet_worker.has_context=pet_worker.authenticated=true;
    strcpy(pet_worker.cloud.context.account_id,signed_release.account_id);
    strcpy(pet_worker.cloud.context.device_id,s_id);
}
static void make_active(void)
{
    pet_replace_t *s=&s_single.state.state;
    assert(pet_replace_request(s,"00000000-0000-4000-8000-000000000005",&signed_release.pack,"binding-before"));
    assert(pet_replace_fenced(s,"00000000-0000-4000-8000-000000000006","00000000-0000-4000-8000-000000000007",&signed_release.pack));
    assert(pet_replace_invalidate(s)&&pet_replace_begin_download(s));
    assert(pet_replace_progress(s,signed_release.pack.bytes,signed_release.pack.sha256));
    assert(pet_replace_verified(s,signed_release.pack.sha256)&&pet_replace_activate(s));
    assert(pet_replace_commit(s,s->operation_id,s->fence_id,&signed_release.pack,"binding-after",
        "00000000-0000-4000-8000-000000000008","11"));
    pet_control_context_t *c=&pet_worker.cloud.context;
    c->binding.assigned=true;strcpy(c->binding.build_id,s->active.build_id);strcpy(c->binding.sha256,s->active.sha256);
    strcpy(c->binding.revision,s->binding_revision);strcpy(c->binding.relationship_id,s->relationship_id);
    strcpy(c->config.face_id,"big-sal");strcpy(c->config.version,"12"); /* Newer settings do not rewrite activation evidence. */
}
int main(void)
{
    pack_length=fread(pack_bytes,1,sizeof(pack_bytes),stdin);assert(pack_length&&pack_length<sizeof(pack_bytes));
    initialize();assert(!s_single_boot_healthy);
    // A first USB boot can have a VALID selection without an OTA receipt.
    // Actual on-boot service/UI and fresh independent control evidence still
    // precede any pack access; no confirmation or receipt is fabricated.
    s_firmware=NULL;assert(!single_compatible(NULL,&signed_release));s_firmware=&firmware_worker;
    firmware_worker.authenticated=false;assert(!single_compatible(NULL,&signed_release));firmware_worker.authenticated=true;
    firmware_worker.next_poll_ms=0;assert(!single_compatible(NULL,&signed_release));
    firmware_worker.next_poll_ms=clock_ms;assert(!single_compatible(NULL,&signed_release));
    firmware_worker.next_poll_ms=clock_ms+15000;
    setup_ok=false;assert(!single_compatible(NULL,&signed_release));setup_ok=true;
    wifi_ok=false;assert(!single_compatible(NULL,&signed_release));wifi_ok=true;
    ui_ok=false;assert(!single_compatible(NULL,&signed_release));ui_ok=true;
    s_identity_ready=false;assert(!single_compatible(NULL,&signed_release));s_identity_ready=true;
    assert(!maps&&!s_single_boot_healthy&&single_compatible(NULL,&signed_release)&&s_single_boot_healthy);
    qualified=false;assert(!single_compatible(NULL,&signed_release));qualified=true;
    boot_ok=false;assert(!single_compatible(NULL,&signed_release));boot_ok=true;
    observed.selection.records[0].state=PET_OTA_STATE_PENDING;assert(!single_compatible(NULL,&signed_release));
    observed.selection.records[0].state=PET_OTA_STATE_VALID;
    observed.running_slot=1;assert(!single_compatible(NULL,&signed_release));observed.running_slot=0;
    observed.partition_sha256[0]='e';assert(!single_compatible(NULL,&signed_release));observed.partition_sha256[0]='c';
    signed_release.requirements.minimum_firmware_epoch=4;assert(!single_compatible(NULL,&signed_release));
    signed_release.requirements.minimum_firmware_epoch=3;
    // This firmware verifies imported releases too.
    signed_release.requirements.imported_release=true;assert(single_compatible(NULL,&signed_release));
    signed_release.requirements.imported_release=false;
    s_single.state.journal.generation=0;assert(!single_compatible(NULL,&signed_release));s_single.state.journal.generation=2;
    assert(single_verify(NULL,&signed_release));map_ok=false;assert(!single_verify(NULL,&signed_release));map_ok=true;
    // A proof lasts until the pet partition is written; then the bytes are
    // validated again, in full.
    unsigned before_validations=full_validations;
    assert(single_verify(NULL,&signed_release)&&full_validations==before_validations);
    ++store_writes;allocation_fail=true;assert(!single_verify(NULL,&signed_release));allocation_fail=false;
    ++store_writes;pack_bytes[pack_length-1]^=1;assert(!single_verify(NULL,&signed_release));pack_bytes[pack_length-1]^=1;
    ++pack_length;assert(!single_verify(NULL,&signed_release));--pack_length;
    // Release metadata listing another codec never rejects the exact bytes
    // its SHA-256 pins; the validator decides which codecs play.
    signed_release.requirements.codecs|=1;assert(single_verify(NULL,&signed_release));signed_release.requirements.codecs=32;
    pet_firmware_protection_t protection;
    memset(&protection,0xff,sizeof(protection)); /* Single-pet devices list no installed pets. */
    firmware_protection(NULL,false,&protection);assert(protection.known&&!protection.active.requirements.present&&!protection.installed_count);
    make_active();assert(single_prepare_active(&pet_worker.cloud.context));
    // Reactivating the same pet reuses this boot's proof: nothing is
    // inflated again, and binding for display only inspects the pack.
    before_validations=full_validations;
    assert(single_prepare_active(&pet_worker.cloud.context)&&full_validations==before_validations);
    ++store_writes;assert(single_prepare_active(&pet_worker.cloud.context)&&full_validations==before_validations+1);
    assert(s_prepared[0].bytes==pack_bytes&&s_prepared[0].length==pack_length&&!strcmp(s_prepared[0].manifest.face_id,"big-sal"));
    assert(!strcmp(s_single.state.state.config_version,"11"));
    pet_control_context_t wrong=pet_worker.cloud.context;
    wrong.binding.sha256[0]^=1;assert(!single_prepare_active(&wrong));
    wrong=pet_worker.cloud.context;strcpy(wrong.config.face_id,"different");assert(!single_prepare_active(&wrong));
    wrong=pet_worker.cloud.context;wrong.account_id[0]^=1;assert(!single_prepare_active(&wrong));
    signature_ok=false;assert(!single_prepare_active(&pet_worker.cloud.context));
    firmware_protection(NULL,false,&protection);assert(!protection.known);signature_ok=true;
    firmware_protection(NULL,false,&protection);
    assert(protection.known&&protection.active.requirements.present&&!protection.interrupted.requirements.present&&!protection.installed_count);
    layout_ok=false;firmware_protection(NULL,false,&protection);assert(!protection.known);layout_ok=true;
    observed.partition_sha256[0]='e';firmware_protection(NULL,false,&protection);assert(!protection.known);observed.partition_sha256[0]='c';
    manifest_ok=false;firmware_protection(NULL,false,&protection);assert(!protection.known);manifest_ok=true;
    // A new queued operation observed by poll revokes admission even before
    // ACTIVE becomes REQUESTED. The already-proven boot can still progress.
    ui_ok=false;pet_worker.admitted=false;
    assert(single_compatible(NULL,&signed_release));ui_ok=true;
    assert(pet_replace_request(&s_single.state.state,"00000000-0000-4000-8000-000000000005",&signed_release.pack,"binding-after"));
    // Revoking the old pet's admission and hiding setup during a replacement
    // must not deadlock the already healthy boot before durable detachment.
    ui_ok=false;pet_worker.admitted=false;
    assert(single_compatible(NULL,&signed_release));
    firmware_worker.authenticated=false;assert(!single_compatible(NULL,&signed_release));firmware_worker.authenticated=true;
    // A power cycle does not carry that proof into an interrupted replacement.
    s_single_boot_healthy=false;assert(!single_compatible(NULL,&signed_release));
    ui_ok=true;assert(single_compatible(NULL,&signed_release)&&s_single_boot_healthy);
    firmware_protection(NULL,false,&protection);assert(!protection.known);
    assert(pet_replace_fenced(&s_single.state.state,"00000000-0000-4000-8000-000000000006",
        "00000000-0000-4000-8000-000000000007",&signed_release.pack));
    assert(pet_replace_invalidate(&s_single.state.state));
    firmware_protection(NULL,false,&protection);
    assert(protection.known&&!protection.active.requirements.present&&protection.interrupted.requirements.present&&!protection.installed_count);
    assert(pet_replace_cancel(&s_single.state.state));
    firmware_protection(NULL,false,&protection);assert(protection.known&&protection.interrupted.requirements.present);
    assert(firmware_health(NULL)==PET_FW_STORAGE_RECOVERY);
    s_pet=NULL;assert(firmware_health(NULL)==PET_FW_STORAGE_NONE);s_pet=&pet_worker;
    ui_ok=false;assert(firmware_health(NULL)==PET_FW_STORAGE_NONE);ui_ok=true;
    s_single.state.ready=false;assert(firmware_health(NULL)==PET_FW_STORAGE_RECOVERY);
    firmware_protection(NULL,false,&protection);assert(!protection.known);s_single.state.ready=true;
    publish_status(0);assert(visible_status.single_pet_slot&&!visible_status.has_pet&&visible_status.installation==PET_ONBOARDING_INSTALL_FAILED);
    assert(pet_replace_empty(&s_single.state.state,s_single.layout.pack_capacity_bytes));make_active();
    s_active_slot=0;s_audio_started=true;pet_worker.admitted=true;ui_ok=false;
    assert(firmware_health(NULL)==PET_FW_STORAGE_PACK);pack_ui_ok=false;assert(firmware_health(NULL)==PET_FW_STORAGE_NONE);
    pack_ui_ok=ui_ok=true;publish_status(0);assert(visible_status.has_pet&&visible_status.installation_percent==100);

    // Real scheduler: independent endpoints on one reservation, never a pet
    // write after firmware becomes busy; no dependence on a live conversation.
    independent_updates(clock_ms);assert(fw_steps==1&&pet_steps==1&&!s_fw_reserved);
    assert(!pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));
    queue_firmware=true;independent_updates(clock_ms);
    assert(fw_steps==2&&pet_steps==2&&s_fw_reserved&&!pet_worker.admitted);
    assert(pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));
    queue_firmware=firmware_busy=false;independent_updates(clock_ms);
    assert(!pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL)&&!s_fw_reserved);
    assert(pet_runtime_claim(&s_resources,PET_RESOURCE_VOICE));
    unsigned before=fw_steps;independent_updates(clock_ms);assert(fw_steps==before);
    pet_runtime_release(&s_resources,PET_RESOURCE_VOICE);
    s_single.state.ready=false;independent_updates(clock_ms);assert(fw_steps==before+1&&pet_steps==fw_steps);
    s_single.state.ready=true;firmware_worker.next_poll_ms=clock_ms+15000;pet_transition=true;
    independent_updates(clock_ms);assert(!firmware_worker.next_poll_ms);pet_transition=false;
    wifi_ok=false;pet_worker.admitted=true;capturing=playing=true;before=fw_steps;
    independent_updates(clock_ms);assert(fw_steps==before&&!pet_worker.admitted&&!s_control_healthy&&!capturing&&!playing);
    wifi_ok=true;pet_worker.admitted=true;s_handshake_pending=true;s_handshake_deadline=(int64_t)clock_ms*1000;
    independent_updates(clock_ms);assert(!s_handshake_pending&&!pet_worker.admitted);
    // A saved DOWNLOADING firmware operation cannot deadlock a transient pet
    // journal read failure. Reopen retains its authority and revokes pet use.
    firmware_busy=true;firmware_worker.has_operation=true;
    s_single.state.ready=false;assert(pet_runtime_claim(&s_resources,PET_RESOURCE_CONTROL));
    assert(single_reopen()&&s_single.state.ready&&firmware_busy&&firmware_worker.has_operation&&!pet_worker.admitted);
    assert(!aborted&&reopened==1);
    s_single.state.ready=false;firmware_worker.image.active=true;abort_ok=false;
    assert(!single_reopen()&&firmware_worker.writer_fault&&reopened==1);
    abort_ok=true;assert(single_reopen()&&aborted==2&&!firmware_worker.writer_fault&&!firmware_worker.image.active);
    reopen_ok=false;s_single.state.ready=false;assert(!single_reopen()&&!s_single.state.ready);
    pet_runtime_release(&s_resources,PET_RESOURCE_CONTROL);assert(!single_reopen());
    s_firmware=NULL;firmware_busy=true;before=pet_steps;independent_updates(clock_ms);assert(pet_steps==before+1&&s_fw_reserved);
    assert(manifest_reads&&maps&&freezes);

    // Responsive voice. The listening face comes first; gateway sends happen
    // outside the audio lock; thinking ignores taps; a tap during a control
    // step waits for it; a stale or failed start never opens the microphone.
    s_firmware=&firmware_worker;firmware_busy=false;
    pet_runtime_release(&s_resources,PET_RESOURCE_CONTROL);pet_runtime_release(&s_resources,PET_RESOURCE_VOICE);
    s_voice_allowed=true;s_audio_started=true;s_face_started=true;capturing=playing=false;face_state=PET_FACE_IDLE;
    strcpy(s_applied.config.ai_pet_id,"lavender-dragon");
    const audio_event_t tap={.kind=AUDIO_TAP},retry={.kind=AUDIO_TAP_RETRY};
    audio_send_t send={0};
    handle_audio(&tap,&send);
    assert(face_state==PET_FACE_LISTENING&&s_turn_active&&send.kind==SEND_INPUT_START&&!input_starts&&!capturing);
    finish_send(&send,&tap);assert(input_starts==1&&captures==1&&capturing&&!lock_held);
    send=(audio_send_t){0};handle_audio(&tap,&send);assert(!capturing&&send.kind==SEND_NONE);
    const audio_event_t done={.kind=AUDIO_CAPTURE_DONE,.stream=s_input_stream,.sequence=3,.duration=500,.result=ESP_OK};
    handle_audio(&done,&send);assert(face_state==PET_FACE_THINKING&&send.kind==SEND_INPUT_END&&!input_ends);
    finish_send(&send,&done);assert(input_ends==1&&s_turn_active);
    send=(audio_send_t){0};handle_audio(&tap,&send);
    assert(face_state==PET_FACE_THINKING&&s_turn_active&&send.kind==SEND_NONE);
    // A tap stops the answer. Its frames still in flight and its end are
    // dropped quietly; only a foreign stream forces revalidation.
    uint32_t stream=s_input_stream;unsigned before_cancels=cancels;uint8_t pcm[4]={0};s_revalidate=false;
    assert(audio_start(77,24000)==ESP_OK&&face_state==PET_FACE_SPEAKING&&playing);
    assert(audio_chunk(77,0,pcm,sizeof pcm)==ESP_OK&&enqueued==1);
    send=(audio_send_t){0};handle_audio(&tap,&send);
    assert(!playing&&!s_turn_active&&face_state==PET_FACE_IDLE&&send.kind==SEND_CANCEL&&cancels==before_cancels);
    finish_send(&send,&tap);assert(cancels==before_cancels+1&&cancelled_stream==stream);
    assert(!pet_runtime_owns(&s_resources,PET_RESOURCE_VOICE));
    assert(audio_chunk(77,1,pcm,sizeof pcm)==ESP_OK&&enqueued==1);audio_end(77,1);assert(!s_revalidate&&!finished);
    assert(audio_chunk(78,0,pcm,sizeof pcm)!=ESP_OK&&s_revalidate);s_revalidate=false;
    // After a network stall a burst of speech (more frames than the speaker's
    // ten-frame queue) waits for room: nothing is lost, the end marker matches
    // and the session stays.
    unsigned burst_enqueued=enqueued,burst_finished=finished;speaker_waited_ms=0;
    assert(audio_start(80,24000)==ESP_OK&&playing);
    for(uint32_t sequence=0;sequence<24;++sequence)assert(audio_chunk(80,sequence,pcm,sizeof pcm)==ESP_OK&&!s_revalidate);
    assert(enqueued==burst_enqueued+24&&speaker_waited_ms>=14*40&&speaker_queued<SPEAKER_SLOTS);
    audio_end(80,23);assert(!s_revalidate&&finished==burst_finished+1);
    // A speaker that stops taking frames skips them after the wait, counted,
    // and the reply still ends: the conversation is never dropped for it.
    speaker_stuck=true;burst_enqueued=enqueued;speaker_waited_ms=0;
    assert(audio_start(81,24000)==ESP_OK);
    for(uint32_t sequence=0;sequence<13;++sequence)assert(audio_chunk(81,sequence,pcm,sizeof pcm)==ESP_OK&&!s_revalidate);
    assert(enqueued==burst_enqueued+SPEAKER_SLOTS&&speaker_waited_ms==4*SPEAKER_ROOM_WAIT_MS);
    audio_end(81,12);assert(!s_revalidate&&finished==burst_finished+2);
    speaker_stuck=false;playing=false;
    // An answer the device did not ask for is cancelled too.
    assert(audio_start(79,24000)==ESP_OK&&!s_input_stream);
    send=(audio_send_t){0};handle_audio(&tap,&send);assert(send.kind==SEND_CANCEL&&!send.stream&&!playing);
    finish_send(&send,&tap);assert(cancels==before_cancels+2&&!cancelled_stream);
    // A tap during a control step (a cloud poll of a few seconds) waits for
    // it, with the microphone closed and the face not LISTENING, then listens
    // when control releases: 5 s later too, where it used to give up at 2 s.
    assert(pet_runtime_claim(&s_resources,PET_RESOURCE_CONTROL));
    send=(audio_send_t){0};handle_audio(&tap,&send);
    assert(s_tap_pending&&s_tap_for_control&&face_state==PET_FACE_IDLE&&send.kind==SEND_NONE);
    handle_audio(&retry,&send);assert(s_tap_pending&&send.kind==SEND_NONE);
    const audio_event_t idle={.kind=AUDIO_STATE};
    handle_audio(&idle,&send);assert(s_tap_pending&&face_state==PET_FACE_IDLE);
    clock_ms+=5000;handle_audio(&retry,&send);assert(s_tap_pending&&send.kind==SEND_NONE&&!capturing);
    // Control yields after its current call: the pet worker's step waits for the listen.
    s_audio_events=(QueueHandle_t)&retries_queued;unsigned before_drops=dropped_events,before_pet=pet_steps;
    independent_updates(clock_ms);
    assert(retries_queued==1&&dropped_events==before_drops&&!pet_runtime_owns(&s_resources,PET_RESOURCE_CONTROL));
    assert(pet_steps==before_pet);
    handle_audio(&retry,&send);assert(!s_tap_pending&&!s_tap_for_control&&send.kind==SEND_INPUT_START);
    assert(face_state==PET_FACE_LISTENING);
    finish_send(&send,&retry);assert(capturing&&input_starts==2);
    pet_audio_capture_stop();abandon_turn();
    // ...but not forever (10 s), and a second tap withdraws it.
    assert(pet_runtime_claim(&s_resources,PET_RESOURCE_CONTROL));
    send=(audio_send_t){0};handle_audio(&tap,&send);assert(s_tap_pending);
    clock_ms+=9900;handle_audio(&retry,&send);assert(s_tap_pending);
    clock_ms+=200;handle_audio(&retry,&send);assert(!s_tap_pending&&face_state==PET_FACE_IDLE&&send.kind==SEND_NONE);
    handle_audio(&tap,&send);assert(s_tap_pending);handle_audio(&tap,&send);assert(!s_tap_pending&&face_state==PET_FACE_IDLE);
    pet_runtime_release(&s_resources,PET_RESOURCE_CONTROL);
    // A failed announcement ends the turn, shows the error face and revalidates the session.
    input_start_ok=false;s_revalidate=false;send=(audio_send_t){0};
    handle_audio(&tap,&send);finish_send(&send,&tap);
    assert(!capturing&&!s_turn_active&&face_state==PET_FACE_ERROR&&s_revalidate&&!pet_runtime_owns(&s_resources,PET_RESOURCE_VOICE));
    input_start_ok=true;
    // Speech that started during the announcement wins: no microphone.
    capture_ok=false;before_cancels=cancels;send=(audio_send_t){0};
    handle_audio(&tap,&send);finish_send(&send,&tap);
    assert(!capturing&&!s_turn_active&&cancels==before_cancels+1);capture_ok=true;
    // A freeze between decision and send never opens the microphone.
    send=(audio_send_t){0};handle_audio(&tap,&send);atomic_fetch_add(&s_generation,1);
    unsigned before_captures=captures;finish_send(&send,&tap);assert(captures==before_captures&&!capturing);
    abandon_turn();
    // Configuration and speech results are sent outside the lock.
    audio_event_t config={.kind=AUDIO_CONFIG,.generation=atomic_load(&s_generation)};send=(audio_send_t){0};
    handle_audio(&config,&send);assert(send.kind==SEND_CONFIG);finish_send(&send,&config);assert(config_results==1);
    // Mouth timing arrives only on the socket. It applies once the rest matches
    // the applied context, and NVS keeps it, written only when it changes.
    const pet_control_config_t applied=s_applied.config;
    memset(&s_applied.config,0,sizeof(s_applied.config));strcpy(s_applied.config.version,"9");
    strcpy(s_applied.config.face_id,"glowberry");strcpy(s_applied.config.ai_pet_id,"relationship-fixture");
    audio_event_t timed={.kind=AUDIO_CONFIG,.generation=atomic_load(&s_generation),
        .config={.version=9,.has_speech_mouth_offset=true,.speech_mouth_offset_ms=-20}};
    strcpy(timed.config.face_id,"glowberry");strcpy(timed.config.ai_pet_id,"relationship-fixture");
    unsigned stores=mouth_offset_stores;send=(audio_send_t){0};s_revalidate=false;
    handle_audio(&timed,&send);assert(send.kind==SEND_CONFIG&&send.same&&audio_mouth_offset==-20);
    finish_send(&send,&timed);
    assert(mouth_offset_stores==stores+1&&stored_mouth_present&&stored_mouth_offset==-20&&config_results==2);
    send=(audio_send_t){0};handle_audio(&timed,&send);finish_send(&send,&timed);
    assert(mouth_offset_stores==stores+1&&config_results==3); /* Unchanged: no second write. */
    audio_event_t other=timed;other.config.speech_mouth_offset_ms=40;other.config.volume=1;send=(audio_send_t){0};
    handle_audio(&other,&send);assert(!send.same&&audio_mouth_offset==-20&&s_revalidate);
    finish_send(&send,&other);assert(mouth_offset_stores==stores+1&&config_results==4);
    audio_event_t cleared=timed;cleared.config.has_speech_mouth_offset=false;cleared.config.speech_mouth_offset_ms=0;
    send=(audio_send_t){0};handle_audio(&cleared,&send);finish_send(&send,&cleared);
    assert(audio_mouth_offset==0&&mouth_offset_stores==stores+2&&!stored_mouth_present);
    s_applied.config=applied;s_revalidate=false;
    audio_event_t speech={.kind=AUDIO_SPEECH};send=(audio_send_t){0};
    handle_audio(&speech,&send);assert(send.kind==SEND_SPEECH);finish_send(&send,&speech);assert(speech_results==1);
    puts("single runtime: qualified compatibility, full pack validation, persisted metadata, protected recovery, health, serialized scheduling and responsive voice passed");
}
