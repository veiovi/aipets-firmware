#include "pet_firmware_receipt.h"
#include <string.h>

static bool bounded(const char *s,size_t capacity)
{if(!s)return false;for(size_t i=0;i<capacity;++i)if(!s[i])return true;return false;}
static bool sha(const char *s)
{return bounded(s,65)&&strlen(s)==64&&strspn(s,"0123456789abcdef")==64;}
static bool uuid(const char *s)
{
    if(!bounded(s,37)||strlen(s)!=36)return false;
    for(unsigned i=0;i<36;++i){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}
        else if(!strchr("0123456789abcdef",s[i]))return false;}
    return s[14]>='1'&&s[14]<='5'&&strchr("89ab",s[19]);
}
static bool terminal(pet_fw_phase_t phase)
{return phase==PET_FW_HEALTHY||phase==PET_FW_FAILED||phase==PET_FW_ROLLED_BACK;}
static bool error_code(const char *s)
{return bounded(s,81)&&s[0]>='A'&&s[0]<='Z'&&strspn(s,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")==strlen(s);}
static bool empty(const pet_firmware_receipt_t *r)
{
    return r->phase==PET_FW_EMPTY&&!r->image_bytes&&!r->downloaded_bytes&&!r->sequence&&!r->acknowledged_sequence&&
        !r->firmware_epoch&&!r->target_offset&&!r->layout_id&&!r->image_verified&&!r->healthy_storage&&
        !r->operation_id[0]&&!r->release_id[0]&&!r->target_sha256[0]&&!r->previous_sha256[0]&&!r->previous_boot_id[0]&&
        !r->report_boot_id[0]&&!r->running_sha256[0]&&!r->partition_sha256[0]&&!r->firmware_version[0]&&!r->error_code[0];
}
bool pet_firmware_receipt_valid(const pet_firmware_receipt_t *r)
{
    if(!r)return false;
    if(r->phase==PET_FW_EMPTY)return empty(r);
    pet_flash_layout_t layout;
    if(r->phase<PET_FW_DOWNLOADING||r->phase>PET_FW_ROLLED_BACK||!r->sequence||r->acknowledged_sequence>r->sequence||
       r->layout_id<PET_LAYOUT_SINGLE_2M||!pet_flash_layout_known(r->layout_id,&layout)||
       !r->image_bytes||(uint64_t)r->image_bytes*100>(uint64_t)layout.app_slot_bytes*85||
       r->downloaded_bytes>r->image_bytes||!r->firmware_epoch||
       (r->target_offset!=0x30000&&r->target_offset!=0x30000+layout.app_slot_bytes)||
       !uuid(r->operation_id)||!uuid(r->release_id)||!uuid(r->previous_boot_id)||!uuid(r->report_boot_id)||
       !sha(r->target_sha256)||!sha(r->previous_sha256)||!sha(r->running_sha256)||!sha(r->partition_sha256)||
       !bounded(r->firmware_version,sizeof(r->firmware_version))||!r->firmware_version[0]||
       !bounded(r->error_code,sizeof(r->error_code)))return false;
    if(r->phase==PET_FW_STAGED||r->phase==PET_FW_REBOOTING||r->phase==PET_FW_HEALTHY){
        if(!r->image_verified||r->downloaded_bytes!=r->image_bytes)return false;
    }
    if(r->image_verified&&r->downloaded_bytes!=r->image_bytes)return false;
    if(r->phase==PET_FW_DOWNLOADING&&r->image_verified)return false;
    if(r->phase==PET_FW_HEALTHY){
        if(r->healthy_storage<PET_FW_STORAGE_PACK||r->healthy_storage>PET_FW_STORAGE_RECOVERY||
           strcmp(r->target_sha256,r->running_sha256)||!strcmp(r->report_boot_id,r->previous_boot_id))return false;
    }else if(r->healthy_storage!=PET_FW_STORAGE_NONE)return false;
    if((r->phase==PET_FW_DOWNLOADING||r->phase==PET_FW_STAGED||r->phase==PET_FW_REBOOTING||r->phase==PET_FW_ROLLED_BACK)&&
       strcmp(r->previous_sha256,r->running_sha256))return false;
    if(r->phase==PET_FW_ROLLED_BACK&&!strcmp(r->report_boot_id,r->previous_boot_id))return false;
    return r->phase==PET_FW_FAILED||r->phase==PET_FW_ROLLED_BACK?error_code(r->error_code):!r->error_code[0];
}
static bool commit_value(pet_firmware_receipt_t *out,const pet_firmware_receipt_t *next)
{if(!pet_firmware_receipt_valid(next))return false;*out=*next;return true;}
bool pet_firmware_receipt_begin(pet_firmware_receipt_t *r,const pet_firmware_release_t *release,
                                const char *operation,const char *previous_sha,const char *boot,uint32_t offset)
{return pet_firmware_receipt_begin_at_boot(r,release,operation,previous_sha,boot,boot,offset);}
bool pet_firmware_receipt_begin_at_boot(pet_firmware_receipt_t *r,const pet_firmware_release_t *release,
                                        const char *operation,const char *previous_sha,const char *previous_boot,const char *current_boot,uint32_t offset)
{
    if(!r||!release||!pet_firmware_receipt_valid(r)||
       !(r->phase==PET_FW_EMPTY||(terminal(r->phase)&&r->acknowledged_sequence==r->sequence))||
       !uuid(operation)||!strcmp(r->operation_id,operation)||!uuid(previous_boot)||!uuid(current_boot)||!sha(previous_sha)||!uuid(release->release_id)||!sha(release->sha256)||
       !sha(release->requirements.partition_sha256)||!bounded(release->version,sizeof(release->version)))return false;
    pet_firmware_receipt_t next={.phase=PET_FW_DOWNLOADING,.image_bytes=release->bytes,.sequence=1,
        .firmware_epoch=release->requirements.firmware_epoch,.target_offset=offset,.layout_id=release->requirements.layout.id};
    strcpy(next.operation_id,operation);strcpy(next.release_id,release->release_id);strcpy(next.target_sha256,release->sha256);
    strcpy(next.previous_sha256,previous_sha);strcpy(next.running_sha256,previous_sha);strcpy(next.previous_boot_id,previous_boot);
    strcpy(next.report_boot_id,current_boot);strcpy(next.partition_sha256,release->requirements.partition_sha256);strcpy(next.firmware_version,release->version);
    return commit_value(r,&next);
}
bool pet_firmware_receipt_progress(pet_firmware_receipt_t *r,uint32_t downloaded,const char *boot)
{
    if(!pet_firmware_receipt_valid(r)||r->phase!=PET_FW_DOWNLOADING||!r->acknowledged_sequence||
       downloaded<r->downloaded_bytes||downloaded>r->image_bytes||!uuid(boot)||r->sequence==UINT32_MAX)return false;
    pet_firmware_receipt_t next=*r;next.downloaded_bytes=downloaded;strcpy(next.report_boot_id,boot);++next.sequence;
    return commit_value(r,&next);
}
bool pet_firmware_receipt_stage(pet_firmware_receipt_t *r,const char *verified_sha)
{
    if(!pet_firmware_receipt_valid(r)||r->phase!=PET_FW_DOWNLOADING||!r->acknowledged_sequence||
       r->downloaded_bytes!=r->image_bytes||!sha(verified_sha)||strcmp(verified_sha,r->target_sha256))return false;
    pet_firmware_receipt_t next=*r;next.phase=PET_FW_STAGED;next.image_verified=true;return commit_value(r,&next);
}
bool pet_firmware_receipt_reboot(pet_firmware_receipt_t *r)
{return r&&pet_firmware_receipt_reboot_at_boot(r,r->report_boot_id);}
bool pet_firmware_receipt_reboot_at_boot(pet_firmware_receipt_t *r,const char *boot)
{
    if(!pet_firmware_receipt_valid(r)||r->phase!=PET_FW_STAGED||r->acknowledged_sequence!=r->sequence||r->sequence==UINT32_MAX||!uuid(boot))return false;
    pet_firmware_receipt_t next=*r;next.phase=PET_FW_REBOOTING;strcpy(next.report_boot_id,boot);++next.sequence;return commit_value(r,&next);
}
bool pet_firmware_receipt_healthy(pet_firmware_receipt_t *r,const char *boot,const char *running,
                                 bool ui,bool wifi,bool control,bool application_validated,pet_fw_storage_t storage)
{
    if(!pet_firmware_receipt_valid(r)||(r->phase!=PET_FW_STAGED&&r->phase!=PET_FW_REBOOTING)||
       !ui||!wifi||!control||!application_validated||!uuid(boot)||!sha(running)||r->sequence==UINT32_MAX)return false;
    pet_firmware_receipt_t next=*r;next.phase=PET_FW_HEALTHY;next.healthy_storage=storage;
    strcpy(next.report_boot_id,boot);strcpy(next.running_sha256,running);++next.sequence;return commit_value(r,&next);
}
bool pet_firmware_receipt_fail(pet_firmware_receipt_t *r,bool rollback,const char *boot,const char *running,const char *error)
{
    if(!pet_firmware_receipt_valid(r)||r->phase==PET_FW_EMPTY||terminal(r->phase)||!uuid(boot)||!sha(running)||
       !error_code(error)||r->sequence==UINT32_MAX||!r->acknowledged_sequence)return false;
    pet_firmware_receipt_t next=*r;next.phase=rollback?PET_FW_ROLLED_BACK:PET_FW_FAILED;
    strcpy(next.report_boot_id,boot);strcpy(next.running_sha256,running);strcpy(next.error_code,error);++next.sequence;
    return commit_value(r,&next);
}
bool pet_firmware_receipt_ack(pet_firmware_receipt_t *r,uint32_t sequence)
{
    if(!pet_firmware_receipt_valid(r)||!sequence||sequence!=r->sequence)return false;
    r->acknowledged_sequence=sequence;return true;
}
const char *pet_firmware_receipt_status(const pet_firmware_receipt_t *r)
{
    if(!pet_firmware_receipt_valid(r))return NULL;
    switch(r->phase){case PET_FW_DOWNLOADING:case PET_FW_STAGED:return "downloading";
      case PET_FW_REBOOTING:return "rebooting";case PET_FW_HEALTHY:return "healthy";
      case PET_FW_FAILED:return "failed";case PET_FW_ROLLED_BACK:return "rolled_back";default:return NULL;}
}

static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t get32(const uint8_t *p){uint32_t n=0;for(unsigned i=0;i<4;++i)n|=(uint32_t)p[i]<<(8*i);return n;}
static void put64(uint8_t *p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static uint64_t get64(const uint8_t *p){return get32(p)|((uint64_t)get32(p+4)<<32);}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
static bool encode(const pet_firmware_receipt_t *v,uint64_t generation,uint8_t out[PET_FIRMWARE_RECEIPT_BYTES])
{
    if(!generation||!pet_firmware_receipt_valid(v))return false;
    memset(out,0,PET_FIRMWARE_RECEIPT_BYTES);memcpy(out,"FWR2",4);put32(out+4,PET_FIRMWARE_RECEIPT_BYTES);put64(out+8,generation);
    const uint32_t fields[]={v->phase,v->image_bytes,v->downloaded_bytes,v->sequence,v->acknowledged_sequence,
        v->firmware_epoch,v->target_offset,v->layout_id,v->image_verified,v->healthy_storage};
    size_t at=16;for(unsigned i=0;i<10;++i){put32(out+at,fields[i]);at+=4;}
#define SAVE_TEXT(name) do {memcpy(out+at,v->name,strlen(v->name));at+=sizeof(v->name);} while(0)
    SAVE_TEXT(operation_id);SAVE_TEXT(release_id);SAVE_TEXT(target_sha256);SAVE_TEXT(previous_sha256);
    SAVE_TEXT(previous_boot_id);SAVE_TEXT(report_boot_id);SAVE_TEXT(running_sha256);SAVE_TEXT(partition_sha256);
    SAVE_TEXT(firmware_version);SAVE_TEXT(error_code);
#undef SAVE_TEXT
    put32(out+PET_FIRMWARE_RECEIPT_BYTES-4,crc(out,PET_FIRMWARE_RECEIPT_BYTES-4));return true;
}
static bool decode(const uint8_t in[PET_FIRMWARE_RECEIPT_BYTES],pet_firmware_receipt_t *out,uint64_t *generation)
{
    if(memcmp(in,"FWR2",4)||get32(in+4)!=PET_FIRMWARE_RECEIPT_BYTES||!get64(in+8)||
       get32(in+PET_FIRMWARE_RECEIPT_BYTES-4)!=crc(in,PET_FIRMWARE_RECEIPT_BYTES-4))return false;
    uint32_t fields[10];size_t at=16;for(unsigned i=0;i<10;++i){fields[i]=get32(in+at);at+=4;}
    if(fields[0]>PET_FW_ROLLED_BACK||fields[7]>PET_LAYOUT_SINGLE_4M||fields[8]>1||fields[9]>PET_FW_STORAGE_RECOVERY)return false;
    pet_firmware_receipt_t v={.phase=(pet_fw_phase_t)fields[0],.image_bytes=fields[1],.downloaded_bytes=fields[2],
        .sequence=fields[3],.acknowledged_sequence=fields[4],.firmware_epoch=fields[5],.target_offset=fields[6],
        .layout_id=(pet_layout_id_t)fields[7],.image_verified=fields[8]!=0,.healthy_storage=(pet_fw_storage_t)fields[9]};
#define LOAD_TEXT(name) do {memcpy(v.name,in+at,sizeof(v.name));at+=sizeof(v.name);if(!bounded(v.name,sizeof(v.name)))return false;} while(0)
    LOAD_TEXT(operation_id);LOAD_TEXT(release_id);LOAD_TEXT(target_sha256);LOAD_TEXT(previous_sha256);
    LOAD_TEXT(previous_boot_id);LOAD_TEXT(report_boot_id);LOAD_TEXT(running_sha256);LOAD_TEXT(partition_sha256);
    LOAD_TEXT(firmware_version);LOAD_TEXT(error_code);
#undef LOAD_TEXT
    uint8_t canonical[PET_FIRMWARE_RECEIPT_BYTES];
    if(!encode(&v,get64(in+8),canonical)||memcmp(canonical,in,sizeof(canonical)))return false;
    *out=v;*generation=get64(in+8);return true;
}
static bool same_value(const pet_firmware_receipt_t *a,const pet_firmware_receipt_t *b)
{
    uint8_t left[PET_FIRMWARE_RECEIPT_BYTES],right[PET_FIRMWARE_RECEIPT_BYTES];
    return encode(a,1,left)&&encode(b,1,right)&&!memcmp(left,right,sizeof(left));
}
/* The storage boundary accepts one legal transition, not an arbitrary valid
 * struct. This also makes acknowledgement/progress/identity regressions fail
 * before any NVS write, even if a future caller omits a pure-state helper. */
static bool legal_transition(const pet_firmware_receipt_t *from,const pet_firmware_receipt_t *to)
{
    if(!pet_firmware_receipt_valid(from)||!pet_firmware_receipt_valid(to))return false;
    if(same_value(from,to))return true;
    pet_firmware_receipt_t candidate=*from;
    if(strcmp(from->operation_id,to->operation_id)){
        pet_firmware_release_t release={.bytes=to->image_bytes};
        strcpy(release.release_id,to->release_id);strcpy(release.sha256,to->target_sha256);
        strcpy(release.version,to->firmware_version);strcpy(release.requirements.partition_sha256,to->partition_sha256);
        release.requirements.firmware_epoch=to->firmware_epoch;
        if(!pet_flash_layout_known(to->layout_id,&release.requirements.layout))return false;
        return pet_firmware_receipt_begin_at_boot(&candidate,&release,to->operation_id,to->previous_sha256,to->previous_boot_id,to->report_boot_id,to->target_offset)&&
            same_value(&candidate,to);
    }
#define TRY_TRANSITION(call) do {candidate=*from;if((call)&&same_value(&candidate,to))return true;} while(0)
    TRY_TRANSITION(pet_firmware_receipt_ack(&candidate,to->acknowledged_sequence));
    TRY_TRANSITION(pet_firmware_receipt_progress(&candidate,to->downloaded_bytes,to->report_boot_id));
    TRY_TRANSITION(pet_firmware_receipt_stage(&candidate,to->target_sha256));
    TRY_TRANSITION(pet_firmware_receipt_reboot_at_boot(&candidate,to->report_boot_id));
    TRY_TRANSITION(pet_firmware_receipt_healthy(&candidate,to->report_boot_id,to->running_sha256,true,true,true,true,to->healthy_storage));
    if(to->phase==PET_FW_FAILED||to->phase==PET_FW_ROLLED_BACK)
        TRY_TRANSITION(pet_firmware_receipt_fail(&candidate,to->phase==PET_FW_ROLLED_BACK,to->report_boot_id,to->running_sha256,to->error_code));
#undef TRY_TRANSITION
    return false;
}
static bool write_verified(pet_firmware_receipt_store_t *s,unsigned slot,const uint8_t record[PET_FIRMWARE_RECEIPT_BYTES])
{
    uint8_t actual[PET_FIRMWARE_RECEIPT_BYTES];
    return s->io.write(s->io.context,slot,record)&&s->io.read(s->io.context,slot,actual)==1&&!memcmp(record,actual,sizeof(actual));
}
bool pet_firmware_receipt_open(pet_firmware_receipt_store_t *s,const pet_firmware_receipt_io_t *io)
{
    if(!s||!io||!io->read||!io->write)return false;
    pet_firmware_receipt_io_t callbacks=*io;memset(s,0,sizeof(*s));s->io=callbacks;
    uint8_t records[2][PET_FIRMWARE_RECEIPT_BYTES];pet_firmware_receipt_t values[2];uint64_t generations[2]={0};int read[2];bool valid[2];
    for(unsigned i=0;i<2;++i){read[i]=callbacks.read(callbacks.context,i,records[i]);valid[i]=read[i]==1&&decode(records[i],&values[i],&generations[i]);}
    if(read[0]<0||read[1]<0)return false;
    if(!valid[0]&&!valid[1]){
        if(read[0]!=0||read[1]!=0||!encode(&s->value,1,records[0])||
           !write_verified(s,0,records[0])||!write_verified(s,1,records[0]))return false;
        s->generation=1;s->slot=1;s->loaded=true;return true;
    }
    if(valid[0]&&valid[1]&&generations[0]==generations[1]&&memcmp(records[0],records[1],sizeof(records[0])))return false;
    unsigned slot=valid[1]&&(!valid[0]||generations[1]>=generations[0])?1:0;
    s->value=values[slot];s->generation=generations[slot];s->slot=slot;s->loaded=true;return true;
}
bool pet_firmware_receipt_save(pet_firmware_receipt_store_t *s,const pet_firmware_receipt_t *next)
{
    if(!s||!s->loaded||!next||s->generation==UINT64_MAX||!legal_transition(&s->value,next))return false;
    uint8_t encoded[PET_FIRMWARE_RECEIPT_BYTES];
    if(!encode(next,s->generation+1,encoded))return false;
    unsigned slot=s->slot^1u;
    if(!write_verified(s,slot,encoded)){s->loaded=false;return false;}
    s->value=*next;++s->generation;s->slot=slot;return true;
}
