/* Orchestration unit fixtures. Signature/wire, transport and actual flash
 * boundaries are independently exercised by their real-adapter test suites. */
#include "pet_firmware_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static pet_firmware_control_t worker;
static pet_firmware_control_config_t config;
static pet_firmware_receipt_store_t store;
static pet_firmware_operation_t operation,reply;
static pet_firmware_receipt_t request_report;
static pet_firmware_boot_state_t boot;
static uint8_t records[2][PET_FIRMWARE_RECEIPT_BYTES];static bool present[2];
static bool has_operation,verified,transport_ok,known_layout,known_packs,bootloader_ok,write_ok,confirm_ok;
static bool persist_fail,persist_ambiguous,lose_report_reply,conflict_reply,minimal_poll,pack_unknown_poll;
static unsigned polls,reports,ranges,begins,writes,finishes,selections,confirms,restarts,freezes,polled_installed;
static pet_firmware_protection_t pets; /* Installed pets beyond an active/interrupted pack (three-pet layouts). */
static pet_firmware_protection_t fresh_pets; /* What flash proves now, when it differs from the last proof. */
static bool fresh_differs;static unsigned fresh_proofs;
static pet_fw_storage_t health;
static char running[65];static uint64_t now;
static uint32_t transport_delay_ms;static bool abort_ok;
static unsigned downloads_completed,downloads_stopped,download_writes;
static const char *initial_boot="00000000-0000-4000-8000-000000000003",*next_boot="00000000-0000-4000-8000-000000000004";

static int read_record(void *p,unsigned slot,uint8_t out[PET_FIRMWARE_RECEIPT_BYTES])
{(void)p;if(!present[slot])return 0;memcpy(out,records[slot],sizeof(records[slot]));return 1;}
static bool write_record(void *p,unsigned slot,const uint8_t in[PET_FIRMWARE_RECEIPT_BYTES])
{(void)p;if(!persist_fail||persist_ambiguous){memcpy(records[slot],in,sizeof(records[slot]));present[slot]=true;}return !persist_fail;}
static const pet_firmware_receipt_io_t io={read_record,write_record,NULL};
static void protection(void *p,bool fresh,pet_firmware_protection_t *out)
{(void)p;fresh_proofs+=fresh;*out=fresh&&fresh_differs?fresh_pets:pets;out->known=known_packs;}
static pet_fw_storage_t health_check(void *p){(void)p;return health;}
static void freeze(void *p){(void)p;++freezes;}
static void restart(void *p){(void)p;++restarts;}
static uint64_t clock_ms(void *p){(void)p;return now;}
bool pet_firmware_image_bootloader_matches(const pet_firmware_bootloader_t *p){return bootloader_ok&&p&&p->bytes==0x7000;}
bool pet_firmware_image_boot_state(pet_firmware_boot_state_t *out){if(!known_layout)return false;*out=boot;return true;}
bool pet_firmware_image_running(char out[65],uint32_t *bytes){strcpy(out,running);*bytes=operation.release.bytes;return known_layout;}
/* Stands in for the real rule (firmware_release_test.c) on one field: every
 * installed pet's minimum epoch. */
bool pet_firmware_release_compatible(const pet_firmware_release_t *r,const pet_flash_layout_t *layout,const char *sha,
    const pet_firmware_bootloader_t *profile,const pet_firmware_protection_t *p)
{
    if(!p||!p->known||p->active.requirements.present||p->interrupted.requirements.present||p->installed_count>PET_FIRMWARE_INSTALLED_MAX)return false;
    for(unsigned i=0;i<p->installed_count;++i)
        if(!p->installed[i].requirements.present||p->installed[i].requirements.minimum_firmware_epoch>r->requirements.firmware_epoch)return false;
    return profile&&profile->bytes==r->requirements.bootloader.bytes&&
 !strcmp(profile->sha256,r->requirements.bootloader.sha256)&&layout->id==r->requirements.layout.id&&!strcmp(sha,r->requirements.partition_sha256);
}
bool pet_firmware_image_begin(pet_firmware_image_t *image,pet_firmware_receipt_store_t *s,const pet_firmware_release_t *r,
    const pet_firmware_bootloader_t *profile,const pet_firmware_protection_t *p)
{assert(s->loaded&&s->value.phase==PET_FW_DOWNLOADING&&s->value.sequence==s->value.acknowledged_sequence);
 assert(pet_firmware_release_compatible(r,&boot.layout,boot.partition_sha256,profile,p)&&p->installed_count==pets.installed_count);
 assert(!image->active);++begins;image->active=true;image->written=0;image->image_bytes=r->bytes;return true;}
bool pet_firmware_image_write(pet_firmware_image_t *image,uint32_t offset,const void *p,size_t n)
{assert(image->active&&p&&offset==image->written&&n&&n<=65536);++writes;if(!write_ok)return false;image->written+=(uint32_t)n;return true;}
bool pet_firmware_image_finish(pet_firmware_image_t *image)
{assert(image->active&&image->written==image->image_bytes);++finishes;image->active=false;return true;}
bool pet_firmware_image_abort(pet_firmware_image_t *image){if(image->active&&!abort_ok)return false;image->active=false;return true;}
bool pet_firmware_image_select(pet_firmware_receipt_store_t *s,const pet_firmware_release_t *r,const char *boot_id,
    const pet_firmware_bootloader_t *profile,const pet_firmware_protection_t *p)
{assert(s->value.phase==PET_FW_REBOOTING&&s->value.sequence==s->value.acknowledged_sequence&&!strcmp(boot_id,s->value.report_boot_id));
 assert(pet_firmware_release_compatible(r,&boot.layout,boot.partition_sha256,profile,p)&&p->installed_count==pets.installed_count);
 ++selections;boot.selection.records[1]=(pet_ota_record_t){.sequence=4,.state=PET_OTA_STATE_NEW,.slot=1,.crc_valid=true,.bootable=true};
 boot.selection.active=1;return true;}
bool pet_firmware_image_confirm(uint32_t offset,const char *sha,const pet_firmware_bootloader_t *profile)
{assert(offset==boot.running_offset&&!strcmp(sha,running)&&profile->bytes);++confirms;
 if(confirm_ok)boot.selection.records[boot.selection.active].state=PET_OTA_STATE_VALID;return confirm_ok;}
bool pet_firmware_wire_encode_recovery_poll(const char *boot_id,char *json,size_t n)
{if(!boot_id||strlen(boot_id)!=36||n<2)return false;minimal_poll=true;strcpy(json,"p");return true;}
bool pet_firmware_wire_encode_poll(const pet_firmware_requirements_t *r,const char *sha,const char *boot_id,
 const pet_firmware_protection_t *p,char *json,size_t n)
{assert(r&&r->imported_release&&sha&&boot_id&&p&&n>1);minimal_poll=false;pack_unknown_poll=!p->known;polled_installed=p->installed_count;
 strcpy(json,"p");return true;}
bool pet_firmware_wire_encode_report(const pet_firmware_receipt_t *r,char *json,size_t n)
{assert(pet_firmware_receipt_valid(r)&&n>1);request_report=*r;strcpy(json,"r");return true;}
static void reported(pet_firmware_operation_t *o,const pet_firmware_receipt_t *r)
{
    o->report=(pet_firmware_report_t){.phase=r->phase==PET_FW_STAGED?PET_FW_DOWNLOADING:r->phase,.sequence=r->sequence,
        .downloaded_bytes=r->downloaded_bytes,.healthy_storage=r->healthy_storage};
    strcpy(o->report.boot_id,r->report_boot_id);strcpy(o->report.running_sha256,r->running_sha256);strcpy(o->report.error_code,r->error_code);
}
bool pet_firmware_wire_identity_matches(const pet_firmware_operation_t *o,const pet_firmware_receipt_t *r)
{return o&&pet_firmware_receipt_valid(r)&&!strcmp(o->id,r->operation_id)&&!strcmp(o->release.release_id,r->release_id)&&
 !strcmp(o->release.sha256,r->target_sha256)&&!strcmp(o->previous_sha256,r->previous_sha256)&&!strcmp(o->previous_boot_id,r->previous_boot_id);}
bool pet_firmware_wire_matches_receipt(const pet_firmware_operation_t *o,const pet_firmware_receipt_t *r)
{pet_firmware_operation_t expected=*o;reported(&expected,r);return pet_firmware_wire_identity_matches(o,r)&&!memcmp(&o->report,&expected.report,sizeof(o->report));}
bool pet_firmware_wire_poll(const char *json,size_t n,const char *device,const pet_pack_trust_key_t *keys,size_t count,pet_firmware_poll_t *out)
{(void)keys;(void)count;assert(json&&n==1&&device);if(!verified)return false;memset(out,0,sizeof(*out));out->has_operation=has_operation;if(has_operation)out->operation=operation;return true;}
bool pet_firmware_wire_operation(const char *json,size_t n,const char *device,const pet_pack_trust_key_t *keys,size_t count,pet_firmware_operation_t *out)
{(void)keys;(void)count;assert(json&&n==1&&device);if(!verified)return false;*out=reply;return true;}
bool pet_control_http_json(const pet_control_http_t *h,const char *path,const char *body,char *response,size_t n,pet_control_http_result_t *result)
{
    assert(h&&body&&n>1);now+=transport_delay_ms;*result=(pet_control_http_result_t){.status=transport_ok?200:503,.bytes=1,.retry_seconds=15};strcpy(response,"x");
    if(!strcmp(path,"/v2/device/firmware/poll"))++polls;
    else {assert(!strcmp(path,"/v2/device/firmware/report"));++reports;
        if(transport_ok){reply=operation;strcpy(reply.id,request_report.operation_id);reported(&reply,&request_report);
            operation=reply;if(operation.report.phase>=PET_FW_HEALTHY)has_operation=false;
            if(conflict_reply)++reply.report.sequence;
            if(lose_report_reply){lose_report_reply=false;result->status=503;return false;}
        }
    }
    return transport_ok;
}
bool pet_control_http_firmware_range(const pet_control_http_t *h,pet_control_download_t *d,const char *id,const char *sha,uint32_t total,
    uint32_t offset,void *p,size_t n,pet_control_http_result_t *result)
{assert(h&&d==&worker.download&&!strcmp(id,operation.id)&&!strcmp(sha,operation.release.sha256)&&total==operation.release.bytes&&offset+n<=total);
 ++ranges;*result=(pet_control_http_result_t){.status=transport_ok?206:503,.bytes=n,.retry_seconds=15};memset(p,0x57,n);
 if(!d->url[0])strcpy(d->url,"session");
 d->client=transport_ok&&offset+n<total?(void *)&worker:NULL; /* The kept connection. */
 return transport_ok;}
void pet_control_download_wrote(pet_control_download_t *d){assert(d==&worker.download&&d->url[0]);++download_writes;}
void pet_control_download_end(pet_control_download_t *d,bool complete)
{assert(d==&worker.download);if(d->url[0]){if(complete)++downloads_completed;else ++downloads_stopped;}memset(d,0,sizeof(*d));}
static void reset(void)
{
    memset(&worker,0,sizeof(worker));memset(&config,0,sizeof(config));memset(&store,0,sizeof(store));memset(records,0,sizeof(records));memset(present,0,sizeof(present));
    memset(&operation,0,sizeof(operation));memset(&reply,0,sizeof(reply));memset(&boot,0,sizeof(boot));
    has_operation=verified=transport_ok=known_layout=known_packs=bootloader_ok=write_ok=confirm_ok=true;
    persist_fail=persist_ambiguous=lose_report_reply=conflict_reply=minimal_poll=pack_unknown_poll=false;
    polls=reports=ranges=begins=writes=finishes=selections=confirms=restarts=freezes=polled_installed=0;now=1;health=PET_FW_STORAGE_SETUP;
    memset(&pets,0,sizeof(pets));memset(&fresh_pets,0,sizeof(fresh_pets));fresh_differs=false;fresh_proofs=0;
    transport_delay_ms=0;abort_ok=true;downloads_completed=downloads_stopped=download_writes=0;
    config.http=(pet_control_http_t){"https://fixture.invalid","fixture-device","not-a-real-credential"};
    strcpy(config.boot_id,initial_boot);config.firmware_epoch=2;config.bootloader.bytes=0x7000;memset(config.bootloader.sha256,'d',64);
    config.protection=protection;config.health=health_check;config.freeze=freeze;config.restart=restart;config.now_ms=clock_ms;
    assert(pet_flash_layout_known(PET_LAYOUT_SINGLE_2M,&boot.layout));memset(boot.partition_sha256,'c',64);boot.running_offset=0x30000;
    boot.selection.records[0]=(pet_ota_record_t){.sequence=3,.state=PET_OTA_STATE_VALID,.slot=0,.crc_valid=true,.bootable=true};boot.selection.active=0;
    operation.release.bytes=65537;operation.release.requirements.layout=boot.layout;operation.release.requirements.bootloader=config.bootloader;
    operation.release.requirements.firmware_epoch=3;operation.release.requirements.formats=6;operation.release.requirements.codecs=0xff;
    strcpy(operation.release.requirements.partition_sha256,boot.partition_sha256);strcpy(operation.release.version,"fixture");
    strcpy(operation.id,"00000000-0000-4000-8000-000000000001");strcpy(operation.release.release_id,"00000000-0000-4000-8000-000000000002");
    strcpy(operation.previous_boot_id,initial_boot);memset(operation.previous_sha256,'a',64);memset(operation.release.sha256,'b',64);strcpy(running,operation.previous_sha256);
    assert(pet_firmware_receipt_open(&store,&io));assert(pet_firmware_control_init(&worker,&config,&store));
}
static void step(void){pet_firmware_control_step(&worker,now);now+=100;}
static void until(pet_fw_phase_t phase,bool ack)
{for(unsigned i=0;i<200;++i){if(store.loaded&&store.value.phase==phase&&(!ack||store.value.sequence==store.value.acknowledged_sequence))return;step();}
 fprintf(stderr,"phase wanted %u got %u; status %u %s\n",phase,store.value.phase,worker.status,worker.error_code);assert(false);}
static void power_cycle(bool target)
{
    assert(pet_firmware_receipt_open(&store,&io));strcpy(config.boot_id,next_boot);
    if(target){boot.running_offset=0x230000;boot.running_slot=1;strcpy(running,operation.release.sha256);config.firmware_epoch=3;
        boot.selection.active=1;boot.selection.records[1]=(pet_ota_record_t){.sequence=4,.state=PET_OTA_STATE_PENDING,.slot=1,.crc_valid=true,.bootable=true};}
    assert(pet_firmware_control_init(&worker,&config,&store));now+=1000;
}
int main(void)
{
    reset();step();assert(polls==1&&!begins);step();assert(store.value.phase==PET_FW_DOWNLOADING&&!begins&&pet_firmware_control_blocks_pet(&worker));
    lose_report_reply=true;step();assert(!begins&&store.value.acknowledged_sequence==0&&operation.report.sequence==1);
    now+=15000;step();assert(store.value.acknowledged_sequence==1&&!begins);
    until(PET_FW_REBOOTING,true);assert(begins==1&&writes==2&&finishes==1&&!selections);
    assert(downloads_completed==1&&!downloads_stopped&&download_writes==2&&!worker.download.client&&!worker.download.url[0]);
    step();step();assert(selections==1&&restarts==1);
    assert(fresh_proofs==3); /* Accepting, starting the image and switching prove every pet again. */
    power_cycle(true);health=PET_FW_STORAGE_NONE;step();step();assert(!confirms&&store.value.phase==PET_FW_REBOOTING);
    health=PET_FW_STORAGE_RECOVERY;until(PET_FW_HEALTHY,false);assert(confirms==1&&boot.selection.records[1].state==PET_OTA_STATE_VALID);
    lose_report_reply=true;step();assert(!has_operation&&store.value.acknowledged_sequence<store.value.sequence);
    power_cycle(true);boot.selection.records[1].state=PET_OTA_STATE_VALID;
    unsigned before=polls;step();assert(polls==before&&store.value.acknowledged_sequence==store.value.sequence); // terminal ACK without a poll
    step();step();assert(worker.status==PET_FW_CONTROL_IDLE&&!pet_firmware_control_blocks_pet(&worker));

    // Power loss while downloading restarts bytes at zero, but never regresses
    // durable/cloud progress or accepts a different operation.
    reset();until(PET_FW_DOWNLOADING,true);step();step();step();assert(worker.image.written==65536);
    uint32_t high=store.value.downloaded_bytes;assert(high==65536);power_cycle(false);
    until(PET_FW_STAGED,false);assert(begins==2&&store.value.downloaded_bytes>=high);
    // STAGED after a new boot requires exact cloud correlation before selecting.
    assert(pet_firmware_receipt_open(&store,&io));strcpy(config.boot_id,"00000000-0000-4000-8000-000000000005");
    assert(pet_firmware_control_init(&worker,&config,&store));until(PET_FW_REBOOTING,true);
    assert(!strcmp(store.value.report_boot_id,config.boot_id));

    // Changed boot + missing target record is ambiguous attempt history. With
    // a proven previous VALID selection report failure, never retry candidate.
    reset();until(PET_FW_REBOOTING,true);power_cycle(false);until(PET_FW_FAILED,false);
    assert(!selections&&!restarts&&!strcmp(store.value.error_code,"FIRMWARE_BOOT_OUTCOME_UNKNOWN"));
    reset();until(PET_FW_REBOOTING,true);power_cycle(false);
    boot.selection.records[1]=(pet_ota_record_t){.sequence=4,.state=PET_OTA_STATE_ABORTED,.slot=1,.crc_valid=true};
    until(PET_FW_ROLLED_BACK,false);assert(!selections&&!restarts);
    reset();until(PET_FW_REBOOTING,true);power_cycle(false);
    boot.selection.active=1;boot.selection.records[1]=(pet_ota_record_t){.sequence=4,.state=PET_OTA_STATE_NEW,.slot=1,.crc_valid=true,.bootable=true};
    step();step();assert(worker.status==PET_FW_CONTROL_RECOVERY&&!selections&&!restarts&&store.value.phase==PET_FW_REBOOTING);

    // VALID-before-receipt failure: reopen the durable REBOOTING record, rerun
    // health on the actual already VALID target, and finish without download.
    reset();until(PET_FW_REBOOTING,true);power_cycle(true);step();persist_fail=true;step();
    assert(!store.loaded&&confirms==1&&boot.selection.records[1].state==PET_OTA_STATE_VALID);
    persist_fail=false;power_cycle(true);boot.selection.records[1].state=PET_OTA_STATE_VALID;strcpy(config.boot_id,"00000000-0000-4000-8000-000000000006");
    assert(pet_firmware_control_init(&worker,&config,&store));until(PET_FW_HEALTHY,false);assert(begins==1&&confirms==2);

    reset();known_layout=false;step();assert(minimal_poll&&polls==1);step();assert(worker.status==PET_FW_CONTROL_RECOVERY&&!begins);
    reset();store.loaded=false;step();assert(polls==1&&pack_unknown_poll);step();assert(worker.status==PET_FW_CONTROL_RECOVERY&&!begins);
    reset();known_packs=false;step();step();assert(worker.status==PET_FW_CONTROL_RECOVERY&&!begins&&store.value.phase==PET_FW_EMPTY);
    reset();verified=false;step();assert(worker.status==PET_FW_CONTROL_RECOVERY&&!begins&&!worker.authenticated);
    reset();until(PET_FW_DOWNLOADING,false);conflict_reply=true;step();assert(!store.value.acknowledged_sequence&&!begins&&worker.status==PET_FW_CONTROL_RECOVERY);
    reset();until(PET_FW_DOWNLOADING,true);++operation.report.sequence;step();step();assert(worker.status==PET_FW_CONTROL_RECOVERY&&!begins);
    reset();until(PET_FW_DOWNLOADING,true);strcpy(operation.id,"00000000-0000-4000-8000-000000000009");step();step();assert(worker.status==PET_FW_CONTROL_RECOVERY&&!begins);
    reset();write_ok=false;until(PET_FW_FAILED,false);assert(begins==1&&writes==1&&!worker.image.active&&!selections);
    assert(worker.download.client);step();assert(downloads_stopped==1&&!downloads_completed&&!worker.download.client); // Failed image.
    reset();transport_ok=false;transport_delay_ms=30000;step();
    assert(worker.retry_at_ms==45001);step();assert(polls==1); // full Retry-After after slow response
    reset();until(PET_FW_DOWNLOADING,true);step();step();abort_ok=false;persist_fail=true;step();
    assert(worker.writer_fault&&!store.loaded&&worker.image.active);unsigned old_writes=writes,old_reports=reports;
    persist_fail=false;assert(pet_firmware_receipt_open(&store,&io));step();
    assert(worker.status==PET_FW_CONTROL_RECOVERY&&writes==old_writes&&reports==old_reports&&pet_firmware_control_blocks_pet(&worker));
    abort_ok=true;until(PET_FW_STAGED,false);assert(!worker.writer_fault&&begins==2);

    // Three-pet protection: every installed pet reaches the acceptance,
    // download, staging and boot-selection checks, not only the one on screen.
    for(unsigned later=0;later<4;++later){
        reset();pets.installed_count=2;
        for(unsigned i=0;i<2;++i)pets.installed[i].requirements=(pet_firmware_protected_pack_t){.present=true,.minimum_firmware_epoch=3};
        if(later==0){
            pets.installed[1].requirements.minimum_firmware_epoch=4;step();assert(polls==1&&polled_installed==2);step();
            assert(worker.status==PET_FW_CONTROL_RECOVERY&&!strcmp(worker.error_code,"FIRMWARE_ACCEPT_REJECTED")&&!begins&&store.value.phase==PET_FW_EMPTY);
            continue;
        }
        const pet_fw_phase_t phase[]={PET_FW_EMPTY,PET_FW_DOWNLOADING,PET_FW_STAGED,PET_FW_REBOOTING};
        until(phase[later],phase[later]!=PET_FW_STAGED);
        /* A pet installed meanwhile that this candidate cannot read stops it. */
        pets.installed[1].requirements.minimum_firmware_epoch=4;unsigned before_selections=selections;
        for(unsigned i=0;i<5&&worker.status!=PET_FW_CONTROL_RECOVERY;++i)step();
        assert(worker.status==PET_FW_CONTROL_RECOVERY&&!strcmp(worker.error_code,"FIRMWARE_INCOMPATIBLE")&&selections==before_selections);
        pets.installed[1].requirements.minimum_firmware_epoch=3;until(PET_FW_REBOOTING,true);
        for(unsigned i=0;i<5&&selections==before_selections;++i)step();
        assert(selections==before_selections+1&&restarts==1);
    }

    // Polls and checks may reuse the last proof; each flash action proves every
    // pet again and refuses one that flash no longer proves.
    for(unsigned action=0;action<3;++action){
        reset();
        const pet_fw_phase_t before[]={PET_FW_EMPTY,PET_FW_DOWNLOADING,PET_FW_REBOOTING};
        if(action)until(before[action],true);else step();
        fresh_differs=true;fresh_pets.installed_count=1;
        fresh_pets.installed[0].requirements=(pet_firmware_protected_pack_t){.present=true,.minimum_firmware_epoch=4};
        unsigned before_begins=begins;step();step();
        assert(worker.status==PET_FW_CONTROL_RECOVERY&&!strcmp(worker.error_code,action?"FIRMWARE_INCOMPATIBLE":"FIRMWARE_ACCEPT_REJECTED"));
        assert(begins==before_begins&&!selections&&!restarts&&store.value.phase==before[action]);
    }
    // A three-pet device never updates to an older epoch (Pocket firmware before
    // epoch 4 cannot report its pets); a single-pet device still may.
    reset();config.firmware_epoch=4;assert(pet_firmware_control_init(&worker,&config,&store));
    until(PET_FW_DOWNLOADING,true);assert(operation.release.requirements.firmware_epoch==3);
    reset();assert(pet_flash_layout_known(PET_LAYOUT_THREE_3P5M,&boot.layout));operation.release.requirements.layout=boot.layout;
    config.firmware_epoch=4;assert(pet_firmware_control_init(&worker,&config,&store));step();step();
    assert(worker.status==PET_FW_CONTROL_RECOVERY&&!strcmp(worker.error_code,"FIRMWARE_ACCEPT_REJECTED")&&store.value.phase==PET_FW_EMPTY);
    operation.release.requirements.firmware_epoch=4;until(PET_FW_DOWNLOADING,true);

    reset();until(PET_FW_REBOOTING,true);power_cycle(true);health=PET_FW_STORAGE_NONE;transport_ok=false;
    assert(!pet_firmware_control_health_deadline(&worker,89999));assert(pet_firmware_control_health_deadline(&worker,90000)&&restarts==1);
    boot.selection.active=0;assert(!pet_firmware_control_health_deadline(&worker,100000)&&restarts==1);
    boot.selection.active=1;boot.selection.records[1].state=PET_OTA_STATE_VALID;
    assert(!pet_firmware_control_health_deadline(&worker,100000)&&restarts==1);
    puts("firmware worker: receipt-first ACK replay, bounded transfer, reboot correlation, no-pet health, ambiguous rollback, recovery polling and every installed pet passed");return 0;
}
