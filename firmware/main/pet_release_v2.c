#include "pet_release_v2.h"
#include "pet_board.h"
#include <string.h>
#include "cJSON.h"
#include "mbedtls/sha256.h"

static const cJSON *field(const cJSON *o,const char *name)
{return cJSON_GetObjectItemCaseSensitive(o,name);}
static bool keys_only(const cJSON *o,const char *names,unsigned count)
{
    if(!cJSON_IsObject(o)||cJSON_GetArraySize(o)!=(int)count)return false;
    for(const cJSON *a=o->child;a;a=a->next){
        if(!a->string)return false;
        for(const cJSON *b=a->next;b;b=b->next)if(!b->string||!strcmp(a->string,b->string))return false;
        bool found=false;
        for(const char *p=names;*p;){
            const char *end=strchr(p,'|');size_t n=end?(size_t)(end-p):strlen(p);
            if(strlen(a->string)==n&&!memcmp(a->string,p,n)){found=true;break;}
            if(!end)break;
            p=end+1;
        }
        if(!found)return false;
    }
    return true;
}
static bool equal(const cJSON *o,const char *name,const char *s)
{const cJSON *v=field(o,name);return s&&cJSON_IsString(v)&&!strcmp(v->valuestring,s);}
static bool text(const cJSON *o,const char *name,char *out,size_t capacity)
{
    const cJSON *v=field(o,name);
    if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=capacity)return false;
    strcpy(out,v->valuestring);return true;
}
static bool hex(const char *s,size_t capacity)
{return s&&memchr(s,0,capacity)&&strlen(s)==capacity-1&&strspn(s,"0123456789abcdef")==capacity-1;}
static bool uuid(const char *s)
{
    if(!s||!memchr(s,0,37)||strlen(s)!=36)return false;
    for(unsigned i=0;i<36;++i){
        if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}
        else if(!strchr("0123456789abcdef",s[i]))return false;
    }
    return s[14]>='1'&&s[14]<='5'&&strchr("89ab",s[19]);
}
static bool number(const cJSON *o,const char *name,uint32_t min,uint32_t max,uint32_t *out)
{
    const cJSON *v=field(o,name);
    if(!cJSON_IsNumber(v)||!(v->valuedouble>=min&&v->valuedouble<=max))return false;
    uint32_t n=(uint32_t)v->valuedouble;if(v->valuedouble!=n)return false;*out=n;return true;
}
static bool exact(const cJSON *o,const char *name,uint32_t n)
{uint32_t out;return number(o,name,n,n,&out);}
static bool timestamp(const char *s)
{
    size_t n=strlen(s);if(n<20||n>40||s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':')return false;
    unsigned p[6]={0};const unsigned start[]={0,5,8,11,14,17},length[]={4,2,2,2,2,2};
    for(unsigned i=0;i<6;++i)for(unsigned j=0;j<length[i];++j){
        char c=s[start[i]+j];if(c<'0'||c>'9')return false;p[i]=p[i]*10+(unsigned)(c-'0');
    }
    const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(p[1]<1||p[1]>12||p[2]<1||p[2]>days[p[1]-1]+(p[1]==2&&p[0]%4==0&&(p[0]%100!=0||p[0]%400==0))||
       p[3]>23||p[4]>59||p[5]>59)return false;
    size_t i=19;
    if(s[i]=='.'){size_t first=++i;while(i<n&&s[i]>='0'&&s[i]<='9')++i;if(i==first)return false;}
    if(i==n-1&&s[i]=='Z')return true;
    if(n-i!=6||(s[i]!='+'&&s[i]!='-')||s[i+3]!=':'||s[i+1]<'0'||s[i+1]>'2'||s[i+2]<'0'||s[i+2]>'9'||
       s[i+4]<'0'||s[i+4]>'5'||s[i+5]<'0'||s[i+5]>'9')return false;
    return (s[i+1]-'0')*10+s[i+2]-'0'<=23;
}
static bool requirements(const cJSON *o,pet_release_v2_t *r)
{
    pet_firmware_protected_pack_t *p=&r->requirements;
    p->imported_release=field(o,"importedRelease")!=NULL; /* Optional, exactly 1. */
    if(!keys_only(o,"hardware|chip|layout|formatVersion|resolution|renderer|codecs|minimumFirmwareEpoch|updateMode|importedRelease",p->imported_release?10:9)||
       (p->imported_release&&!exact(o,"importedRelease",1))||
       !equal(o,"hardware",pet_board_current()->hardware)||!equal(o,"chip","esp32s3")||
       !equal(o,"renderer","frame-player-cloud-0.4")||!equal(o,"updateMode","single-slot-replace-v2")||
       !exact(o,"formatVersion",2)||!exact(o,"resolution",240)||
       !number(o,"minimumFirmwareEpoch",1,UINT32_MAX,&p->minimum_firmware_epoch))return false;
    const cJSON *layout=field(o,"layout");pet_flash_layout_t known={0};
    if(!keys_only(layout,"id|partitionTableSha256|flashBytes|appSlotBytes|petPartitionBytes|manifestReservationBytes|packCapacityBytes",7))return false;
    /* A release names one physical layout: a single-pet device or the Pocket
     * Terminal's three-pet store, where packCapacityBytes is one slot. */
    for(unsigned id=PET_LAYOUT_SINGLE_2M;id<=PET_LAYOUT_THREE_3P5M;++id)
        if(equal(layout,"id",pet_flash_layout_name((pet_layout_id_t)id))){pet_flash_layout_known((pet_layout_id_t)id,&known);break;}
    if(!known.pet_slots||!text(layout,"partitionTableSha256",p->partition_sha256,sizeof(p->partition_sha256))||
       !hex(p->partition_sha256,sizeof(p->partition_sha256))||!exact(layout,"flashBytes",PET_FLASH_BYTES)||
       !exact(layout,"appSlotBytes",known.app_slot_bytes)||!exact(layout,"petPartitionBytes",known.pet_partition_bytes)||
       !exact(layout,"manifestReservationBytes",PET_REPLACE_MANIFEST_BYTES)||!exact(layout,"packCapacityBytes",known.pack_capacity_bytes)||
       r->pack.bytes>known.pack_capacity_bytes||(known.id==PET_LAYOUT_THREE_3P5M&&
       (r->pack.bytes>PET_RELEASE_V2_THREE_PET_MAX_BYTES||p->minimum_firmware_epoch<PET_RELEASE_V2_THREE_PET_EPOCH)))return false;
    const cJSON *codecs=field(o,"codecs");uint8_t mask=0;
    if(!cJSON_IsArray(codecs)||cJSON_GetArraySize(codecs)<1||cJSON_GetArraySize(codecs)>8)return false;
    for(const cJSON *v=codecs->child;v;v=v->next){
        if(!cJSON_IsNumber(v)||!(v->valuedouble>=0&&v->valuedouble<=7))return false;
        unsigned codec=(unsigned)v->valuedouble;
        if(v->valuedouble!=codec||(mask&(1u<<codec)))return false;
        mask|=(uint8_t)(1u<<codec);
    }
    p->present=true;p->layout_id=known.id;p->bytes=r->pack.bytes;p->format=p->resolution_divisor=2;p->codecs=mask;return true;
}
/* Exactly one of the two cloud descriptions (release-v2.ts): H3-sampled layered
 * packs, or authored step timing, which firmware epoch 3 introduced. */
static bool sampling_ok(const cJSON *s,uint32_t minimum_firmware_epoch)
{
    if(keys_only(s,"version|movementFps|preserveDuration|effectFps",4))
        return exact(s,"version",2)&&exact(s,"movementFps",4)&&exact(s,"effectFps",8)&&cJSON_IsTrue(field(s,"preserveDuration"));
    return keys_only(s,"version|timing|tickMs|preserveDuration",4)&&exact(s,"version",3)&&
        equal(s,"timing","authored-steps")&&exact(s,"tickMs",33)&&cJSON_IsTrue(field(s,"preserveDuration"))&&
        minimum_firmware_epoch>=PET_RELEASE_V2_AUTHORED_TIMING_EPOCH;
}
/* The cloud's imported variant (release-v2.ts): provenance binds the unchanged
 * signed bytes, the native validation and the saved configuration. It needs
 * requirements.importedRelease, authored timing and the exact-hash human
 * approval; an imported file never claims a candidate promotion. */
static bool imported_ok(const cJSON *v,const cJSON *sampling,pet_release_v2_t *r)
{
    return keys_only(v,"version|kind|importId|originalSha256|validationSha256|configurationSha256",6)&&
        exact(v,"version",1)&&equal(v,"kind","validated-import")&&
        text(v,"importId",r->import_id,sizeof(r->import_id))&&uuid(r->import_id)&&
        text(v,"originalSha256",r->original_sha256,sizeof(r->original_sha256))&&!strcmp(r->original_sha256,r->pack.sha256)&&
        text(v,"validationSha256",r->validation_sha256,sizeof(r->validation_sha256))&&!strcmp(r->validation_sha256,r->source_manifest_sha256)&&
        text(v,"configurationSha256",r->configuration_sha256,sizeof(r->configuration_sha256))&&!strcmp(r->configuration_sha256,r->input_hash)&&
        r->requirements.imported_release&&exact(sampling,"version",3)&&!r->promoted_approval;
}
static bool same_pack(const pet_replace_pack_t *a,const pet_replace_pack_t *b)
{
    return a&&b&&uuid(a->build_id)&&uuid(b->build_id)&&hex(a->sha256,sizeof(a->sha256))&&hex(b->sha256,sizeof(b->sha256))&&
        a->bytes>0&&a->bytes<=PET_REPLACE_MAX_BYTES&&a->bytes==b->bytes&&!strcmp(a->build_id,b->build_id)&&!strcmp(a->sha256,b->sha256);
}
/* A release that replaces an installed pet names that pack exactly. Only a
 * three-pet device on epoch 5 or later can honour it. */
static bool replacement_target_ok(const cJSON *target,pet_release_v2_t *r)
{
    pet_replace_pack_t *t=&r->replacement_target;
    return keys_only(target,"buildId|sha256|bytes",3)&&
        text(target,"buildId",t->build_id,sizeof(t->build_id))&&uuid(t->build_id)&&
        text(target,"sha256",t->sha256,sizeof(t->sha256))&&hex(t->sha256,sizeof(t->sha256))&&
        number(target,"bytes",1,PET_RELEASE_V2_THREE_PET_MAX_BYTES,&t->bytes)&&
        r->requirements.layout_id==PET_LAYOUT_THREE_3P5M&&
        r->requirements.minimum_firmware_epoch>=PET_RELEASE_V2_TARGETED_EPOCH;
}
static bool verify(const char *payload,const char *key,const char *signature,const char *account,
                    const pet_replace_pack_t *expected,const pet_pack_trust_key_t *trust,size_t count,pet_release_v2_t *out)
{
    if(!out||!uuid(account)||!same_pack(expected,expected)||strlen(payload)>7000||strlen(key)>80||
       strspn(key,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")!=strlen(key)||
       !pet_release_verify_signature(payload,strlen(payload)+1,key,strlen(key)+1,signature,strlen(signature)+1,trust,count))return false;
    cJSON *o=pet_control_json(payload,strlen(payload),7000);pet_release_v2_t r={0};
    const cJSON *compiler=field(o,"compiler"),*provenance=field(o,"provenance"),*sampling=field(o,"sampling"),*approval=field(o,"approval");
    r.imported=cJSON_IsNull(compiler);
    const cJSON *target=field(o,"replacementTarget");r.has_replacement_target=target!=NULL;
    bool ok=keys_only(o,"kind|version|buildId|accountId|projectId|faceId|packVersion|sha256|bytes|inputHash|characterFingerprint|compiler|provenance|requirements|sampling|inventorySha256|actionMapSha256|sourceManifestSha256|approval|readiness|replacementTarget",19+(provenance!=NULL)+r.has_replacement_target)&&
        equal(o,"kind","private-device-release")&&exact(o,"version",2)&&equal(o,"readiness","device-ready")&&
        text(o,"buildId",r.pack.build_id,sizeof(r.pack.build_id))&&
        text(o,"sha256",r.pack.sha256,sizeof(r.pack.sha256))&&number(o,"bytes",1,PET_REPLACE_MAX_BYTES,&r.pack.bytes)&&same_pack(&r.pack,expected)&&
        text(o,"accountId",r.account_id,sizeof(r.account_id))&&equal(o,"accountId",account)&&
        text(o,"projectId",r.project_id,sizeof(r.project_id))&&uuid(r.project_id)&&
        text(o,"faceId",r.face_id,sizeof(r.face_id))&&text(o,"packVersion",r.version,sizeof(r.version))&&
        text(o,"inputHash",r.input_hash,sizeof(r.input_hash))&&hex(r.input_hash,sizeof(r.input_hash))&&
        text(o,"characterFingerprint",r.character_fingerprint,sizeof(r.character_fingerprint))&&hex(r.character_fingerprint,sizeof(r.character_fingerprint))&&
        (r.imported||(keys_only(compiler,"version|commit|sha256",3)&&text(compiler,"version",r.compiler_version,sizeof(r.compiler_version))&&
        text(compiler,"commit",r.compiler_commit,sizeof(r.compiler_commit))&&hex(r.compiler_commit,sizeof(r.compiler_commit))&&
        text(compiler,"sha256",r.compiler_sha256,sizeof(r.compiler_sha256))&&hex(r.compiler_sha256,sizeof(r.compiler_sha256))))&&
        requirements(field(o,"requirements"),&r)&&sampling_ok(sampling,r.requirements.minimum_firmware_epoch)&&
        text(o,"inventorySha256",r.inventory_sha256,sizeof(r.inventory_sha256))&&hex(r.inventory_sha256,sizeof(r.inventory_sha256))&&
        text(o,"actionMapSha256",r.action_map_sha256,sizeof(r.action_map_sha256))&&hex(r.action_map_sha256,sizeof(r.action_map_sha256))&&
        text(o,"sourceManifestSha256",r.source_manifest_sha256,sizeof(r.source_manifest_sha256))&&hex(r.source_manifest_sha256,sizeof(r.source_manifest_sha256))&&
        text(approval,"actorId",r.approval_actor,sizeof(r.approval_actor))&&uuid(r.approval_actor)&&
        text(approval,"at",r.approval_at,sizeof(r.approval_at))&&timestamp(r.approval_at);
    if(ok&&equal(approval,"kind","human-compiled-pack"))
        ok=keys_only(approval,"kind|actorId|at|sha256",4)&&equal(approval,"sha256",r.pack.sha256);
    else if(ok){
        r.promoted_approval=true;
        ok=keys_only(approval,"kind|actorId|at|sha256|candidateId|approvalId|algorithm",7)&&
           equal(approval,"kind","promoted-compiled-pack")&&equal(approval,"algorithm","approved-header-v1")&&
           text(approval,"sha256",r.reviewed_sha256,sizeof(r.reviewed_sha256))&&hex(r.reviewed_sha256,sizeof(r.reviewed_sha256))&&
           strcmp(r.reviewed_sha256,r.pack.sha256)!=0&&
           text(approval,"candidateId",r.candidate_id,sizeof(r.candidate_id))&&uuid(r.candidate_id)&&
           text(approval,"approvalId",r.approval_id,sizeof(r.approval_id))&&uuid(r.approval_id);
    }
    /* A compiled release keeps its contract: no import provenance or requirement. */
    if(ok)ok=r.imported?imported_ok(provenance,sampling,&r):!provenance&&!r.requirements.imported_release;
    if(ok&&r.has_replacement_target)ok=replacement_target_ok(target,&r);
    cJSON_Delete(o);if(ok)*out=r;return ok;
}
bool pet_release_v2_verify(const cJSON *envelope,const char *account,const pet_replace_pack_t *expected,
                           const pet_pack_trust_key_t *trust,size_t count,pet_release_v2_t *out)
{
    if(!keys_only(envelope,"algorithm|keyId|payload|signature",4)||!equal(envelope,"algorithm","ES256"))return false;
    const cJSON *payload=field(envelope,"payload"),*key=field(envelope,"keyId"),*signature=field(envelope,"signature");
    return cJSON_IsString(payload)&&cJSON_IsString(key)&&cJSON_IsString(signature)&&
        verify(payload->valuestring,key->valuestring,signature->valuestring,account,expected,trust,count,out);
}
bool pet_release_v2_compatible(const pet_release_v2_t *r,const pet_firmware_requirements_t *current)
{
    if(!r||!current||!same_pack(&r->pack,&r->pack)||current->bootloader.bytes<4096||current->bootloader.bytes>0x8000||
       !hex(current->bootloader.sha256,sizeof(current->bootloader.sha256))||!hex(current->partition_sha256,sizeof(current->partition_sha256)))return false;
    pet_flash_layout_t layout;
    if(!pet_flash_layout_known(current->layout.id,&layout)||current->layout.id<PET_LAYOUT_SINGLE_2M||
       current->layout.app_slot_bytes!=layout.app_slot_bytes||current->layout.pet_partition_bytes!=layout.pet_partition_bytes||
       current->layout.pack_capacity_bytes!=layout.pack_capacity_bytes)return false;
    const pet_firmware_protected_pack_t *p=&r->requirements;
    return p->present&&p->layout_id==layout.id&&hex(p->partition_sha256,sizeof(p->partition_sha256))&&
        !strcmp(p->partition_sha256,current->partition_sha256)&&p->bytes==r->pack.bytes&&p->bytes<=layout.pack_capacity_bytes&&
        p->minimum_firmware_epoch>0&&p->minimum_firmware_epoch<=current->firmware_epoch&&p->format==2&&p->resolution_divisor==2&&
        (current->formats&(1u<<2))&&p->codecs&&!(p->codecs&~current->codecs)&&(!p->imported_release||current->imported_release);
}

enum { RECORD_HEADER=48 };
static uint16_t rd16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static void wr16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
bool pet_release_v2_record_encode(const cJSON *envelope,const char *account,const pet_replace_pack_t *expected,
                                  const pet_pack_trust_key_t *trust,size_t count,void *record,size_t bytes)
{
    pet_release_v2_t parsed;
    if(!record||bytes!=PET_REPLACE_MANIFEST_BYTES||!pet_release_v2_verify(envelope,account,expected,trust,count,&parsed))return false;
    const char *parts[]={field(envelope,"payload")->valuestring,field(envelope,"keyId")->valuestring,field(envelope,"signature")->valuestring};
    uint8_t *p=record;memset(p,0xff,bytes);memset(p,0,RECORD_HEADER);memcpy(p,"PSM2",4);wr16(p+4,2);wr16(p+6,RECORD_HEADER);
    size_t end=RECORD_HEADER;
    for(unsigned i=0;i<3;++i){size_t n=strlen(parts[i])+1;wr16(p+8+i*2,(uint16_t)n);memcpy(p+end,parts[i],n);end+=n;}
    return !mbedtls_sha256(p+RECORD_HEADER,end-RECORD_HEADER,p+16,0);
}
bool pet_release_v2_record_verify(const void *record,size_t bytes,const char *account,const pet_replace_pack_t *expected,
                                  const pet_pack_trust_key_t *trust,size_t count,pet_release_v2_t *out)
{
    if(!record||bytes!=PET_REPLACE_MANIFEST_BYTES)return false;
    const uint8_t *p=record;
    if(memcmp(p,"PSM2",4)||rd16(p+4)!=2||rd16(p+6)!=RECORD_HEADER||rd16(p+14))return false;
    size_t lengths[]={rd16(p+8),rd16(p+10),rd16(p+12)};
    if(lengths[0]<2||lengths[0]>7001||lengths[1]<2||lengths[1]>81||lengths[2]!=87)return false;
    const char *parts[3];size_t end=RECORD_HEADER;
    for(unsigned i=0;i<3;++i){
        if(lengths[i]>bytes-end||p[end+lengths[i]-1]||memchr(p+end,0,lengths[i]-1))return false;
        parts[i]=(const char *)p+end;end+=lengths[i];
    }
    for(size_t i=end;i<bytes;++i)if(p[i]!=0xff)return false;
    uint8_t digest[32];
    return !mbedtls_sha256(p+RECORD_HEADER,end-RECORD_HEADER,digest,0)&&!memcmp(digest,p+16,32)&&
        verify(parts[0],parts[1],parts[2],account,expected,trust,count,out);
}
