#include "pet_firmware_release.h"
#include "pet_board.h"
#include <string.h>
#include "cJSON.h"

static const cJSON *field(const cJSON *object,const char *name)
{return cJSON_GetObjectItemCaseSensitive(object,name);}
static bool keys_only(const cJSON *object,const char *names,unsigned count)
{
    if(!cJSON_IsObject(object)||cJSON_GetArraySize(object)!=(int)count)return false;
    for(const cJSON *item=object->child;item;item=item->next){
        bool found=false;
        for(const char *p=names;*p;){
            const char *end=strchr(p,'|');size_t n=end?(size_t)(end-p):strlen(p);
            if(strlen(item->string)==n&&!memcmp(p,item->string,n)){found=true;break;}
            if(!end)break;
            p=end+1;
        }
        if(!found)return false;
    }
    return true;
}
static bool equal(const cJSON *object,const char *name,const char *value)
{const cJSON *v=field(object,name);return cJSON_IsString(v)&&!strcmp(v->valuestring,value);}
static bool text(const cJSON *object,const char *name,char *out,size_t capacity)
{
    const cJSON *v=field(object,name);
    if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=capacity)return false;
    strcpy(out,v->valuestring);return true;
}
static bool hash(const char *value,size_t size)
{
    size_t length=0;while(length<size&&value[length])++length;
    return length==size-1&&strspn(value,"0123456789abcdef")==length;
}
static bool timestamp(const char *s)
{
    const size_t n=strlen(s);
    if(n<20||n>40||s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':')return false;
    unsigned parts[6]={0};const unsigned starts[]={0,5,8,11,14,17},lengths[]={4,2,2,2,2,2};
    for(unsigned i=0;i<6;++i)for(unsigned j=0;j<lengths[i];++j){
        char c=s[starts[i]+j];if(c<'0'||c>'9')return false;parts[i]=10*parts[i]+(unsigned)(c-'0');
    }
    const unsigned y=parts[0],month=parts[1],day=parts[2];
    const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(month<1||month>12||day<1||day>days[month-1]+(month==2&&y%4==0&&(y%100!=0||y%400==0))||
       parts[3]>23||parts[4]>59||parts[5]>59)return false;
    size_t i=19;
    if(s[i]=='.'){size_t first=++i;while(i<n&&s[i]>='0'&&s[i]<='9')++i;if(i==first)return false;}
    if(i==n-1&&s[i]=='Z')return true;
    if(n-i!=6||(s[i]!='+'&&s[i]!='-')||s[i+3]!=':')return false;
    if(s[i+1]<'0'||s[i+1]>'2'||s[i+2]<'0'||s[i+2]>'9'||s[i+4]<'0'||s[i+4]>'5'||s[i+5]<'0'||s[i+5]>'9')return false;
    return (s[i+1]-'0')*10+s[i+2]-'0'<=23;
}
static bool uuid(const char *value)
{
    if(strlen(value)!=36)return false;
    for(unsigned i=0;i<36;++i){
        if(i==8||i==13||i==18||i==23){if(value[i]!='-')return false;}
        else if(!strchr("0123456789abcdef",value[i]))return false;
    }
    return value[14]>='1'&&value[14]<='5'&&strchr("89ab",value[19]);
}
static bool number(const cJSON *object,const char *name,uint32_t min,uint32_t max,uint32_t *out)
{
    const cJSON *v=field(object,name);
    if(!cJSON_IsNumber(v)||!(v->valuedouble>=min&&v->valuedouble<=max))return false;
    uint32_t n=(uint32_t)v->valuedouble;if(v->valuedouble!=n)return false;*out=n;return true;
}
static bool exact_number(const cJSON *object,const char *name,uint32_t expected)
{uint32_t n;return number(object,name,expected,expected,&n);}
static bool mask(const cJSON *object,const char *name,unsigned min,unsigned max,uint8_t *out)
{
    const cJSON *array=field(object,name);uint8_t value=0;
    if(!cJSON_IsArray(array)||cJSON_GetArraySize(array)<1||cJSON_GetArraySize(array)>(int)(max-min+1))return false;
    for(const cJSON *item=array->child;item;item=item->next){
        if(!cJSON_IsNumber(item)||!(item->valuedouble>=min&&item->valuedouble<=max))return false;
        unsigned n=(unsigned)item->valuedouble;
        if(item->valuedouble!=n||(value&(1u<<n)))return false;
        value|=(uint8_t)(1u<<n);
    }
    *out=value;return true;
}
static bool parse_layout(const cJSON *object,pet_firmware_requirements_t *out)
{
    if(!keys_only(object,"id|partitionTableSha256|flashBytes|appSlotBytes|petPartitionBytes|manifestReservationBytes|packCapacityBytes",7))return false;
    bool found=false;
    /* Firmware is built for one physical layout, single-pet or three-pet;
     * a release for another layout never matches the device's table. */
    for(unsigned id=PET_LAYOUT_SINGLE_2M;id<=PET_LAYOUT_THREE_3P5M;++id)
        if(equal(object,"id",pet_flash_layout_name((pet_layout_id_t)id))){
            found=pet_flash_layout_known((pet_layout_id_t)id,&out->layout);break;
        }
    return found&&text(object,"partitionTableSha256",out->partition_sha256,sizeof(out->partition_sha256))&&
        hash(out->partition_sha256,sizeof(out->partition_sha256))&&exact_number(object,"flashBytes",PET_FLASH_BYTES)&&
        exact_number(object,"appSlotBytes",out->layout.app_slot_bytes)&&
        exact_number(object,"petPartitionBytes",out->layout.pet_partition_bytes)&&
        exact_number(object,"manifestReservationBytes",0x2000)&&
        exact_number(object,"packCapacityBytes",out->layout.pack_capacity_bytes);
}
static bool parse_renderer(const cJSON *object,pet_firmware_requirements_t *out)
{
    out->imported_release=field(object,"importedRelease")!=NULL; /* Optional, exactly 1. */
    if(!keys_only(object,"renderer|formats|resolutions|codecs|importedRelease",out->imported_release?5:4)||
       (out->imported_release&&!exact_number(object,"importedRelease",1))||
       !equal(object,"renderer","frame-player-cloud-0.4")||!mask(object,"formats",1,2,&out->formats)||
       !mask(object,"codecs",0,7,&out->codecs))return false;
    const cJSON *resolutions=field(object,"resolutions");uint8_t formats=0;
    if(!cJSON_IsArray(resolutions)||cJSON_GetArraySize(resolutions)<1||cJSON_GetArraySize(resolutions)>2)return false;
    for(const cJSON *v=resolutions->child;v;v=v->next){
        uint8_t bit=cJSON_IsNumber(v)&&v->valuedouble==120?2:cJSON_IsNumber(v)&&v->valuedouble==240?4:0;
        if(!bit||(formats&bit))return false;
        formats|=bit;
    }
    return formats==out->formats;
}
static bool parse_bootloader(const cJSON *object,pet_firmware_bootloader_t *out)
{
    return keys_only(object,"sha256|bytes|rollback",3)&&cJSON_IsTrue(field(object,"rollback"))&&
        number(object,"bytes",4096,0x8000,&out->bytes)&&text(object,"sha256",out->sha256,sizeof(out->sha256))&&hash(out->sha256,sizeof(out->sha256));
}
bool pet_firmware_release_verify(const cJSON *envelope,const pet_pack_trust_key_t *trust,
                                 size_t key_count,pet_firmware_release_t *out)
{
    if(!out||!keys_only(envelope,"algorithm|keyId|payload|signature",4)||!equal(envelope,"algorithm","ES256"))return false;
    const cJSON *payload_text=field(envelope,"payload"),*key_id=field(envelope,"keyId"),*signature=field(envelope,"signature");
    if(!cJSON_IsString(payload_text)||!cJSON_IsString(key_id)||!cJSON_IsString(signature)||
       strlen(payload_text->valuestring)>7000||strlen(key_id->valuestring)>80||strlen(signature->valuestring)!=86||
       strspn(key_id->valuestring,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")!=strlen(key_id->valuestring)||
       !pet_release_verify_signature(payload_text->valuestring,strlen(payload_text->valuestring)+1,
          key_id->valuestring,strlen(key_id->valuestring)+1,signature->valuestring,87,trust,key_count))return false;
    cJSON *payload=pet_control_json(payload_text->valuestring,strlen(payload_text->valuestring),7000);
    pet_firmware_release_t parsed={0};char created_at[81];
    bool ok=keys_only(payload,"kind|version|releaseId|createdAt|generic|hardware|chip|petUpdateMode|firmwareUpdateMode|firmwareVersion|firmwareEpoch|appSha256|appBytes|layout|renderer|bootloader|independentControl|artifactManifestSha256|sourceCommit",19)&&
        equal(payload,"kind","private-firmware-release")&&exact_number(payload,"version",2)&&
        cJSON_IsTrue(field(payload,"generic"))&&cJSON_IsTrue(field(payload,"independentControl"))&&
        equal(payload,"hardware",pet_board_current()->hardware)&&equal(payload,"chip","esp32s3")&&
        equal(payload,"petUpdateMode","single-slot-replace-v2")&&equal(payload,"firmwareUpdateMode","dual-app-ota-v1")&&
        text(payload,"releaseId",parsed.release_id,sizeof(parsed.release_id))&&uuid(parsed.release_id)&&
        text(payload,"firmwareVersion",parsed.version,sizeof(parsed.version))&&
        text(payload,"createdAt",created_at,sizeof(created_at))&&timestamp(created_at)&&
        number(payload,"firmwareEpoch",1,UINT32_MAX,&parsed.requirements.firmware_epoch)&&
        number(payload,"appBytes",1,0x400000,&parsed.bytes)&&
        text(payload,"appSha256",parsed.sha256,sizeof(parsed.sha256))&&hash(parsed.sha256,sizeof(parsed.sha256))&&
        text(payload,"artifactManifestSha256",parsed.artifact_manifest_sha256,sizeof(parsed.artifact_manifest_sha256))&&
        hash(parsed.artifact_manifest_sha256,sizeof(parsed.artifact_manifest_sha256))&&
        text(payload,"sourceCommit",parsed.source_commit,sizeof(parsed.source_commit))&&hash(parsed.source_commit,sizeof(parsed.source_commit))&&
        parse_layout(field(payload,"layout"),&parsed.requirements)&&parse_renderer(field(payload,"renderer"),&parsed.requirements)&&
        parse_bootloader(field(payload,"bootloader"),&parsed.requirements.bootloader)&&
        (uint64_t)parsed.bytes*100<=(uint64_t)parsed.requirements.layout.app_slot_bytes*85&&
        (parsed.requirements.layout.pet_slots!=PET_LAYOUT_THREE_SLOTS||parsed.requirements.firmware_epoch>=3); /* As the cloud. */
    cJSON_Delete(payload);if(ok)*out=parsed;return ok;
}
static bool protects(const pet_firmware_release_t *release,const pet_firmware_protected_pack_t *pack)
{
    if(!pack)return false;
    if(!pack->present)return true;
    const pet_firmware_requirements_t *r=&release->requirements;
    if(pack->format!=1&&pack->format!=2)return false;
    return pack->layout_id==r->layout.id&&hash(pack->partition_sha256,sizeof(pack->partition_sha256))&&
        !strcmp(pack->partition_sha256,r->partition_sha256)&&pack->bytes>0&&pack->bytes<=r->layout.pack_capacity_bytes&&
        pack->minimum_firmware_epoch>0&&pack->minimum_firmware_epoch<=r->firmware_epoch&&
        pack->resolution_divisor==pack->format&&(r->formats&(1u<<pack->format))&&pack->codecs&&
        !(pack->codecs&~r->codecs)&&(pack->format!=1||!(pack->codecs&0xe0))&&(!pack->imported_release||r->imported_release);
}
/* A single-pet layout has its one pack active or interrupted; a three-pet
 * layout lists every other installed pet too, at most one pet per slot. */
static bool protects_all(const pet_firmware_release_t *release,const pet_flash_layout_t *physical,
                         const pet_firmware_protection_t *p)
{
    const unsigned slots=physical->pet_slots==PET_LAYOUT_THREE_SLOTS?PET_LAYOUT_THREE_SLOTS:0,count=p->installed_count;
    if(count>slots||count+p->active.requirements.present+p->interrupted.requirements.present>(slots?slots:2)||
       !protects(release,&p->active.requirements)||!protects(release,&p->interrupted.requirements))return false;
    for(unsigned i=0;i<count;++i)
        if(!p->installed[i].requirements.present||!protects(release,&p->installed[i].requirements))return false;
    return true;
}
bool pet_firmware_release_compatible(const pet_firmware_release_t *release,const pet_flash_layout_t *physical,
                                     const char *partition_sha256,const pet_firmware_bootloader_t *bootloader,
                                     const pet_firmware_protection_t *protection)
{
    if(!release||!physical||!partition_sha256||!bootloader||bootloader->bytes<4096||bootloader->bytes>0x8000||
       !hash(bootloader->sha256,sizeof(bootloader->sha256))||!protection||!protection->known||!hash(partition_sha256,65)||
       !hash(release->requirements.partition_sha256,sizeof(release->requirements.partition_sha256)))return false;
    const pet_firmware_requirements_t *r=&release->requirements;
    return r->layout.id==physical->id&&!strcmp(r->partition_sha256,partition_sha256)&&
        r->bootloader.bytes==bootloader->bytes&&hash(r->bootloader.sha256,sizeof(r->bootloader.sha256))&&!strcmp(r->bootloader.sha256,bootloader->sha256)&&
        r->layout.app_slot_bytes==physical->app_slot_bytes&&r->layout.pack_capacity_bytes==physical->pack_capacity_bytes&&
        r->layout.pet_partition_bytes==physical->pet_partition_bytes&&release->bytes>0&&
        (uint64_t)release->bytes*100<=(uint64_t)physical->app_slot_bytes*85&&
        protects_all(release,physical,protection);
}
