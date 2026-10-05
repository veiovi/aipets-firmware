#include "pet_firmware_wire.h"
#include "pet_board.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#ifdef ESP_PLATFORM
#include "esp_mac.h"
#endif

static const cJSON *field(const cJSON *o,const char *name){return cJSON_GetObjectItemCaseSensitive(o,name);}
static bool bounded(const char *s,size_t cap){if(!s)return false;for(size_t i=0;i<cap;++i)if(!s[i])return true;return false;}
static bool sha(const char *s){return bounded(s,65)&&strlen(s)==64&&strspn(s,"0123456789abcdef")==64;}
static bool uuid(const char *s)
{
    if(!bounded(s,37)||strlen(s)!=36)return false;
    for(unsigned i=0;i<36;++i){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}
        else if(!strchr("0123456789abcdef",s[i]))return false;}
    return s[14]>='1'&&s[14]<='5'&&strchr("89ab",s[19]);
}
static bool keys(const cJSON *o,const char *names,unsigned count)
{
    if(!cJSON_IsObject(o)||cJSON_GetArraySize(o)!=(int)count)return false;
    for(const cJSON *item=o->child;item;item=item->next){bool found=false;
        for(const char *p=names;*p;){const char *end=strchr(p,'|');size_t n=end?(size_t)(end-p):strlen(p);
            if(item->string&&strlen(item->string)==n&&!memcmp(p,item->string,n)){found=true;break;}
            if(!end)break;
            p=end+1;
        }if(!found)return false;
    }return true;
}
static bool equal(const cJSON *o,const char *name,const char *value)
{const cJSON *v=field(o,name);return cJSON_IsString(v)&&!strcmp(v->valuestring,value);}
static bool text(const cJSON *o,const char *name,char *out,size_t cap)
{const cJSON *v=field(o,name);if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=cap)return false;strcpy(out,v->valuestring);return true;}
static bool number(const cJSON *o,const char *name,uint32_t min,uint32_t max,uint32_t *out)
{
    const cJSON *v=field(o,name);if(!cJSON_IsNumber(v)||!(v->valuedouble>=min&&v->valuedouble<=max))return false;
    uint32_t n=(uint32_t)v->valuedouble;if(v->valuedouble!=n)return false;*out=n;return true;
}
static bool exact(const cJSON *o,const char *name,uint32_t value){uint32_t n;return number(o,name,value,value,&n);}
static pet_fw_storage_t storage(const cJSON *o)
{
    if(!keys(o,"uiReady|storage|wifiConnected|controlAuthenticated",4)||!cJSON_IsTrue(field(o,"uiReady"))||
       !cJSON_IsTrue(field(o,"wifiConnected"))||!cJSON_IsTrue(field(o,"controlAuthenticated")))return PET_FW_STORAGE_NONE;
    return equal(o,"storage","pack-ready")?PET_FW_STORAGE_PACK:equal(o,"storage","setup-ready")?PET_FW_STORAGE_SETUP:
        equal(o,"storage","recovery-ready")?PET_FW_STORAGE_RECOVERY:PET_FW_STORAGE_NONE;
}
static const char *storage_name(pet_fw_storage_t value)
{switch(value){case PET_FW_STORAGE_PACK:return "pack-ready";case PET_FW_STORAGE_SETUP:return "setup-ready";
 case PET_FW_STORAGE_RECOVERY:return "recovery-ready";default:return NULL;}}
static bool parse_phase(const cJSON *o,pet_fw_phase_t *out)
{
    const char *names[]={"queued","downloading","rebooting","healthy","failed","rolled_back"};
    const pet_fw_phase_t phases[]={PET_FW_EMPTY,PET_FW_DOWNLOADING,PET_FW_REBOOTING,PET_FW_HEALTHY,PET_FW_FAILED,PET_FW_ROLLED_BACK};
    for(unsigned i=0;i<6;++i)if(equal(o,"state",names[i])){*out=phases[i];return true;}
    return false;
}
static bool parse_operation(const cJSON *o,const char *device,const pet_pack_trust_key_t *trust,size_t count,pet_firmware_operation_t *out)
{
    pet_firmware_operation_t next={0};pet_firmware_report_t *r=&next.report;
    if(!keys(o,"version|operationId|deviceId|releaseId|requestId|state|sha256|bytes|downloadedBytes|sequence|previousSha256|previousBootId|downloadPath|manifest|release|lastReport",16)||
       !exact(o,"version",2)||!device||!equal(o,"deviceId",device)||
       !text(o,"operationId",next.id,sizeof(next.id))||!uuid(next.id)||
       !text(o,"requestId",next.request_id,sizeof(next.request_id))||!uuid(next.request_id)||
       !text(o,"previousSha256",next.previous_sha256,sizeof(next.previous_sha256))||!sha(next.previous_sha256)||
       !text(o,"previousBootId",next.previous_boot_id,sizeof(next.previous_boot_id))||!uuid(next.previous_boot_id)||
       !parse_phase(o,&r->phase)||!number(o,"sequence",0,UINT32_MAX,&r->sequence)||
       !pet_firmware_release_verify(field(o,"manifest"),trust,count,&next.release)||
       !equal(o,"releaseId",next.release.release_id)||!equal(o,"sha256",next.release.sha256)||
       !exact(o,"bytes",next.release.bytes)||!number(o,"downloadedBytes",0,next.release.bytes,&r->downloaded_bytes))return false;
    char path[80];snprintf(path,sizeof(path),"/v2/device/firmware/%s/binary",next.id);
    if(!equal(o,"downloadPath",path))return false;
    const cJSON *payload=field(field(o,"manifest"),"payload");
    cJSON *signed_release=pet_control_json(payload->valuestring,strlen(payload->valuestring),7000);
    bool same=cJSON_Compare(signed_release,field(o,"release"),true);cJSON_Delete(signed_release);if(!same)return false;
    const cJSON *report=field(o,"lastReport");
    if(r->phase==PET_FW_EMPTY){if(r->sequence||r->downloaded_bytes||!cJSON_IsNull(report))return false;}
    else {
        if(!keys(report,"version|operationId|releaseId|sha256|sequence|status|downloadedBytes|bootId|runningSha256|health|errorCode",11)||
           !r->sequence||!exact(report,"version",2)||!equal(report,"operationId",next.id)||
           !equal(report,"releaseId",next.release.release_id)||!equal(report,"sha256",next.release.sha256)||
           !exact(report,"sequence",r->sequence)||!exact(report,"downloadedBytes",r->downloaded_bytes)||
           !equal(report,"status",field(o,"state")->valuestring)||
           !text(report,"bootId",r->boot_id,sizeof(r->boot_id))||!uuid(r->boot_id)||
           !text(report,"runningSha256",r->running_sha256,sizeof(r->running_sha256))||!sha(r->running_sha256))return false;
        if(r->phase==PET_FW_HEALTHY){r->healthy_storage=storage(field(report,"health"));if(!r->healthy_storage)return false;}
        else if(!cJSON_IsNull(field(report,"health")))return false;
        if(r->phase==PET_FW_FAILED||r->phase==PET_FW_ROLLED_BACK){
            if(!text(report,"errorCode",r->error_code,sizeof(r->error_code))||r->error_code[0]<'A'||r->error_code[0]>'Z'||
               strspn(r->error_code,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=strlen(r->error_code))return false;
        }else if(!cJSON_IsNull(field(report,"errorCode")))return false;
        if((r->phase==PET_FW_REBOOTING||r->phase==PET_FW_HEALTHY)&&r->downloaded_bytes!=next.release.bytes)return false;
        if((r->phase==PET_FW_DOWNLOADING||r->phase==PET_FW_REBOOTING||r->phase==PET_FW_ROLLED_BACK)&&strcmp(r->running_sha256,next.previous_sha256))return false;
        if(r->phase==PET_FW_HEALTHY&&strcmp(r->running_sha256,next.release.sha256))return false;
        if((r->phase==PET_FW_HEALTHY||r->phase==PET_FW_ROLLED_BACK)&&!strcmp(r->boot_id,next.previous_boot_id))return false;
    }
    *out=next;return true;
}
bool pet_firmware_wire_operation(const char *json,size_t bytes,const char *device,
                                 const pet_pack_trust_key_t *trust,size_t count,pet_firmware_operation_t *out)
{
    if(!out)return false;
    cJSON *root=pet_control_json(json,bytes,PET_CONTROL_RESPONSE_MAX);
    bool ok=parse_operation(root,device,trust,count,out);cJSON_Delete(root);return ok;
}
bool pet_firmware_wire_poll(const char *json,size_t bytes,const char *device,
                            const pet_pack_trust_key_t *trust,size_t count,pet_firmware_poll_t *out)
{
    if(!out)return false;
    cJSON *root=pet_control_json(json,bytes,PET_CONTROL_RESPONSE_MAX);pet_firmware_poll_t next={0};
    bool ok=keys(root,"version|revision|operation|nextPollSeconds",4)&&exact(root,"version",2)&&exact(root,"nextPollSeconds",15)&&
        text(root,"revision",next.revision,sizeof(next.revision))&&uuid(next.revision);
    const cJSON *operation=field(root,"operation");next.has_operation=!cJSON_IsNull(operation);
    if(ok&&next.has_operation)ok=parse_operation(operation,device,trust,count,&next.operation);
    cJSON_Delete(root);if(ok)*out=next;return ok;
}
bool pet_firmware_wire_identity_matches(const pet_firmware_operation_t *o,const pet_firmware_receipt_t *r)
{
    if(!o||!pet_firmware_receipt_valid(r)||r->phase==PET_FW_EMPTY)return false;
    const pet_firmware_release_t *v=&o->release;
    return !strcmp(o->id,r->operation_id)&&!strcmp(v->release_id,r->release_id)&&!strcmp(v->sha256,r->target_sha256)&&
        !strcmp(v->version,r->firmware_version)&&v->bytes==r->image_bytes&&v->requirements.firmware_epoch==r->firmware_epoch&&
        v->requirements.layout.id==r->layout_id&&!strcmp(v->requirements.partition_sha256,r->partition_sha256)&&
        !strcmp(o->previous_sha256,r->previous_sha256)&&!strcmp(o->previous_boot_id,r->previous_boot_id);
}
bool pet_firmware_wire_matches_receipt(const pet_firmware_operation_t *o,const pet_firmware_receipt_t *r)
{
    if(!pet_firmware_wire_identity_matches(o,r))return false;
    const pet_firmware_report_t *p=&o->report;
    pet_fw_phase_t phase=r->phase==PET_FW_STAGED?PET_FW_DOWNLOADING:r->phase;
    return phase==p->phase&&p->sequence==r->sequence&&p->downloaded_bytes==r->downloaded_bytes&&
        p->healthy_storage==r->healthy_storage&&!strcmp(p->boot_id,r->report_boot_id)&&
        !strcmp(p->running_sha256,r->running_sha256)&&!strcmp(p->error_code,r->error_code);
}

static bool add(cJSON *o,const char *name,cJSON *value)
{if(value&&o&&cJSON_AddItemToObject(o,name,value))return true;cJSON_Delete(value);return false;}
static cJSON *mask_array(uint8_t mask,unsigned min,unsigned max,unsigned scale)
{
    cJSON *a=cJSON_CreateArray();if(!a)return NULL;
    for(unsigned i=min;i<=max;++i)if(mask&(1u<<i)){
        cJSON *n=cJSON_CreateNumber(i*scale);if(!n||!cJSON_AddItemToArray(a,n)){cJSON_Delete(n);cJSON_Delete(a);return NULL;}
    }
    return a;
}
#define STR(o,n,v) add(o,n,cJSON_CreateString(v))
#define NUM(o,n,v) add(o,n,cJSON_CreateNumber(v))
#define TRUE(o,n) add(o,n,cJSON_CreateTrue())
static cJSON *layout_json(const pet_firmware_requirements_t *r)
{
    pet_flash_layout_t known;
    if(!r||r->layout.id<PET_LAYOUT_SINGLE_2M||!pet_flash_layout_known(r->layout.id,&known)||!sha(r->partition_sha256)||
       known.app_slot_bytes!=r->layout.app_slot_bytes||known.pet_partition_bytes!=r->layout.pet_partition_bytes||
       known.pack_capacity_bytes!=r->layout.pack_capacity_bytes)return NULL;
    cJSON *o=cJSON_CreateObject();
    if(!STR(o,"id",pet_flash_layout_name(known.id))||!STR(o,"partitionTableSha256",r->partition_sha256)||
       !NUM(o,"flashBytes",PET_FLASH_BYTES)||!NUM(o,"appSlotBytes",known.app_slot_bytes)||
       !NUM(o,"petPartitionBytes",known.pet_partition_bytes)||!NUM(o,"manifestReservationBytes",0x2000)||
       !NUM(o,"packCapacityBytes",known.pack_capacity_bytes)){cJSON_Delete(o);return NULL;}return o;
}
static cJSON *renderer_json(const pet_firmware_requirements_t *r)
{
    if(!r||!r->formats||(r->formats&~6)||!r->codecs)return NULL;
    cJSON *o=cJSON_CreateObject();
    if(!STR(o,"renderer","frame-player-cloud-0.4")||!add(o,"formats",mask_array(r->formats,1,2,1))||
       !add(o,"resolutions",mask_array(r->formats,1,2,120))||!add(o,"codecs",mask_array(r->codecs,0,7,1))||
       (r->imported_release&&!NUM(o,"importedRelease",1))){cJSON_Delete(o);return NULL;}
    return o;
}
static cJSON *bootloader_json(const pet_firmware_bootloader_t *b)
{
    if(!b->bytes)return cJSON_CreateNull();
    if(b->bytes<4096||b->bytes>0x8000||!sha(b->sha256))return NULL;
    cJSON *o=cJSON_CreateObject();if(!STR(o,"sha256",b->sha256)||!NUM(o,"bytes",b->bytes)||!TRUE(o,"rollback")){cJSON_Delete(o);return NULL;}
    return o;
}
static cJSON *pack_json(const pet_firmware_pack_identity_t *pack,const pet_firmware_requirements_t *current)
{
    if(!pack)return NULL;
    const pet_firmware_protected_pack_t *p=&pack->requirements;
    if(!p->present)return cJSON_CreateNull();
    if(!uuid(pack->build_id)||!sha(pack->sha256)||!sha(p->partition_sha256)||p->layout_id!=current->layout.id||
       strcmp(p->partition_sha256,current->partition_sha256)||!p->bytes||p->bytes>current->layout.pack_capacity_bytes||
       !p->minimum_firmware_epoch||(p->format!=1&&p->format!=2)||p->resolution_divisor!=p->format||!p->codecs||
       (p->format==1&&(p->codecs&0xe0)))return NULL;
    cJSON *requirements=cJSON_CreateObject();
    if(!STR(requirements,"hardware",pet_board_current()->hardware)||!STR(requirements,"chip","esp32s3")||
       !add(requirements,"layout",layout_json(current))||!NUM(requirements,"formatVersion",p->format)||
       !NUM(requirements,"resolution",120*p->resolution_divisor)||!STR(requirements,"renderer","frame-player-cloud-0.4")||
       !add(requirements,"codecs",mask_array(p->codecs,0,7,1))||!NUM(requirements,"minimumFirmwareEpoch",p->minimum_firmware_epoch)||
       !STR(requirements,"updateMode","single-slot-replace-v2")||
       (p->imported_release&&!NUM(requirements,"importedRelease",1))){cJSON_Delete(requirements);return NULL;}
    cJSON *o=cJSON_CreateObject();
    if(!add(o,"requirements",requirements)){cJSON_Delete(o);return NULL;}
    if(!STR(o,"buildId",pack->build_id)||!STR(o,"sha256",pack->sha256)||!NUM(o,"bytes",p->bytes)){cJSON_Delete(o);return NULL;}return o;
}
/* Single-pet devices keep exactly {known,active,interrupted}; a three-pet
 * device lists its other pets (at most one per slot, as the producer and
 * pet_firmware_release_compatible require). */
static cJSON *protection_json(const pet_firmware_protection_t *p,const pet_firmware_requirements_t *current)
{
    const unsigned count=p->installed_count;
    cJSON *o=cJSON_CreateObject();
    if(!add(o,"known",cJSON_CreateBool(p->known))){cJSON_Delete(o);return NULL;}
    if(!p->known)return o;
    if(count>PET_FIRMWARE_INSTALLED_MAX||
       !add(o,"active",pack_json(&p->active,current))||!add(o,"interrupted",pack_json(&p->interrupted,current))){cJSON_Delete(o);return NULL;}
    if(current->layout.pet_slots!=PET_LAYOUT_THREE_SLOTS)return o;
    cJSON *installed=cJSON_CreateArray();
    if(!add(o,"installed",installed)){cJSON_Delete(o);return NULL;}
    for(unsigned i=0;i<count;++i){
        cJSON *pack=p->installed[i].requirements.present?pack_json(&p->installed[i],current):NULL;
        if(!pack||!cJSON_AddItemToArray(installed,pack)){cJSON_Delete(pack);cJSON_Delete(o);return NULL;}
    }
    return o;
}
static bool print_json(cJSON *root,char *json,size_t capacity)
{
    bool ok=root&&json&&capacity>0&&capacity<=8192&&cJSON_PrintPreallocated(root,json,(int)capacity,false);
    cJSON_Delete(root);if(!ok&&json&&capacity)json[0]=0;return ok;
}
bool pet_firmware_wire_encode_poll(const pet_firmware_requirements_t *r,const char *running,const char *boot,
                                   const pet_firmware_protection_t *packs,char *json,size_t capacity)
{
    if(json&&capacity)json[0]=0;
    if(!r||!r->firmware_epoch||!sha(running)||!uuid(boot)||!packs)return false;
    cJSON *caps=cJSON_CreateObject();
    if(!NUM(caps,"version",2)||!STR(caps,"hardware",pet_board_current()->hardware)||!STR(caps,"chip","esp32s3")||
       !add(caps,"layout",layout_json(r))||!NUM(caps,"firmwareEpoch",r->firmware_epoch)||!STR(caps,"firmwareSha256",running)||
       !add(caps,"renderer",renderer_json(r))||!add(caps,"bootloader",bootloader_json(&r->bootloader))||!STR(caps,"petUpdateMode","single-slot-replace-v2")||
       !STR(caps,"firmwareUpdateMode","dual-app-ota-v1")||!TRUE(caps,"independentControl")){cJSON_Delete(caps);return false;}
#ifdef ESP_PLATFORM
    /* Same base eFuse identity read by the USB ROM loader. This is public
     * correlation data inside an authenticated control report, not a credential. */
    uint8_t mac[6];char usb_mac[18];
    if(esp_efuse_mac_get_default(mac)!=ESP_OK){cJSON_Delete(caps);return false;}
    snprintf(usb_mac,sizeof(usb_mac),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    if(!STR(caps,"usbMac",usb_mac)){cJSON_Delete(caps);return false;}
#endif
    cJSON *protection=protection_json(packs,r);
    if(!protection){cJSON_Delete(caps);return false;}
    cJSON *root=cJSON_CreateObject();
    if(!add(root,"capabilities",caps)){cJSON_Delete(protection);cJSON_Delete(root);return false;}
    if(!add(root,"protection",protection)||!NUM(root,"version",2)||!STR(root,"bootId",boot)){cJSON_Delete(root);return false;}
    return print_json(root,json,capacity);
}
bool pet_firmware_wire_encode_report(const pet_firmware_receipt_t *r,char *json,size_t capacity)
{
    if(json&&capacity)json[0]=0;
    const char *status=pet_firmware_receipt_status(r);if(!status)return false;
    cJSON *health=r->phase==PET_FW_HEALTHY?cJSON_CreateObject():cJSON_CreateNull();
    if(r->phase==PET_FW_HEALTHY&&(!TRUE(health,"uiReady")||!STR(health,"storage",storage_name(r->healthy_storage))||
       !TRUE(health,"wifiConnected")||!TRUE(health,"controlAuthenticated"))){cJSON_Delete(health);return false;}
    cJSON *root=cJSON_CreateObject();
    if(!add(root,"health",health)||!NUM(root,"version",2)||!STR(root,"operationId",r->operation_id)||
       !STR(root,"releaseId",r->release_id)||!STR(root,"sha256",r->target_sha256)||!NUM(root,"sequence",r->sequence)||
       !STR(root,"status",status)||!NUM(root,"downloadedBytes",r->downloaded_bytes)||!STR(root,"bootId",r->report_boot_id)||
       !STR(root,"runningSha256",r->running_sha256)||!add(root,"errorCode",r->error_code[0]?cJSON_CreateString(r->error_code):cJSON_CreateNull())){
        cJSON_Delete(root);return false;
    }return print_json(root,json,capacity);
}
bool pet_firmware_wire_encode_recovery_poll(const char *boot,char *json,size_t capacity)
{
    if(json&&capacity)json[0]=0;
    if(!uuid(boot))return false;
    cJSON *protection=cJSON_CreateObject();
    if(!add(protection,"known",cJSON_CreateFalse())){cJSON_Delete(protection);return false;}
    cJSON *root=cJSON_CreateObject();
    if(!add(root,"protection",protection)||!NUM(root,"version",2)||!STR(root,"bootId",boot)||
       !add(root,"capabilities",cJSON_CreateNull())){cJSON_Delete(root);return false;}
    return print_json(root,json,capacity);
}
