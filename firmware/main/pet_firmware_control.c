#include "pet_firmware_control.h"
#include <string.h>

static bool terminal(pet_fw_phase_t p)
{return p==PET_FW_HEALTHY||p==PET_FW_FAILED||p==PET_FW_ROLLED_BACK;}
static pet_firmware_control_status_t status(pet_firmware_control_t *w,pet_firmware_control_status_t value,const char *error)
{w->status=value;if(error){strncpy(w->error_code,error,sizeof(w->error_code)-1);w->error_code[80]=0;}else w->error_code[0]=0;return value;}
static uint64_t completed_at(pet_firmware_control_t *w,uint64_t started)
{uint64_t actual=w->config.now_ms(w->config.context);return actual>started?actual:started;}
static bool save(pet_firmware_control_t *w,const pet_firmware_receipt_t *next)
{
    if(pet_firmware_receipt_save(w->store,next))return true;
    /* A failed persistence/read-back is ambiguous. Quiesce any handle, but do
     * not release the cloud reservation or invent a terminal report. */
    w->writer_fault=!pet_firmware_image_abort(&w->image);status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_STORAGE");return false;
}
static bool snapshot(pet_firmware_control_t *w,pet_firmware_boot_state_t *boot,
                     pet_firmware_requirements_t *caps,char running[65],pet_firmware_protection_t *packs)
{
    memset(caps,0,sizeof(*caps));memset(packs,0,sizeof(*packs));running[0]=0;
    uint32_t bytes;
    if(!w->config.firmware_epoch||!pet_firmware_image_boot_state(boot)||!pet_firmware_image_running(running,&bytes))return false;
    caps->layout=boot->layout;strcpy(caps->partition_sha256,boot->partition_sha256);
    caps->firmware_epoch=w->config.firmware_epoch;caps->formats=6;caps->codecs=0xff;
    caps->imported_release=true; /* pet_release_v2.c verifies imported releases. */
    if(pet_firmware_image_bootloader_matches(&w->config.bootloader))caps->bootloader=w->config.bootloader;
    w->config.protection(w->config.context,false,packs);
    if(!w->store->loaded){memset(packs,0,sizeof(*packs));}return true;
}
/* A three-pet device never returns to an older epoch: Pocket firmware before
 * epoch 4 cannot report its pets, and a pet store format change bumps it. */
static bool compatible(pet_firmware_control_t *w,const pet_firmware_requirements_t *caps,const pet_firmware_protection_t *packs)
{
    return (caps->layout.pet_slots!=PET_LAYOUT_THREE_SLOTS||w->operation.release.requirements.firmware_epoch>=caps->firmware_epoch)&&
        pet_firmware_release_compatible(&w->operation.release,&caps->layout,caps->partition_sha256,&caps->bootloader,packs);
}
/* Before a flash action, every pet is proved again instead of trusting the
 * last proof. */
static bool reproved(pet_firmware_control_t *w,const pet_firmware_requirements_t *caps,pet_firmware_protection_t *packs)
{
    memset(packs,0,sizeof(*packs));w->config.protection(w->config.context,true,packs);
    return w->store->loaded&&compatible(w,caps,packs);
}
static void network_wait(pet_firmware_control_t *w,uint64_t now,const pet_control_http_result_t *result)
{
    unsigned seconds=result->retry_seconds?result->retry_seconds:15;
    w->retry_at_ms=completed_at(w,now)+(uint64_t)seconds*1000;w->authenticated=false;
    status(w,PET_FW_CONTROL_WAITING,"FIRMWARE_CONTROL_UNAVAILABLE");
}
bool pet_firmware_control_init(pet_firmware_control_t *w,const pet_firmware_control_config_t *config,pet_firmware_receipt_store_t *store)
{
    if(!w||!config||!store||!config->http.origin||!config->http.device_id||!config->http.credential||
       !config->protection||!config->health||!config->freeze||!config->restart||!config->now_ms||(config->key_count&&!config->keys)||
       !memchr(config->boot_id,0,sizeof(config->boot_id)))return false;
    char probe[256];if(!pet_firmware_wire_encode_recovery_poll(config->boot_id,probe,sizeof(probe)))return false;
    memset(w,0,sizeof(*w));w->config=*config;w->store=store;return true;
}
bool pet_firmware_control_blocks_pet(const pet_firmware_control_t *w)
{
    if(!w||!w->store||!w->store->loaded||w->image.active||w->has_operation||w->writer_fault)return true;
    const pet_firmware_receipt_t *r=&w->store->value;
    return !pet_firmware_receipt_valid(r)||(r->phase!=PET_FW_EMPTY&&(!terminal(r->phase)||r->acknowledged_sequence!=r->sequence));
}
static pet_firmware_control_status_t report(pet_firmware_control_t *w,uint64_t now)
{
    pet_control_http_result_t result={0};pet_firmware_operation_t reply;
    if(!pet_firmware_wire_encode_report(&w->store->value,w->request,sizeof(w->request)))
        return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_INVALID");
    if(!pet_control_http_json(&w->config.http,"/v2/device/firmware/report",w->request,w->response,sizeof(w->response),&result)||result.status!=200){
        network_wait(w,now,&result);return w->status;
    }
    if(!pet_firmware_wire_operation(w->response,result.bytes,w->config.http.device_id,w->config.keys,w->config.key_count,&reply)||
       !pet_firmware_wire_matches_receipt(&reply,&w->store->value)){
        w->authenticated=false;w->retry_at_ms=completed_at(w,now)+15000;return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_CONFLICT");
    }
    pet_firmware_receipt_t next=w->store->value;
    if(!pet_firmware_receipt_ack(&next,next.sequence))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_INVALID");
    if(!save(w,&next))return w->status;
    w->authenticated=true;w->next_poll_ms=0; // Refresh current cloud identity before any next flash action.
    return status(w,PET_FW_CONTROL_WORKING,NULL);
}
static pet_firmware_control_status_t poll(pet_firmware_control_t *w,uint64_t now)
{
    pet_firmware_boot_state_t boot;pet_firmware_requirements_t caps;char running[65];pet_firmware_protection_t packs;
    bool known=snapshot(w,&boot,&caps,running,&packs);
    bool encoded=known?pet_firmware_wire_encode_poll(&caps,running,w->config.boot_id,&packs,w->request,sizeof(w->request)):
        pet_firmware_wire_encode_recovery_poll(w->config.boot_id,w->request,sizeof(w->request));
    if(!encoded)return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_SNAPSHOT_INVALID");
    pet_control_http_result_t result={0};pet_firmware_poll_t reply;
    if(!pet_control_http_json(&w->config.http,"/v2/device/firmware/poll",w->request,w->response,sizeof(w->response),&result)||result.status!=200){
        network_wait(w,now,&result);return w->status;
    }
    if(!pet_firmware_wire_poll(w->response,result.bytes,w->config.http.device_id,w->config.keys,w->config.key_count,&reply)){
        w->authenticated=false;w->retry_at_ms=completed_at(w,now)+15000;return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_OPERATION_UNTRUSTED");
    }
    w->authenticated=true;w->next_poll_ms=completed_at(w,now)+15000;w->has_operation=reply.has_operation;
    if(reply.has_operation)w->operation=reply.operation;else memset(&w->operation,0,sizeof(w->operation));
    return status(w,PET_FW_CONTROL_WORKING,NULL);
}
static pet_firmware_control_status_t fail_quiescent(pet_firmware_control_t *w,bool rollback,const char *error)
{
    if(!pet_firmware_image_abort(&w->image)){w->writer_fault=true;return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_WRITER_UNCERTAIN");}
    pet_firmware_boot_state_t boot;char running[65];uint32_t bytes;
    if(!pet_firmware_image_boot_state(&boot)||!pet_ota_selection_is(&boot.selection,boot.running_slot,PET_OTA_STATE_VALID)||
       boot.running_offset==w->store->value.target_offset||!pet_firmware_image_running(running,&bytes)||
       strcmp(running,w->store->value.previous_sha256))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_BOOT_UNCERTAIN");
    pet_firmware_receipt_t next=w->store->value;
    if(!pet_firmware_receipt_fail(&next,rollback,w->config.boot_id,running,error))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_INVALID");
    if(!save(w,&next))return w->status;
    return status(w,PET_FW_CONTROL_WORKING,error);
}
static pet_firmware_control_status_t download(pet_firmware_control_t *w,uint64_t now,const pet_firmware_protection_t *packs)
{
    if(!w->image.active){
        w->config.freeze(w->config.context);
        if(!pet_firmware_image_begin(&w->image,w->store,&w->operation.release,&w->config.bootloader,packs))
            return fail_quiescent(w,false,"FIRMWARE_BEGIN_FAILED");
        return status(w,PET_FW_CONTROL_WORKING,NULL);
    }
    if(w->image.written==w->image.image_bytes){
        pet_control_download_end(&w->download,true);
        if(!pet_firmware_image_finish(&w->image))return fail_quiescent(w,false,"FIRMWARE_VERIFY_FAILED");
        pet_firmware_receipt_t next=w->store->value;
        if(!pet_firmware_receipt_stage(&next,next.target_sha256))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_INVALID");
        if(!save(w,&next))return w->status;
        return status(w,PET_FW_CONTROL_WORKING,NULL);
    }
    uint32_t offset=w->image.written;size_t bytes=w->image.image_bytes-offset;if(bytes>sizeof(w->chunk))bytes=sizeof(w->chunk);
    pet_control_http_result_t result={0};
    if(!pet_control_http_firmware_range(&w->config.http,&w->download,w->operation.id,w->operation.release.sha256,
        w->operation.release.bytes,offset,w->chunk,bytes,&result)){network_wait(w,now,&result);return w->status;}
    if(!pet_firmware_image_write(&w->image,offset,w->chunk,bytes))return fail_quiescent(w,false,"FIRMWARE_WRITE_FAILED");
    pet_firmware_receipt_t next=w->store->value;uint32_t high=w->image.written>next.downloaded_bytes?w->image.written:next.downloaded_bytes;
    if(!pet_firmware_receipt_progress(&next,high,w->config.boot_id))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_INVALID");
    if(!save(w,&next))return w->status;
    pet_control_download_wrote(&w->download);
    return status(w,PET_FW_CONTROL_WORKING,NULL);
}
pet_firmware_control_status_t pet_firmware_control_step(pet_firmware_control_t *w,uint64_t now)
{
    if(!w||!w->store)return PET_FW_CONTROL_RECOVERY;
    if(!w->image.active||!w->has_operation||(w->status!=PET_FW_CONTROL_WORKING&&w->status!=PET_FW_CONTROL_WAITING))
        pet_control_download_end(&w->download,false);
    if(now<w->retry_at_ms)return w->status;
    if(w->writer_fault){
        if(!pet_firmware_image_abort(&w->image)){
            if(!w->next_poll_ms||now>=w->next_poll_ms)poll(w,now);
            return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_WRITER_UNCERTAIN");
        }
        w->writer_fault=false;
    }
    if(w->store->loaded&&!pet_firmware_receipt_valid(&w->store->value)){
        w->writer_fault=!pet_firmware_image_abort(&w->image);w->store->loaded=false;
    }
    if(w->store->loaded&&w->store->value.sequence>w->store->value.acknowledged_sequence)return report(w,now);
    if(!w->next_poll_ms||now>=w->next_poll_ms)return poll(w,now);
    if(!w->store->loaded){w->writer_fault=!pet_firmware_image_abort(&w->image);return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_STORAGE");}
    pet_firmware_receipt_t *r=&w->store->value;
    if(!w->has_operation)return r->phase==PET_FW_EMPTY||terminal(r->phase)?status(w,PET_FW_CONTROL_IDLE,NULL):
        status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_OPERATION_MISSING");
    pet_firmware_boot_state_t boot;pet_firmware_requirements_t caps;pet_firmware_protection_t packs;char running[65];
    if(!snapshot(w,&boot,&caps,running,&packs))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_CAPABILITIES_UNKNOWN");
    if(r->phase==PET_FW_EMPTY||terminal(r->phase)){
        if(w->operation.report.phase!=PET_FW_EMPTY||strcmp(running,w->operation.previous_sha256)||!compatible(w,&caps,&packs)||
           !pet_ota_selection_is(&boot.selection,boot.running_slot,PET_OTA_STATE_VALID)||!reproved(w,&caps,&packs))
            return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_ACCEPT_REJECTED");
        uint32_t target=boot.running_slot?0x30000:0x30000+boot.layout.app_slot_bytes;
        pet_firmware_receipt_t next=*r;
        if(!pet_firmware_receipt_begin_at_boot(&next,&w->operation.release,w->operation.id,w->operation.previous_sha256,
            w->operation.previous_boot_id,w->config.boot_id,target)||!save(w,&next))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_STORAGE");
        w->config.freeze(w->config.context);return status(w,PET_FW_CONTROL_WORKING,NULL);
    }
    if(!pet_firmware_wire_identity_matches(&w->operation,r)||!pet_firmware_wire_matches_receipt(&w->operation,r))
        return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_CONFLICT");
    if(boot.layout.id!=r->layout_id||strcmp(boot.partition_sha256,r->partition_sha256))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_LAYOUT_CHANGED");
    if(boot.running_offset==r->target_offset){
        if((r->phase!=PET_FW_STAGED&&r->phase!=PET_FW_REBOOTING)||strcmp(running,r->target_sha256)||
           !strcmp(w->config.boot_id,r->report_boot_id)||caps.firmware_epoch!=r->firmware_epoch||
           !caps.bootloader.bytes||caps.bootloader.bytes!=w->operation.release.requirements.bootloader.bytes||
           strcmp(caps.bootloader.sha256,w->operation.release.requirements.bootloader.sha256))
            return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_TARGET_UNPROVEN");
        pet_fw_storage_t health=w->config.health(w->config.context);
        if(!w->authenticated||health<PET_FW_STORAGE_PACK||health>PET_FW_STORAGE_RECOVERY)
            return status(w,PET_FW_CONTROL_WAITING,"FIRMWARE_HEALTH_PENDING");
        if(!pet_firmware_image_confirm(r->target_offset,r->target_sha256,&w->config.bootloader))
            return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_BOOT_UNCERTAIN");
        pet_firmware_receipt_t next=*r;
        if(!pet_firmware_receipt_healthy(&next,w->config.boot_id,running,true,true,true,true,health))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_INVALID");
        if(!save(w,&next))return w->status;
        return status(w,PET_FW_CONTROL_WORKING,NULL);
    }
    if(strcmp(running,r->previous_sha256))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_PREVIOUS_UNPROVEN");
    if(r->phase==PET_FW_REBOOTING&&strcmp(w->config.boot_id,r->report_boot_id)){
        if(!pet_ota_selection_is(&boot.selection,boot.running_slot,PET_OTA_STATE_VALID))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_BOOT_UNCERTAIN");
        int target=pet_ota_selection_newest(&boot.selection,1-boot.running_slot);
        bool rollback=target>=0&&(boot.selection.records[target].state==PET_OTA_STATE_ABORTED||boot.selection.records[target].state==PET_OTA_STATE_INVALID);
        return fail_quiescent(w,rollback,rollback?"FIRMWARE_BOOT_ROLLED_BACK":"FIRMWARE_BOOT_OUTCOME_UNKNOWN");
    }
    const bool acting=(r->phase==PET_FW_DOWNLOADING&&!w->image.active)||r->phase==PET_FW_REBOOTING; /* Image start, switch. */
    if(!compatible(w,&caps,&packs)||(acting&&!reproved(w,&caps,&packs)))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_INCOMPATIBLE");
    if(r->phase==PET_FW_DOWNLOADING)return download(w,now,&packs);
    if(r->phase==PET_FW_STAGED){
        if(!pet_ota_selection_is(&boot.selection,boot.running_slot,PET_OTA_STATE_VALID))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_BOOT_UNCERTAIN");
        pet_firmware_receipt_t next=*r;
        if(!pet_firmware_receipt_reboot_at_boot(&next,w->config.boot_id))return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_RECEIPT_INVALID");
        if(!save(w,&next))return w->status;
        return status(w,PET_FW_CONTROL_WORKING,NULL);
    }
    if(r->phase==PET_FW_REBOOTING){
        w->config.freeze(w->config.context);
        if(!pet_firmware_image_select(w->store,&w->operation.release,w->config.boot_id,&w->config.bootloader,&packs))
            return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_BOOT_UNCERTAIN");
        status(w,PET_FW_CONTROL_RESTARTING,NULL);w->config.restart(w->config.context);return w->status;
    }
    return status(w,PET_FW_CONTROL_RECOVERY,"FIRMWARE_STATE_INVALID");
}
bool pet_firmware_control_health_deadline(pet_firmware_control_t *w,uint64_t elapsed)
{
    if(!w||!w->store||!w->store->loaded||elapsed<90000)return false;
    const pet_firmware_receipt_t *r=&w->store->value;
    if(!pet_firmware_receipt_valid(r)||(r->phase!=PET_FW_STAGED&&r->phase!=PET_FW_REBOOTING)||
       !strcmp(w->config.boot_id,r->report_boot_id)||!pet_firmware_image_bootloader_matches(&w->config.bootloader))return false;
    pet_firmware_boot_state_t boot;char sha[65];uint32_t bytes;
    if(!pet_firmware_image_boot_state(&boot)||boot.running_offset!=r->target_offset||
       !pet_ota_selection_is(&boot.selection,boot.running_slot,PET_OTA_STATE_PENDING)||
       !pet_firmware_image_running(sha,&bytes)||strcmp(sha,r->target_sha256))return false;
    w->config.freeze(w->config.context);status(w,PET_FW_CONTROL_RESTARTING,"FIRMWARE_HEALTH_TIMEOUT");
    w->config.restart(w->config.context);return true;
}
