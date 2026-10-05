#include "pet_control_wire.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_animation_profile.h"

static const cJSON *field(const cJSON *obj, const char *key)
{ return cJSON_GetObjectItemCaseSensitive(obj,key); }

static bool keys(const cJSON *obj, const char *allowed, unsigned count)
{
    if(!cJSON_IsObject(obj)||cJSON_GetArraySize(obj)!=(int)count) return false;
    for(const cJSON *item=obj->child;item;item=item->next) {
        const char *p=allowed;bool found=false;
        while(*p) {
            const char *end=strchr(p,'|');size_t n=end?(size_t)(end-p):strlen(p);
            if(strlen(item->string)==n&&!memcmp(item->string,p,n)){found=true;break;}
            if(!end)break;
            p=end+1;
        }
        if(!found)return false;
    }
    return true;
}

static bool unique(const cJSON *value)
{
    for(const cJSON *a=value->child;a;a=a->next) {
        if(cJSON_IsObject(value))
            for(const cJSON *b=a->next;b;b=b->next)if(!strcmp(a->string,b->string))return false;
        if(!unique(a))return false;
    }
    return true;
}

cJSON *pet_control_json(const char *data,size_t bytes,size_t limit)
{
    if(!data||!bytes||bytes>limit||limit>PET_CONTROL_RESPONSE_MAX||memchr(data,0,bytes))return NULL;
    unsigned depth=0,nodes=0;bool quoted=false,escape=false;
    for(size_t i=0;i<bytes;++i) {
        char c=data[i];
        if(quoted) {
            if(escape) {
                if(c=='u'&&i+4<bytes&&!memcmp(data+i+1,"0000",4))return NULL;
                escape=false;
            }else if(c=='\\')escape=true;else if(c=='"')quoted=false;
        }else if(c=='"'){quoted=true;if(++nodes>512)return NULL;}
        else if(c=='{'||c=='['){if(++depth>12||++nodes>512)return NULL;}
        else if(c=='}'||c==']'){if(!depth)return NULL;--depth;}
        else if(c==','){if(++nodes>512)return NULL;}
    }
    if(quoted||depth)return NULL;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(data,bytes,&end,false);
    while(end&&end<data+bytes&&(*end==' '||*end=='\n'||*end=='\r'||*end=='\t'))++end;
    if(!root||end!=data+bytes||!unique(root)){cJSON_Delete(root);return NULL;}
    return root;
}

static bool text(const cJSON *obj,const char *key,char *out,size_t capacity,bool nullable)
{
    const cJSON *v=field(obj,key);
    if(nullable&&cJSON_IsNull(v)){out[0]=0;return true;}
    if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=capacity)return false;
    strcpy(out,v->valuestring);return true;
}
static bool number(const cJSON *obj,const char *key,uint32_t min,uint32_t max,uint32_t *out)
{
    const cJSON *v=field(obj,key);
    if(!cJSON_IsNumber(v)||!(v->valuedouble>=min&&v->valuedouble<=max))return false;
    uint32_t n=(uint32_t)v->valuedouble;
    if(v->valuedouble!=n)return false;
    *out=n;
    return true;
}
static bool boolean(const cJSON *obj,const char *key,bool *out)
{
    const cJSON *v=field(obj,key);if(!cJSON_IsBool(v))return false;*out=cJSON_IsTrue(v);return true;
}
static bool equal(const cJSON *obj,const char *key,const char *expected)
{ const cJSON *v=field(obj,key);return cJSON_IsString(v)&&!strcmp(v->valuestring,expected); }
static bool uuid(const char *s)
{
    if(strlen(s)!=36)return false;
    for(unsigned i=0;i<36;++i)
        if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}
        else if(!strchr("0123456789abcdef",s[i]))return false;
    return true;
}
static bool hash(const char *s){return strlen(s)==64&&strspn(s,"0123456789abcdef")==64;}
bool pet_control_version(const cJSON *root){uint32_t n;return number(root,"version",1,1,&n);}

bool pet_control_binding(const cJSON *obj,pet_control_binding_t *out)
{
    if(!out||!keys(obj,"revision|relationshipId|buildId|sha256",4))return false;
    memset(out,0,sizeof(*out));
    if(!text(obj,"revision",out->revision,sizeof(out->revision),false)||
       !text(obj,"relationshipId",out->relationship_id,sizeof(out->relationship_id),true)||
       !text(obj,"buildId",out->build_id,sizeof(out->build_id),true)||
       !text(obj,"sha256",out->sha256,sizeof(out->sha256),true))return false;
    out->assigned=out->relationship_id[0]!=0;
    return out->assigned?(uuid(out->relationship_id)&&uuid(out->build_id)&&hash(out->sha256)):
        (!out->build_id[0]&&!out->sha256[0]);
}

bool pet_control_config(const cJSON *obj,pet_control_config_t *out)
{
    if(!out||!keys(obj,"version|settings",2))return false;
    memset(out,0,sizeof(*out));
    const cJSON *settings=field(obj,"settings");uint32_t n;
    if(!text(obj,"version",out->version,sizeof(out->version),false)||
       !keys(settings,"volume|brightness|shakeSensitivity|recordingTimeoutSeconds|animationProfile|aiPetId|faceId|speechProfile",8))return false;
    if(!number(settings,"volume",0,100,&n))return false;
    out->volume=(uint8_t)n;
    if(!number(settings,"brightness",10,100,&n))return false;
    out->brightness=(uint8_t)n;
    if(!number(settings,"shakeSensitivity",0,100,&n))return false;
    out->shake_sensitivity=(uint8_t)n;
    if(!number(settings,"recordingTimeoutSeconds",8,600,&n)||
       !(n==8||n==15||n==30||n==60||n==180||n==600))return false;
    out->recording_timeout=(uint16_t)n;
    const cJSON *animation_profile=field(settings,"animationProfile");
    pet_animation_profile_t parsed_animation_profile;
    if(!cJSON_IsString(animation_profile)||
       !pet_animation_profile_parse_wire(animation_profile->valuestring,&parsed_animation_profile))return false;
    out->animation_profile=(uint8_t)parsed_animation_profile;
    if(equal(settings,"speechProfile","cartesia-batch"))out->speech_profile=0;
    else if(equal(settings,"speechProfile","cartesia-realtime"))out->speech_profile=1;else return false;
    if(!text(settings,"aiPetId",out->ai_pet_id,sizeof(out->ai_pet_id),false)||
       !text(settings,"faceId",out->face_id,sizeof(out->face_id),false))return false;
    return strspn(out->ai_pet_id,"abcdefghijklmnopqrstuvwxyz0123456789._-")==strlen(out->ai_pet_id)&&
        out->face_id[0]>='a'&&out->face_id[0]<='z'&&strlen(out->face_id)>=3&&
        strspn(out->face_id,"abcdefghijklmnopqrstuvwxyz0123456789-")==strlen(out->face_id)&&
        out->face_id[strlen(out->face_id)-1]!='-';
}

bool pet_control_context(const cJSON *root,pet_control_context_t *out)
{
    if(!out||!pet_control_version(root)||
       !keys(root,"version|deviceId|accountId|selectionAllowed|binding|config|pendingInstallationId|nextPollSeconds",8))return false;
    memset(out,0,sizeof(*out));uint32_t n;
    if(!text(root,"deviceId",out->device_id,sizeof(out->device_id),false)||
       !text(root,"accountId",out->account_id,sizeof(out->account_id),false)||!uuid(out->account_id)||
       !boolean(root,"selectionAllowed",&out->selection_allowed)||
       !pet_control_binding(field(root,"binding"),&out->binding)||
       !pet_control_config(field(root,"config"),&out->config)||
       !text(root,"pendingInstallationId",out->pending_id,sizeof(out->pending_id),true)||
       !number(root,"nextPollSeconds",5,300,&n))return false;
    out->next_poll_seconds=(uint16_t)n;
    return !out->pending_id[0]||uuid(out->pending_id);
}

bool pet_control_library(const cJSON *root,pet_control_library_t *out)
{
    if(!out||!pet_control_version(root)||!keys(root,"version|items|nextCursor",3))return false;
    memset(out,0,sizeof(*out));const cJSON *items=field(root,"items");
    if(!cJSON_IsArray(items)||cJSON_GetArraySize(items)>PET_CONTROL_LIBRARY_MAX||
       !text(root,"nextCursor",out->next_cursor,sizeof(out->next_cursor),true))return false;
    for(const cJSON *item=items->child;item;item=item->next) {
        pet_control_library_item_t *p=&out->items[out->count++];
        if(!keys(item,"projectId|buildId|faceId|name|packVersion|sha256|bytes|renderer|selected|installable|unavailableCode",11)||
           !text(item,"projectId",p->project_id,sizeof(p->project_id),false)||!uuid(p->project_id)||
           !text(item,"buildId",p->build_id,sizeof(p->build_id),false)||!uuid(p->build_id)||
           !text(item,"faceId",p->face_id,sizeof(p->face_id),false)||
           !text(item,"name",p->name,sizeof(p->name),false)||
           !text(item,"packVersion",p->pack_version,sizeof(p->pack_version),false)||
           !text(item,"sha256",p->sha256,sizeof(p->sha256),false)||!hash(p->sha256)||
           !number(item,"bytes",1,PET_INSTALL_PACK_MAX,&p->bytes)||
           !equal(item,"renderer","frame-player-cloud-0.3")||
           !boolean(item,"selected",&p->selected)||!boolean(item,"installable",&p->installable)||
           !text(item,"unavailableCode",p->unavailable_code,sizeof(p->unavailable_code),true)||
           p->installable!=(p->unavailable_code[0]==0))return false;
    }
    return true;
}

bool pet_control_operation(const cJSON *obj,pet_control_operation_t *out)
{
    if(!out||!keys(obj,"installationId|buildId|sha256|bytes|state|downloadedBytes|manifest|downloadPath|resultingBinding",9))return false;
    memset(out,0,sizeof(*out));
    if(!text(obj,"installationId",out->id,sizeof(out->id),false)||!uuid(out->id)||
       !text(obj,"buildId",out->build_id,sizeof(out->build_id),false)||!uuid(out->build_id)||
       !text(obj,"sha256",out->sha256,sizeof(out->sha256),false)||!hash(out->sha256)||
       !number(obj,"bytes",1,PET_INSTALL_PACK_MAX,&out->bytes)||
       !number(obj,"downloadedBytes",0,out->bytes,&out->downloaded_bytes)||
       !text(obj,"downloadPath",out->download_path,sizeof(out->download_path),false))return false;
    static const char *states[]={"queued","downloading","verified","activating","installed","failed","cancelled"};
    unsigned i;for(i=0;i<7;++i)if(equal(obj,"state",states[i]))break;
    if(i==7)return false;
    out->state=(pet_control_op_state_t)i;
    if(i>=PET_OP_VERIFIED&&i<=PET_OP_INSTALLED&&out->downloaded_bytes!=out->bytes)return false;
    char expected[100]="/v1/device/installations/";
    strcat(expected,out->id);strcat(expected,"/pack");
    if(strcmp(expected,out->download_path))return false;
    const cJSON *binding=field(obj,"resultingBinding");
    if(out->state==PET_OP_INSTALLED) {
        if(!pet_control_binding(binding,&out->resulting_binding)||!out->resulting_binding.assigned||
           strcmp(out->build_id,out->resulting_binding.build_id)||strcmp(out->sha256,out->resulting_binding.sha256))return false;
    }else if(!cJSON_IsNull(binding))return false;
    const cJSON *manifest=field(obj,"manifest");
    if(!keys(manifest,"actorId|at|payload|algorithm|keyId|signature",6)||!equal(manifest,"algorithm","ES256")||
       !text(manifest,"payload",out->signed_payload,sizeof(out->signed_payload),false)||
       !text(manifest,"keyId",out->key_id,sizeof(out->key_id),false)||
       !text(manifest,"signature",out->signature,sizeof(out->signature),false))return false;
    return strlen(out->signature)==86&&
        strspn(out->signature,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")==86;
}

bool pet_control_same_binding(const pet_control_binding_t *a,const pet_control_binding_t *b)
{
    return a&&b&&a->assigned==b->assigned&&!strcmp(a->revision,b->revision)&&
        !strcmp(a->relationship_id,b->relationship_id)&&!strcmp(a->build_id,b->build_id)&&!strcmp(a->sha256,b->sha256);
}

bool pet_control_commit_install(pet_install_t *state,const pet_control_operation_t *operation,
                                 const pet_control_context_t *context)
{
    if(!operation||!context||operation->state!=PET_OP_INSTALLED||
       !pet_control_same_binding(&operation->resulting_binding,&context->binding))return false;
    return pet_install_commit(state,operation->id,operation->build_id,operation->sha256,
        context->binding.revision,context->binding.relationship_id,context->config.version);
}

bool pet_control_context_encode(const pet_control_context_t *c,char *json,size_t capacity)
{
    if(!c||!json)return false;
    cJSON *root=cJSON_CreateObject();if(!root)return false;
    cJSON_AddNumberToObject(root,"version",1);cJSON_AddStringToObject(root,"deviceId",c->device_id);
    cJSON_AddStringToObject(root,"accountId",c->account_id);cJSON_AddBoolToObject(root,"selectionAllowed",c->selection_allowed);
    cJSON_AddNumberToObject(root,"nextPollSeconds",c->next_poll_seconds);
    if(c->pending_id[0])cJSON_AddStringToObject(root,"pendingInstallationId",c->pending_id);else cJSON_AddNullToObject(root,"pendingInstallationId");
    cJSON *binding=cJSON_AddObjectToObject(root,"binding");
    cJSON_AddStringToObject(binding,"revision",c->binding.revision);
    const char *names[]={"relationshipId","buildId","sha256"};
    const char *values[]={c->binding.relationship_id,c->binding.build_id,c->binding.sha256};
    for(unsigned i=0;i<3;++i)if(c->binding.assigned)cJSON_AddStringToObject(binding,names[i],values[i]);else cJSON_AddNullToObject(binding,names[i]);
    cJSON *config=cJSON_AddObjectToObject(root,"config");cJSON_AddStringToObject(config,"version",c->config.version);
    cJSON *settings=cJSON_AddObjectToObject(config,"settings");
    cJSON_AddNumberToObject(settings,"volume",c->config.volume);cJSON_AddNumberToObject(settings,"brightness",c->config.brightness);
    cJSON_AddNumberToObject(settings,"shakeSensitivity",c->config.shake_sensitivity);cJSON_AddNumberToObject(settings,"recordingTimeoutSeconds",c->config.recording_timeout);
    const char *animation_profile=pet_animation_profile_wire_value(
        (pet_animation_profile_t)c->config.animation_profile);
    if(!animation_profile){cJSON_Delete(root);return false;}
    cJSON_AddStringToObject(settings,"animationProfile",animation_profile);
    cJSON_AddStringToObject(settings,"speechProfile",c->config.speech_profile==0?"cartesia-batch":c->config.speech_profile==1?"cartesia-realtime":"invalid");
    cJSON_AddStringToObject(settings,"aiPetId",c->config.ai_pet_id);cJSON_AddStringToObject(settings,"faceId",c->config.face_id);
    pet_control_context_t checked;bool ok=pet_control_context(root,&checked);
    char *text=ok?cJSON_PrintUnformatted(root):NULL;
    ok=text&&strlen(text)<capacity;if(ok)strcpy(json,text);
    free(text);cJSON_Delete(root);return ok;
}

static bool cached_context_valid(const pet_install_t *state)
{
    if(!state->cached_context[0])return true;
    cJSON *root=pet_control_json(state->cached_context,strlen(state->cached_context),sizeof(state->cached_context));
    pet_control_context_t cloud;bool ok=pet_control_context(root,&cloud)&&state->active_slot>=0;
    if(ok) {
        const pet_install_slot_t *slot=&state->slots[state->active_slot];
        ok=cloud.binding.assigned&&!strcmp(slot->build_id,cloud.binding.build_id)&&!strcmp(slot->sha256,cloud.binding.sha256)&&
            !strcmp(state->binding_revision,cloud.binding.revision)&&!strcmp(state->relationship_id,cloud.binding.relationship_id)&&
            !strcmp(state->config_version,cloud.config.version);
    }
    cJSON_Delete(root);return ok;
}

bool pet_control_install_encode(const pet_install_t *state,char *json,size_t capacity,size_t *bytes)
{
    if(!pet_install_valid(state)||!cached_context_valid(state)||!json||!bytes)return false;
    cJSON *root=cJSON_CreateObject();if(!root)return false;
    bool ok=cJSON_AddNumberToObject(root,"version",1)&&cJSON_AddNumberToObject(root,"phase",state->phase)&&
        cJSON_AddNumberToObject(root,"active",state->active_slot+1)&&cJSON_AddNumberToObject(root,"candidate",state->candidate_slot+1)&&
        cJSON_AddNumberToObject(root,"downloaded",state->downloaded_bytes)&&
        cJSON_AddStringToObject(root,"request",state->request_id)&&cJSON_AddStringToObject(root,"installation",state->installation_id)&&
        cJSON_AddStringToObject(root,"expected",state->expected_revision)&&cJSON_AddStringToObject(root,"binding",state->binding_revision)&&
        cJSON_AddStringToObject(root,"relationship",state->relationship_id)&&cJSON_AddStringToObject(root,"config",state->config_version)&&
        cJSON_AddStringToObject(root,"cachedContext",state->cached_context);
    cJSON *slots=cJSON_AddArrayToObject(root,"slots");ok=ok&&slots;
    for(unsigned i=0;ok&&i<2;++i) {
        cJSON *slot=cJSON_CreateObject();
        if(!slot){ok=false;break;}
        if(!cJSON_AddItemToArray(slots,slot)){cJSON_Delete(slot);ok=false;break;}
        ok=cJSON_AddNumberToObject(slot,"state",state->slots[i].state)&&
           cJSON_AddNumberToObject(slot,"bytes",state->slots[i].bytes)&&
           cJSON_AddStringToObject(slot,"build",state->slots[i].build_id)&&
           cJSON_AddStringToObject(slot,"hash",state->slots[i].sha256);
    }
    char *serialized=ok?cJSON_PrintUnformatted(root):NULL;
    ok=serialized&&strlen(serialized)<capacity;
    if(ok){*bytes=strlen(serialized);memcpy(json,serialized,*bytes+1);}
    free(serialized);cJSON_Delete(root);return ok;
}

static bool stored_text(const cJSON *root,const char *key,char *out,size_t capacity)
{
    const cJSON *v=field(root,key);
    if(!cJSON_IsString(v)||strlen(v->valuestring)>=capacity)return false;
    strcpy(out,v->valuestring);return true;
}

bool pet_control_install_decode(const char *json,size_t bytes,pet_install_t *state)
{
    if(!state)return false;
    cJSON *root=pet_control_json(json,bytes,4096);uint32_t n=0;pet_install_t parsed;
    pet_install_empty(&parsed);
    bool has_cache=field(root,"cachedContext")!=NULL;
    bool ok=pet_control_version(root)&&(has_cache?
        keys(root,"version|phase|active|candidate|downloaded|request|installation|expected|binding|relationship|config|slots|cachedContext",13):
        keys(root,"version|phase|active|candidate|downloaded|request|installation|expected|binding|relationship|config|slots",12));
    if(ok){ok=number(root,"phase",0,PET_INSTALL_ACTIVATING,&n);parsed.phase=(pet_install_phase_t)n;}
    if(ok){ok=number(root,"active",0,2,&n);parsed.active_slot=(int8_t)n-1;}
    if(ok){ok=number(root,"candidate",0,2,&n);parsed.candidate_slot=(int8_t)n-1;}
    ok=ok&&number(root,"downloaded",0,PET_INSTALL_PACK_MAX,&parsed.downloaded_bytes)&&
        stored_text(root,"request",parsed.request_id,sizeof(parsed.request_id))&&
        stored_text(root,"installation",parsed.installation_id,sizeof(parsed.installation_id))&&
        stored_text(root,"expected",parsed.expected_revision,sizeof(parsed.expected_revision))&&
        stored_text(root,"binding",parsed.binding_revision,sizeof(parsed.binding_revision))&&
        stored_text(root,"relationship",parsed.relationship_id,sizeof(parsed.relationship_id))&&
        stored_text(root,"config",parsed.config_version,sizeof(parsed.config_version));
    const cJSON *slots=field(root,"slots");
    ok=ok&&cJSON_IsArray(slots)&&cJSON_GetArraySize(slots)==2;
    for(unsigned i=0;ok&&i<2;++i) {
        const cJSON *slot=cJSON_GetArrayItem(slots,(int)i);
        ok=keys(slot,"state|bytes|build|hash",4)&&number(slot,"state",0,PET_SLOT_ACTIVE,&n);
        parsed.slots[i].state=(pet_slot_state_t)n;
        ok=ok&&number(slot,"bytes",0,PET_INSTALL_PACK_MAX,&parsed.slots[i].bytes)&&
            stored_text(slot,"build",parsed.slots[i].build_id,sizeof(parsed.slots[i].build_id))&&
            stored_text(slot,"hash",parsed.slots[i].sha256,sizeof(parsed.slots[i].sha256));
    }
    ok=ok&&(!has_cache||stored_text(root,"cachedContext",parsed.cached_context,sizeof(parsed.cached_context)))&&
        pet_install_valid(&parsed)&&cached_context_valid(&parsed);if(ok)*state=parsed;
    cJSON_Delete(root);return ok;
}
