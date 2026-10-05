#include "pet_replace_wire.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static const cJSON *field(const cJSON *o,const char *name){return cJSON_GetObjectItemCaseSensitive(o,name);}
static bool bounded(const char *s,size_t n){return s&&memchr(s,0,n);}
static bool uuid(const char *s)
{
    if(!bounded(s,37)||strlen(s)!=36)return false;
    for(unsigned i=0;i<36;++i){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}
        else if(!strchr("0123456789abcdef",s[i]))return false;}
    return s[14]>='1'&&s[14]<='5'&&strchr("89ab",s[19]);
}
static bool sha(const char *s){return bounded(s,65)&&strlen(s)==64&&strspn(s,"0123456789abcdef")==64;}
/* cJSON checks escaped UTF-16 pairs; validate raw UTF-8 before handing it bytes. */
static bool utf8(const char *s,size_t n)
{
    for(size_t i=0;i<n;){uint32_t c=(unsigned char)s[i++],value,min;unsigned extra;
        if(c<0x80){if(!c)return false;continue;}
        if(c>=0xc2&&c<=0xdf){value=c&31;extra=1;min=0x80;}
        else if(c>=0xe0&&c<=0xef){value=c&15;extra=2;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){value=c&7;extra=3;min=0x10000;}
        else return false;
        if(extra>n-i)return false;
        while(extra--){c=(unsigned char)s[i++];if((c&0xc0)!=0x80)return false;value=(value<<6)|(c&63);}
        if(value<min||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
    }return true;
}
static bool opaque(const char *s){return bounded(s,81)&&s[0]&&utf8(s,strlen(s));}
static bool digit(char c){return c>='0'&&c<='9';}
/* cJSON intentionally accepts some non-JSON lexical forms. Reject them before
 * its bounded syntax/duplicate-key pass; never normalize signed payload bytes. */
static bool lexical(const char *s,size_t n)
{
    for(size_t i=0;i<n;){
        char c=s[i];
        if(strchr(" \t\r\n{}[]:,",c)){++i;continue;}
        if(c=='"'){
            bool closed=false;++i;
            while(i<n){c=s[i++];if(c=='"'){closed=true;break;}
                if((unsigned char)c<0x20)return false;
                if(c!='\\')continue;
                if(i==n)return false;
                c=s[i++];
                if(c=='u'){
                    if(n-i<4)return false;
                    for(unsigned j=0;j<4;++j)if(!strchr("0123456789abcdefABCDEF",s[i++]))return false;
                }else if(!strchr("\"\\/bfnrt",c))return false;
            }
            if(!closed)return false;
            continue;
        }
        if(c=='-'||digit(c)){
            if(c=='-'&&++i==n)return false;
            if(s[i]=='0')++i;
            else {if(s[i]<'1'||s[i]>'9')return false;while(i<n&&digit(s[i]))++i;}
            if(i<n&&s[i]=='.'){++i;if(i==n||!digit(s[i]))return false;while(i<n&&digit(s[i]))++i;}
            if(i<n&&(s[i]=='e'||s[i]=='E')){++i;if(i<n&&(s[i]=='+'||s[i]=='-'))++i;
                if(i==n||!digit(s[i]))return false;
                while(i<n&&digit(s[i]))++i;}
        }else {
            const char *literal=c=='t'?"true":c=='f'?"false":c=='n'?"null":NULL;
            if(!literal||n-i<strlen(literal)||memcmp(s+i,literal,strlen(literal)))return false;
            i+=strlen(literal);
        }
        if(i<n&&!strchr(" \t\r\n{}[]:,",s[i]))return false;
    }return true;
}
static cJSON *document(const char *s,size_t n)
{return s&&n<=PET_CONTROL_RESPONSE_MAX&&utf8(s,n)&&lexical(s,n)?pet_control_json(s,n,PET_CONTROL_RESPONSE_MAX):NULL;}
static bool keys(const cJSON *o,const char *names,unsigned count)
{
    if(!cJSON_IsObject(o)||cJSON_GetArraySize(o)!=(int)count)return false;
    for(const cJSON *v=o->child;v;v=v->next){bool found=false;
        for(const char *p=names;*p;){const char *end=strchr(p,'|');size_t n=end?(size_t)(end-p):strlen(p);
            if(v->string&&strlen(v->string)==n&&!memcmp(v->string,p,n)){found=true;break;}
            if(!end)break;
            p=end+1;
        }if(!found)return false;
    }return true;
}
static bool text(const cJSON *o,const char *name,char *out,size_t cap)
{const cJSON *v=field(o,name);if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=cap)return false;strcpy(out,v->valuestring);return true;}
static bool equal(const cJSON *o,const char *name,const char *s)
{const cJSON *v=field(o,name);return s&&cJSON_IsString(v)&&!strcmp(v->valuestring,s);}
static bool number(const cJSON *o,const char *name,uint32_t min,uint32_t max,uint32_t *out)
{const cJSON *v=field(o,name);if(!cJSON_IsNumber(v)||!(v->valuedouble>=min&&v->valuedouble<=max))return false;
 uint32_t n=(uint32_t)v->valuedouble;if(v->valuedouble!=n)return false;*out=n;return true;}
static bool exact(const cJSON *o,const char *name,uint32_t value){uint32_t n;return number(o,name,value,value,&n);}
static bool boolean(const cJSON *o,const char *name,bool *out)
{const cJSON *v=field(o,name);if(!cJSON_IsBool(v))return false;*out=cJSON_IsTrue(v);return true;}
static bool generation(const cJSON *o,const char *name,uint64_t *out)
{
    const cJSON *v=field(o,name);if(!cJSON_IsString(v)||v->valuestring[0]<'1'||v->valuestring[0]>'9')return false;
    uint64_t value=0;size_t n=strlen(v->valuestring);if(n>20)return false;
    for(size_t i=0;i<n;++i){unsigned digit=(unsigned char)v->valuestring[i]-'0';
        if(digit>9||value>(UINT64_MAX-digit)/10)return false;
        value=value*10+digit;}
    *out=value;return true;
}
static const char *phase_name(pet_replace_cloud_phase_t phase)
{
    static const char *names[]={"queued","fenced","invalidated","downloading","verified","activating","installed","recovery","cancelled","superseded"};
    return (unsigned)phase<sizeof(names)/sizeof(*names)?names[phase]:NULL;
}
static bool phase(const cJSON *o,const char *name,pet_replace_cloud_phase_t *out)
{for(unsigned i=0;i<=PET_CLOUD_SUPERSEDED;++i)if(equal(o,name,phase_name((pet_replace_cloud_phase_t)i))){*out=(pet_replace_cloud_phase_t)i;return true;}return false;}
static bool pack(const cJSON *o,pet_replace_pack_t *out)
{return text(o,"buildId",out->build_id,sizeof(out->build_id))&&uuid(out->build_id)&&text(o,"sha256",out->sha256,sizeof(out->sha256))&&sha(out->sha256)&&number(o,"bytes",1,PET_REPLACE_MAX_BYTES,&out->bytes);}
static bool same_pack(const pet_replace_pack_t *a,const pet_replace_pack_t *b)
{return a->bytes==b->bytes&&!strcmp(a->build_id,b->build_id)&&!strcmp(a->sha256,b->sha256);}
static bool binding(const cJSON *o,pet_control_binding_t *out)
{return pet_control_binding(o,out)&&opaque(out->revision)&&(!out->assigned||(uuid(out->relationship_id)&&uuid(out->build_id)));}
static bool config(const cJSON *o,pet_control_config_t *out)
{return pet_control_config(o,out)&&opaque(out->version)&&strlen(out->face_id)<=63&&
    strchr("abcdefghijklmnopqrstuvwxyz0123456789",out->ai_pet_id[0]);}
/* cJSON_Compare tolerates floating-point differences. Metadata copies require
 * exact values; only the separately verified original payload is authority. */
static bool exact_json(const cJSON *a,const cJSON *b)
{
    if(!a||!b||(a->type&0xff)!=(b->type&0xff))return false;
    if(cJSON_IsNumber(a))return a->valuedouble==b->valuedouble;
    if(cJSON_IsString(a))return !strcmp(a->valuestring,b->valuestring);
    if(cJSON_IsNull(a)||cJSON_IsBool(a))return true;
    if(!cJSON_IsObject(a)&&!cJSON_IsArray(a))return false;
    if(cJSON_GetArraySize(a)!=cJSON_GetArraySize(b))return false;
    const cJSON *other=b->child;
    for(const cJSON *item=a->child;item;item=item->next){
        if(cJSON_IsObject(a))other=field(b,item->string);
        if(!exact_json(item,other))return false;
        if(cJSON_IsArray(a))other=other->next;
    }return true;
}
static bool report(const cJSON *o,pet_replace_report_t *out)
{
    if(!keys(o,"version|installationId|fenceId|buildId|sha256|bytes|status|downloadedBytes|prefixSha256|journalGeneration|activation",11)||
       !exact(o,"version",2)||!pack(o,&out->pack)||!text(o,"installationId",out->operation_id,sizeof(out->operation_id))||!uuid(out->operation_id)||
       !text(o,"fenceId",out->fence_id,sizeof(out->fence_id))||!uuid(out->fence_id)||!generation(o,"journalGeneration",&out->generation)||
       !phase(o,"status",&out->phase)||out->phase<PET_CLOUD_INVALIDATED||out->phase>PET_CLOUD_RECOVERY||
       !number(o,"downloadedBytes",0,out->pack.bytes,&out->downloaded_bytes))return false;
    if(out->downloaded_bytes!=out->pack.bytes&&out->downloaded_bytes%PET_REPLACE_CHECKPOINT_BYTES)return false;
    if(out->downloaded_bytes){if(!text(o,"prefixSha256",out->prefix_sha256,sizeof(out->prefix_sha256))||!sha(out->prefix_sha256))return false;}
    else if(!cJSON_IsNull(field(o,"prefixSha256")))return false;
    if(out->phase==PET_CLOUD_INVALIDATED&&out->downloaded_bytes)return false;
    if(out->phase>=PET_CLOUD_VERIFIED&&out->phase<=PET_CLOUD_INSTALLED&&
       (out->downloaded_bytes!=out->pack.bytes||strcmp(out->prefix_sha256,out->pack.sha256)))return false;
    const cJSON *activation=field(o,"activation");
    if(out->phase!=PET_CLOUD_INSTALLED)return cJSON_IsNull(activation);
    return keys(activation,"bindingRevision|relationshipId|configVersion",3)&&
        text(activation,"bindingRevision",out->binding_revision,sizeof(out->binding_revision))&&opaque(out->binding_revision)&&
        text(activation,"relationshipId",out->relationship_id,sizeof(out->relationship_id))&&uuid(out->relationship_id)&&
        text(activation,"configVersion",out->config_version,sizeof(out->config_version))&&opaque(out->config_version);
}
static bool operation(const cJSON *o,const char *device,const char *account,const pet_pack_trust_key_t *trust,size_t count,pet_replace_operation_t *out)
{
    if(!uuid(account)||!keys(o,"version|installationId|deviceId|accountId|requestId|fenceId|expectedBindingRevision|state|fenceConfirmed|flashReserved|cancelRequested|supersedes|release|manifest|downloadPath|lastReport|resultingBinding|resultingConfig",18)||
       !exact(o,"version",2)||!equal(o,"deviceId",device)||!equal(o,"accountId",account)||
       !text(o,"installationId",out->id,sizeof(out->id))||!uuid(out->id)||!text(o,"requestId",out->request_id,sizeof(out->request_id))||!uuid(out->request_id)||
       !text(o,"fenceId",out->fence_id,sizeof(out->fence_id))||!uuid(out->fence_id)||
       !text(o,"expectedBindingRevision",out->expected_revision,sizeof(out->expected_revision))||!opaque(out->expected_revision)||
       !phase(o,"state",&out->phase)||!boolean(o,"fenceConfirmed",&out->fence_confirmed)||!boolean(o,"flashReserved",&out->flash_reserved)||
       !boolean(o,"cancelRequested",&out->cancel_requested))return false;
    const cJSON *manifest=field(o,"manifest"),*release=field(o,"release");pet_replace_pack_t target={0};
    if(!pack(release,&target)||!pet_release_v2_verify(manifest,account,&target,trust,count,&out->release)||
       !text(manifest,"payload",out->signed_payload,sizeof(out->signed_payload))||!text(manifest,"keyId",out->key_id,sizeof(out->key_id))||
       !text(manifest,"signature",out->signature,sizeof(out->signature)))return false;
    cJSON *signed_release=document(out->signed_payload,strlen(out->signed_payload));
    bool same=exact_json(signed_release,release);cJSON_Delete(signed_release);if(!same)return false;
    const char *face=out->release.face_id;size_t face_bytes=strlen(face);
    if(face_bytes<3||face_bytes>63||face[0]<'a'||face[0]>'z'||face[face_bytes-1]=='-'||
       strspn(face,"abcdefghijklmnopqrstuvwxyz0123456789-")!=face_bytes)return false;
    char path[80];snprintf(path,sizeof(path),"/v2/device/pets/%s/pack",out->id);if(!equal(o,"downloadPath",path))return false;
    const cJSON *previous=field(o,"supersedes"),*receipt=field(o,"lastReport");
    out->supersedes=!cJSON_IsNull(previous);out->has_report=!cJSON_IsNull(receipt);
    if(out->supersedes&&(!keys(previous,"installationId|fenceId|journalGeneration",3)||
       !text(previous,"installationId",out->previous_id,sizeof(out->previous_id))||!uuid(out->previous_id)||!strcmp(out->previous_id,out->id)||
       !text(previous,"fenceId",out->previous_fence_id,sizeof(out->previous_fence_id))||!uuid(out->previous_fence_id)||!strcmp(out->previous_fence_id,out->fence_id)||
       !generation(previous,"journalGeneration",&out->previous_generation)))return false;
    pet_replace_report_t *r=&out->report;
    if(out->has_report&&(!report(receipt,r)||strcmp(r->operation_id,out->id)||strcmp(r->fence_id,out->fence_id)||
       !same_pack(&r->pack,&out->release.pack)||(out->supersedes&&r->generation<=out->previous_generation)))return false;
    if(out->cancel_requested&&(out->phase==PET_CLOUD_QUEUED||out->phase==PET_CLOUD_VERIFIED||
       out->phase==PET_CLOUD_ACTIVATING||out->phase==PET_CLOUD_INSTALLED))return false;
    if(out->phase==PET_CLOUD_CANCELLED&&!out->cancel_requested)return false;
    if(out->phase==PET_CLOUD_QUEUED||out->phase==PET_CLOUD_CANCELLED){
        if(out->supersedes||out->has_report||out->fence_confirmed||out->flash_reserved!=(out->phase==PET_CLOUD_QUEUED))return false;
    }else if(out->phase==PET_CLOUD_FENCED){
        if(!out->flash_reserved||(out->has_report&&r->phase!=PET_CLOUD_RECOVERY))return false;
    }else if(!out->fence_confirmed||!out->has_report||r->phase!=(out->phase==PET_CLOUD_SUPERSEDED?PET_CLOUD_RECOVERY:out->phase)||
             out->flash_reserved!=(out->phase!=PET_CLOUD_INSTALLED&&out->phase!=PET_CLOUD_RECOVERY&&out->phase!=PET_CLOUD_SUPERSEDED))return false;
    out->has_result=out->phase==PET_CLOUD_ACTIVATING||out->phase==PET_CLOUD_INSTALLED;
    if(!out->has_result)return cJSON_IsNull(field(o,"resultingBinding"))&&cJSON_IsNull(field(o,"resultingConfig"));
    if(!binding(field(o,"resultingBinding"),&out->binding)||!out->binding.assigned||
       strcmp(out->binding.build_id,out->release.pack.build_id)||strcmp(out->binding.sha256,out->release.pack.sha256)||
       !strcmp(out->binding.revision,out->expected_revision)||!config(field(o,"resultingConfig"),&out->config)||
       strcmp(out->config.ai_pet_id,face)||strcmp(out->config.face_id,face))return false;
    return out->phase!=PET_CLOUD_INSTALLED||(!strcmp(r->binding_revision,out->binding.revision)&&
        !strcmp(r->relationship_id,out->binding.relationship_id)&&!strcmp(r->config_version,out->config.version));
}
bool pet_replace_wire_operation(const char *json,size_t bytes,const char *device,const char *account,
                                const pet_pack_trust_key_t *trust,size_t count,pet_replace_operation_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));cJSON *root=document(json,bytes);
    bool ok=operation(root,device,account,trust,count,out);cJSON_Delete(root);if(!ok)memset(out,0,sizeof(*out));return ok;
}
bool pet_replace_wire_poll(const char *json,size_t bytes,const char *device,const char *account,
                           const pet_pack_trust_key_t *trust,size_t count,pet_replace_poll_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));cJSON *root=document(json,bytes);pet_control_context_t *c=&out->context;
    out->has_removals = field(root, "removals") != NULL;
    bool ok=keys(root,"version|deviceId|accountId|selectionAllowed|binding|config|operation|nextPollSeconds|removals",
                 out->has_removals ? 9 : 8)&&
        exact(root,"version",2)&&equal(root,"deviceId",device)&&text(root,"deviceId",c->device_id,sizeof(c->device_id))&&
        text(root,"accountId",c->account_id,sizeof(c->account_id))&&uuid(c->account_id)&&(!account||!strcmp(account,c->account_id))&&
        boolean(root,"selectionAllowed",&c->selection_allowed)&&binding(field(root,"binding"),&c->binding)&&
        config(field(root,"config"),&c->config)&&exact(root,"nextPollSeconds",15);
    if(ok){c->next_poll_seconds=15;out->has_operation=!cJSON_IsNull(field(root,"operation"));
        if(out->has_operation){ok=operation(field(root,"operation"),device,c->account_id,trust,count,&out->operation);
            if(ok){strcpy(c->pending_id,out->operation.id);pet_replace_cloud_phase_t p=out->operation.phase;
                if(p!=PET_CLOUD_QUEUED&&p!=PET_CLOUD_INSTALLED&&p!=PET_CLOUD_CANCELLED&&p!=PET_CLOUD_SUPERSEDED&&c->binding.assigned)ok=false;}}
    }
    if (ok && out->has_removals)
    {
        const cJSON *removals = field(root, "removals");
        ok = cJSON_IsArray(removals) && cJSON_GetArraySize(removals) <= (int)PET_REMOVAL_MAX;
        for (const cJSON *item = ok ? removals->child : NULL; item && ok; item = item->next)
        {
            pet_replace_removal_t *r = &out->removals[out->removal_count];
            ok = !out->has_operation && keys(item, "id|buildId|sha256|bytes", 4) &&
                 text(item, "id", r->id, sizeof(r->id)) && uuid(r->id) && pack(item, &r->pack) &&
                 (!c->binding.assigned || strcmp(c->binding.build_id, r->pack.build_id));
            for (unsigned i = 0; i < out->removal_count && ok; ++i)
                ok = strcmp(out->removals[i].id, r->id) != 0;
            if (ok)
                ++out->removal_count;
        }
    }
    cJSON_Delete(root);if(!ok)memset(out,0,sizeof(*out));return ok;
}
static bool emit(cJSON *root,bool valid,char *json,size_t capacity)
{bool ok=valid&&root&&json&&capacity>0&&capacity<=INT_MAX&&cJSON_PrintPreallocated(root,json,(int)capacity,false);
 cJSON_Delete(root);if(!ok&&json&&capacity)json[0]=0;return ok;}
bool pet_replace_wire_encode_poll(const char *boot,char *json,size_t capacity)
{
    if(!uuid(boot)){if(json&&capacity)json[0]=0;return false;}
    cJSON *root=cJSON_CreateObject();return emit(root,root&&cJSON_AddNumberToObject(root,"version",2)&&
        cJSON_AddStringToObject(root,"bootId",boot),json,capacity);
}
bool pet_replace_wire_encode_removal_poll(const char *boot, const char ack[][37], unsigned count,
                                          char *json, size_t capacity)
{
    bool valid = uuid(boot) && count <= PET_REMOVAL_MAX && (!count || ack);
    for (unsigned i = 0; i < count && valid; ++i)
        valid = uuid(ack[i]);
    cJSON *root = valid ? cJSON_CreateObject() : NULL;
    cJSON *removals = root ? cJSON_AddObjectToObject(root, "removals") : NULL;
    cJSON *list = removals ? cJSON_AddArrayToObject(removals, "ack") : NULL;
    valid = valid && list && cJSON_AddNumberToObject(root, "version", 2) &&
            cJSON_AddStringToObject(root, "bootId", boot);
    for (unsigned i = 0; i < count && valid; ++i)
    {
        cJSON *id = cJSON_CreateString(ack[i]);
        valid = id && cJSON_AddItemToArray(list, id);
        if (!valid)
            cJSON_Delete(id);
    }
    return emit(root, valid, json, capacity);
}
bool pet_replace_wire_encode_select(const char *boot,const char *build,const char *hash,char *json,size_t capacity)
{
    /* The cloud's releaseV2UuidSchema: RFC version and variant. A hash cut into
     * UUID shape (a pet placed over USB) usually fails it; about 8% pass, and
     * the cloud then finds no installation of that ID. */
    if(!uuid(boot)||!uuid(build)||!sha(hash)){if(json&&capacity)json[0]=0;return false;}
    cJSON *root=cJSON_CreateObject();return emit(root,root&&cJSON_AddNumberToObject(root,"version",2)&&
        cJSON_AddStringToObject(root,"bootId",boot)&&cJSON_AddStringToObject(root,"buildId",build)&&
        cJSON_AddStringToObject(root,"sha256",hash),json,capacity);
}
bool pet_replace_wire_encode_report(const pet_replace_report_t *r,char *json,size_t capacity)
{
    if(!r||!uuid(r->operation_id)||!uuid(r->fence_id)||!uuid(r->pack.build_id)||!sha(r->pack.sha256)||!r->generation||
       !bounded(r->prefix_sha256,sizeof(r->prefix_sha256))||!bounded(r->binding_revision,sizeof(r->binding_revision))||
       !bounded(r->relationship_id,sizeof(r->relationship_id))||!bounded(r->config_version,sizeof(r->config_version))||!phase_name(r->phase)){
        if(json&&capacity)json[0]=0;
        return false;}
    char gen[21];snprintf(gen,sizeof(gen),"%" PRIu64,r->generation);cJSON *root=cJSON_CreateObject();
    bool ok=root&&cJSON_AddNumberToObject(root,"version",2)&&cJSON_AddStringToObject(root,"installationId",r->operation_id)&&
        cJSON_AddStringToObject(root,"fenceId",r->fence_id)&&cJSON_AddStringToObject(root,"buildId",r->pack.build_id)&&
        cJSON_AddStringToObject(root,"sha256",r->pack.sha256)&&cJSON_AddNumberToObject(root,"bytes",r->pack.bytes)&&
        cJSON_AddStringToObject(root,"status",phase_name(r->phase))&&cJSON_AddNumberToObject(root,"downloadedBytes",r->downloaded_bytes)&&
        (r->prefix_sha256[0]?cJSON_AddStringToObject(root,"prefixSha256",r->prefix_sha256):cJSON_AddNullToObject(root,"prefixSha256"))&&
        cJSON_AddStringToObject(root,"journalGeneration",gen);
    if(ok&&r->phase==PET_CLOUD_INSTALLED){cJSON *a=cJSON_AddObjectToObject(root,"activation");ok=a&&
        cJSON_AddStringToObject(a,"bindingRevision",r->binding_revision)&&cJSON_AddStringToObject(a,"relationshipId",r->relationship_id)&&
        cJSON_AddStringToObject(a,"configVersion",r->config_version);
    }else if(ok)ok=!r->binding_revision[0]&&!r->relationship_id[0]&&!r->config_version[0]&&cJSON_AddNullToObject(root,"activation");
    pet_replace_report_t checked={0};return emit(root,ok&&report(root,&checked),json,capacity);
}
