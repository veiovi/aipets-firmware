#include "pet_replace_receipt.h"
#include <string.h>

static bool same_pack(const pet_replace_pack_t *a,const pet_replace_pack_t *b)
{return a->bytes==b->bytes&&!strcmp(a->build_id,b->build_id)&&!strcmp(a->sha256,b->sha256);}
static bool target(const pet_replace_operation_t *o,const pet_replace_report_t *r)
{return o&&r&&r->generation&&same_pack(&o->release.pack,&r->pack)&&
    !strcmp(o->id,r->operation_id)&&!strcmp(o->fence_id,r->fence_id);}
static bool same_receipt(const pet_replace_report_t *a,const pet_replace_report_t *b)
{
    return a->phase==b->phase&&a->generation==b->generation&&same_pack(&a->pack,&b->pack)&&
        !strcmp(a->operation_id,b->operation_id)&&!strcmp(a->fence_id,b->fence_id)&&
        a->downloaded_bytes==b->downloaded_bytes&&!strcmp(a->prefix_sha256,b->prefix_sha256)&&
        !strcmp(a->binding_revision,b->binding_revision)&&!strcmp(a->relationship_id,b->relationship_id)&&
        !strcmp(a->config_version,b->config_version);
}
bool pet_replace_receipt_make(const pet_replace_journal_t *j,const pet_replace_operation_t *o,pet_replace_report_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));
    if(!j||!o||!j->ready||!j->journal.loaded||!j->journal.generation||!pet_replace_valid(&j->state))return false;
    const pet_replace_t *s=&j->state;pet_replace_report_t r={0};
    if(s->phase==PET_REPLACE_ACTIVE){
        if(!o->has_result||(o->phase!=PET_CLOUD_ACTIVATING&&o->phase!=PET_CLOUD_INSTALLED)||!o->binding.assigned||
           !same_pack(&s->active,&o->release.pack)||strcmp(s->binding_revision,o->binding.revision)||
           strcmp(s->relationship_id,o->binding.relationship_id)||strcmp(s->config_version,o->config.version)||
           strcmp(o->binding.build_id,s->active.build_id)||strcmp(o->binding.sha256,s->active.sha256))return false;
        r.phase=PET_CLOUD_INSTALLED;r.pack=s->active;r.downloaded_bytes=s->active.bytes;
        strcpy(r.prefix_sha256,s->active.sha256);strcpy(r.binding_revision,s->binding_revision);
        strcpy(r.relationship_id,s->relationship_id);strcpy(r.config_version,s->config_version);
    }else{
        if(s->phase<PET_REPLACE_INVALIDATED||s->phase>PET_REPLACE_RECOVERY||
           !same_pack(&s->target,&o->release.pack)||strcmp(s->operation_id,o->id)||strcmp(s->fence_id,o->fence_id)||
           strcmp(s->request_id,o->request_id)||strcmp(s->expected_revision,o->expected_revision))return false;
        switch(s->phase){
            case PET_REPLACE_INVALIDATED:r.phase=PET_CLOUD_INVALIDATED;break;
            case PET_REPLACE_DOWNLOADING:r.phase=PET_CLOUD_DOWNLOADING;break;
            case PET_REPLACE_VERIFIED:r.phase=PET_CLOUD_VERIFIED;break;
            case PET_REPLACE_ACTIVATING:r.phase=PET_CLOUD_ACTIVATING;break;
            case PET_REPLACE_RECOVERY:r.phase=PET_CLOUD_RECOVERY;break;
            default:return false;
        }
        r.pack=s->target;r.downloaded_bytes=s->downloaded_bytes;strcpy(r.prefix_sha256,s->prefix_sha256);
    }
    r.generation=j->journal.generation;strcpy(r.operation_id,o->id);strcpy(r.fence_id,o->fence_id);
    if(o->supersedes&&r.generation<=o->previous_generation)return false;
    *out=r;return true;
}
bool pet_replace_receipt_acknowledged(const pet_replace_operation_t *o,const pet_replace_report_t *r)
{return target(o,r)&&o->has_report&&o->phase==r->phase&&same_receipt(&o->report,r);}
pet_replace_receipt_action_t pet_replace_receipt_classify(const pet_replace_operation_t *o,const pet_replace_report_t *r)
{
    if(!target(o,r)||(o->supersedes&&r->generation<=o->previous_generation))return PET_RECEIPT_REJECT;
    const pet_replace_report_t *last=o->has_report?&o->report:NULL;
    if(last){
        if(r->generation==last->generation)return same_receipt(r,last)?PET_RECEIPT_DUPLICATE:PET_RECEIPT_REJECT;
        if(r->generation<last->generation)return PET_RECEIPT_REJECT;
    }
    if(!o->fence_confirmed||!o->flash_reserved||
       (o->cancel_requested&&r->phase!=PET_CLOUD_RECOVERY&&r->phase!=PET_CLOUD_INVALIDATED))return PET_RECEIPT_REJECT;
    bool allowed=false;
    switch(o->phase){
        case PET_CLOUD_FENCED:allowed=r->phase==PET_CLOUD_INVALIDATED||r->phase==PET_CLOUD_RECOVERY;break;
        case PET_CLOUD_INVALIDATED:allowed=r->phase==PET_CLOUD_INVALIDATED||r->phase==PET_CLOUD_DOWNLOADING||r->phase==PET_CLOUD_RECOVERY;break;
        case PET_CLOUD_DOWNLOADING:allowed=r->phase==PET_CLOUD_INVALIDATED||r->phase==PET_CLOUD_DOWNLOADING||r->phase==PET_CLOUD_VERIFIED||r->phase==PET_CLOUD_RECOVERY;break;
        case PET_CLOUD_VERIFIED:allowed=r->phase==PET_CLOUD_ACTIVATING||r->phase==PET_CLOUD_RECOVERY;break;
        case PET_CLOUD_ACTIVATING:allowed=r->phase==PET_CLOUD_INSTALLED;break;
        default:break;
    }
    if(!allowed)return PET_RECEIPT_REJECT;
    if(last&&(r->phase==PET_CLOUD_DOWNLOADING||(r->phase==PET_CLOUD_RECOVERY&&o->phase!=PET_CLOUD_FENCED))&&
       (r->downloaded_bytes<last->downloaded_bytes||(r->downloaded_bytes==last->downloaded_bytes&&strcmp(r->prefix_sha256,last->prefix_sha256))))return PET_RECEIPT_REJECT;
    if(r->phase==PET_CLOUD_INSTALLED&&(!o->has_result||strcmp(r->binding_revision,o->binding.revision)||
       strcmp(r->relationship_id,o->binding.relationship_id)||strcmp(r->config_version,o->config.version)))return PET_RECEIPT_REJECT;
    return PET_RECEIPT_ADVANCE;
}
