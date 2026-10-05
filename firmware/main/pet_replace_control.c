#include "pet_replace_control.h"
#include <string.h>
#include "cJSON.h"

static pet_replace_control_status_t status(pet_replace_control_t *w,pet_replace_control_status_t s,const char *error)
{w->status=s;if(error){strncpy(w->error_code,error,sizeof(w->error_code)-1);w->error_code[80]=0;}else w->error_code[0]=0;return s;}
static bool same_pack(const pet_replace_pack_t *a,const pet_replace_pack_t *b)
{return a->bytes==b->bytes&&!strcmp(a->build_id,b->build_id)&&!strcmp(a->sha256,b->sha256);}
static bool same_operation(const pet_replace_operation_t *a,const pet_replace_operation_t *b)
{
    return !strcmp(a->id,b->id)&&!strcmp(a->request_id,b->request_id)&&!strcmp(a->fence_id,b->fence_id)&&
        !strcmp(a->expected_revision,b->expected_revision)&&!strcmp(a->signed_payload,b->signed_payload)&&
        !strcmp(a->key_id,b->key_id)&&!strcmp(a->signature,b->signature)&&a->supersedes==b->supersedes&&
        (!a->supersedes||(!strcmp(a->previous_id,b->previous_id)&&!strcmp(a->previous_fence_id,b->previous_fence_id)&&
         a->previous_generation==b->previous_generation));
}
static uint64_t completed(pet_replace_control_t *w,uint64_t started)
{uint64_t now=w->config.now_ms(w->config.context);return now>started?now:started;}
void pet_replace_control_disconnect(pet_replace_control_t *w)
{
    if(!w)return;
    if(w->admitted)w->config.freeze(w->config.context);
    w->admitted=false;w->authenticated=false;w->next_poll_ms=0;
    pet_control_download_close(&w->download);
}
static pet_replace_control_status_t conflict(pet_replace_control_t *w,uint64_t now,const char *error)
{
    pet_replace_control_disconnect(w);w->retry_at_ms=completed(w,now)+15000;
    return status(w,PET_REPLACE_CONTROL_RECOVERY,error);
}
/* The worker's backoff: the answer's Retry-After, else the transport's default. */
static void back_off(pet_replace_control_t *w,uint64_t now,const pet_control_http_result_t *result)
{w->retry_at_ms=completed(w,now)+(uint64_t)(result->retry_seconds?result->retry_seconds:15)*1000;}
static pet_replace_control_status_t network_wait(pet_replace_control_t *w,uint64_t now,const pet_control_http_result_t *result)
{
    /* A failed management poll cannot revoke an admitted, idle pet's Brain.
     * Keep only voice/display admission; no flash work proceeds until a fresh
     * authenticated poll. Explicit refusals still freeze immediately. */
    if (w->admitted && !w->cloud.has_operation &&
        (result->status == 0 || result->status == 429 || result->status >= 500))
    {
        w->authenticated = false;
        w->next_poll_ms = 0;
        pet_control_download_close(&w->download);
        back_off(w, now, result);
        return status(w, PET_REPLACE_CONTROL_WAITING, "PET_CONTROL_UNAVAILABLE");
    }
    pet_replace_control_disconnect(w);back_off(w,now,result);
    return status(w,PET_REPLACE_CONTROL_WAITING,"PET_CONTROL_UNAVAILABLE");
}
static bool ready(const pet_replace_control_t *w)
{return w->store&&w->store->ready&&w->store->journal.loaded&&w->store->journal.generation&&pet_replace_valid(&w->store->state);}
static bool save(pet_replace_control_t *w,const pet_replace_t *next)
{
    if(pet_replace_journal_commit(w->store,next)==PET_JOURNAL_OK)return true;
    w->config.freeze(w->config.context);w->admitted=false;w->manifest_ready=w->verified=false;
    status(w,PET_REPLACE_CONTROL_RECOVERY,"PET_JOURNAL_STORAGE");return false;
}
static bool local_target(const pet_replace_t *s,const pet_replace_operation_t *o)
{
    return same_pack(&s->target,&o->release.pack)&&!strcmp(s->request_id,o->request_id)&&
        !strcmp(s->expected_revision,o->expected_revision)&&
        (s->phase==PET_REPLACE_REQUESTED||(!strcmp(s->operation_id,o->id)&&!strcmp(s->fence_id,o->fence_id)));
}
static bool compatible(pet_replace_control_t *w)
{
    const pet_replace_operation_t *o=&w->cloud.operation;
    return w->writer&&w->writer->initialized&&w->writer->store==w->store&&
        o->release.pack.bytes<=w->store->state.capacity_bytes&&w->config.compatible_healthy(w->config.context,&o->release);
}
bool pet_replace_control_init(pet_replace_control_t *w,const pet_replace_control_config_t *c,
                               pet_replace_journal_t *store,pet_replace_writer_t *writer)
{
    if(!w||!c||!store||!c->http.origin||!c->http.device_id||!c->http.credential||
       !c->compatible_healthy||!c->verify||!c->activate||!c->freeze||!c->now_ms||(c->key_count&&!c->keys)||
       !memchr(c->boot_id,0,sizeof(c->boot_id)))return false;
    char probe[128];if(!pet_replace_wire_encode_poll(c->boot_id,probe,sizeof(probe)))return false;
    memset(w,0,sizeof(*w));w->config=*c;w->store=store;w->writer=writer;return true;
}
/* The authenticated v2 poll result in `incoming` becomes the cloud state. */
static pet_replace_control_status_t adopt(pet_replace_control_t *w,uint64_t now)
{
    bool changed=!w->has_context||w->cloud.has_operation!=w->incoming.has_operation||
        (w->cloud.has_operation&&!same_operation(&w->cloud.operation,&w->incoming.operation));
    if(changed){w->manifest_ready=w->verified=false;memset(&w->verified_pack,0,sizeof(w->verified_pack));}
    if(w->admitted&&(changed||!pet_control_same_binding(&w->cloud.context.binding,&w->incoming.context.binding)||
        strcmp(w->cloud.context.config.version,w->incoming.context.config.version))){
        /* A selection changes only the binding: a device that can rebind keeps its conversation open. */
        if(!changed&&w->config.pause)w->config.pause(w->config.context);else w->config.freeze(w->config.context);
        w->admitted=false;
    }
    w->cloud=w->incoming;w->has_context=w->authenticated=true;w->next_poll_ms=completed(w,now)+15000;
    return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
}
static pet_replace_control_status_t poll(pet_replace_control_t *w,uint64_t now)
{
    bool encoded = w->config.remove ?
        pet_replace_wire_encode_removal_poll(w->config.boot_id, w->removal_ack, w->removal_ack_count,
                                             w->request, sizeof(w->request)) :
        pet_replace_wire_encode_poll(w->config.boot_id, w->request, sizeof(w->request));
    if (!encoded)
        return conflict(w, now, "PET_POLL_INVALID");
    pet_control_http_result_t result={0};
    if(!pet_control_http_json(&w->config.http,"/v2/device/pets/poll",w->request,w->response,sizeof(w->response),&result)||result.status!=200)
        return network_wait(w,now,&result);
    if(!pet_replace_wire_poll(w->response,result.bytes,w->config.http.device_id,NULL,
        w->config.keys,w->config.key_count,&w->incoming) || (w->incoming.has_removals && !w->config.remove))
        return conflict(w,now,"PET_OPERATION_UNTRUSTED");
    /* The authenticated poll names the device's account. A new one (the device
     * moved to another account) is taken as at boot: the old account's
     * conversation and verified pack end here, and its signed slot records no
     * longer verify against the new account. */
    if(w->has_context&&strcmp(w->cloud.context.account_id,w->incoming.context.account_id)){
        pet_replace_control_disconnect(w);w->has_context=false;
    }
    w->removal_ack_count = 0; /* A valid reply accepted this poll's acknowledgements. */
    return adopt(w,now);
}
pet_replace_select_t pet_replace_control_select(pet_replace_control_t *w,uint64_t now,const pet_replace_pack_t *pack,int *http)
{
    pet_control_http_result_t result={0};
    if(http)*http=0;
    if(!w||!pack||!w->has_context||!w->authenticated||now<w->retry_at_ms||
       w->cloud.removal_count||w->removal_ack_count)return PET_REPLACE_SELECT_UNAVAILABLE;
    if(!pet_replace_wire_encode_select(w->config.boot_id,pack->build_id,pack->sha256,w->request,sizeof(w->request)))
        return PET_REPLACE_SELECT_REFUSED; /* Not a release ID: nothing the cloud could select. */
    bool answered=pet_control_http_json(&w->config.http,"/v2/device/pets/select",w->request,w->response,sizeof(w->response),&result);
    if(http&&answered)*http=result.status;
    /* No answer, or one to retry (429, 5xx): ask again after the backoff. */
    if(!answered||result.status==429||result.status>=500){back_off(w,now,&result);return PET_REPLACE_SELECT_UNAVAILABLE;}
    if(result.status!=200)return PET_REPLACE_SELECT_REFUSED;
    /* Only a binding of exactly this pet, between installations, is a selection. */
    const pet_control_binding_t *b=&w->incoming.context.binding;
    if(!pet_replace_wire_poll(w->response,result.bytes,w->config.http.device_id,w->cloud.context.account_id,
        w->config.keys,w->config.key_count,&w->incoming)||w->incoming.has_operation||w->incoming.has_removals||!b->assigned||
       strcmp(b->build_id,pack->build_id)||strcmp(b->sha256,pack->sha256))return PET_REPLACE_SELECT_REFUSED;
    adopt(w,now);return PET_REPLACE_SELECT_CHOSEN;
}
static bool encode_fence(pet_replace_control_t *w)
{
    const pet_replace_operation_t *o=&w->cloud.operation;const pet_replace_pack_t *p=&o->release.pack;
    cJSON *root=cJSON_CreateObject();bool ok=root&&cJSON_AddNumberToObject(root,"version",2)&&
        cJSON_AddStringToObject(root,"installationId",o->id)&&cJSON_AddStringToObject(root,"fenceId",o->fence_id)&&
        cJSON_AddStringToObject(root,"buildId",p->build_id)&&cJSON_AddStringToObject(root,"sha256",p->sha256)&&
        cJSON_AddNumberToObject(root,"bytes",p->bytes)&&cJSON_AddStringToObject(root,"expectedBindingRevision",o->expected_revision)&&
        cJSON_PrintPreallocated(root,w->request,sizeof(w->request),false);
    cJSON_Delete(root);return ok;
}
static pet_replace_control_status_t fence(pet_replace_control_t *w,uint64_t now)
{
    if(!encode_fence(w))return conflict(w,now,"PET_FENCE_ENCODING");
    pet_control_http_result_t result={0};
    if(!pet_control_http_json(&w->config.http,"/v2/device/pets/fence",w->request,w->response,sizeof(w->response),&result)||result.status!=200)
        return network_wait(w,now,&result);
    pet_replace_operation_t *reply=&w->incoming.operation;
    if(!pet_replace_wire_operation(w->response,result.bytes,w->config.http.device_id,w->cloud.context.account_id,
        w->config.keys,w->config.key_count,reply)||!same_operation(&w->cloud.operation,reply)||
        reply->phase!=PET_CLOUD_FENCED||!reply->fence_confirmed||!reply->flash_reserved||
        (w->cloud.operation.cancel_requested&&!reply->cancel_requested))return conflict(w,now,"PET_FENCE_CONFLICT");
    w->cloud.operation=*reply;w->authenticated=true;
    w->config.freeze(w->config.context);w->admitted=false;
    return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
}
static pet_replace_control_status_t report(pet_replace_control_t *w,uint64_t now,const pet_replace_report_t *receipt)
{
    if(!pet_replace_wire_encode_report(receipt,w->request,sizeof(w->request)))return conflict(w,now,"PET_RECEIPT_INVALID");
    pet_control_http_result_t result={0};
    if(!pet_control_http_json(&w->config.http,"/v2/device/pets/report",w->request,w->response,sizeof(w->response),&result)||result.status!=200)
        return network_wait(w,now,&result);
    pet_replace_operation_t *reply=&w->incoming.operation;
    if(!pet_replace_wire_operation(w->response,result.bytes,w->config.http.device_id,w->cloud.context.account_id,
        w->config.keys,w->config.key_count,reply)||!same_operation(&w->cloud.operation,reply)||
        !pet_replace_receipt_acknowledged(reply,receipt)||(w->cloud.operation.cancel_requested&&!reply->cancel_requested))
        return conflict(w,now,"PET_RECEIPT_CONFLICT");
    w->cloud.operation=*reply;w->authenticated=true;
    if(receipt->phase==PET_CLOUD_INSTALLED)w->next_poll_ms=0; // Obtain current context, not the pre-activation one.
    return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
}
static bool record(pet_replace_control_t *w,bool write)
{
    const pet_replace_operation_t *o=&w->cloud.operation;
    cJSON *root=cJSON_CreateObject();bool ok=root&&cJSON_AddStringToObject(root,"algorithm","ES256")&&
        cJSON_AddStringToObject(root,"keyId",o->key_id)&&cJSON_AddStringToObject(root,"payload",o->signed_payload)&&
        cJSON_AddStringToObject(root,"signature",o->signature)&&pet_release_v2_record_encode(root,w->cloud.context.account_id,
            &o->release.pack,w->config.keys,w->config.key_count,w->chunk,PET_REPLACE_MANIFEST_BYTES);
    cJSON_Delete(root);if(!ok)return false;
    if(write)return pet_replace_writer_manifest(w->writer,w->chunk,PET_REPLACE_MANIFEST_BYTES);
    return w->writer->io.read(w->writer->io.context,w->store->state.capacity_bytes,
        w->chunk+PET_REPLACE_MANIFEST_BYTES,PET_REPLACE_MANIFEST_BYTES)&&
        !memcmp(w->chunk,w->chunk+PET_REPLACE_MANIFEST_BYTES,PET_REPLACE_MANIFEST_BYTES);
}
static bool verified(pet_replace_control_t *w)
{
    if(w->verified&&same_pack(&w->verified_pack,&w->cloud.operation.release.pack))return true;
    if(!record(w,false)||!w->config.verify(w->config.context,&w->cloud.operation.release))return false;
    w->verified=true;w->verified_pack=w->cloud.operation.release.pack;return true;
}
static pet_replace_control_status_t recover(pet_replace_control_t *w,const char *error)
{
    w->config.freeze(w->config.context);w->admitted=false;w->manifest_ready=w->verified=false;
    pet_replace_t next=w->store->state;
    if(next.phase!=PET_REPLACE_RECOVERY&&(!pet_replace_cancel(&next)||!save(w,&next)))
        return status(w,PET_REPLACE_CONTROL_RECOVERY,"PET_RECOVERY_RECONCILE");
    return status(w,PET_REPLACE_CONTROL_RECOVERY,error);
}
static pet_replace_control_status_t mount(pet_replace_control_t *w,uint64_t now)
{
    const pet_replace_t *s=&w->store->state;const pet_control_binding_t *b=&w->cloud.context.binding;
    if(s->phase==PET_REPLACE_EMPTY)return status(w,PET_REPLACE_CONTROL_IDLE,NULL);
    if(s->phase!=PET_REPLACE_ACTIVE||!b->assigned||strcmp(b->revision,s->binding_revision)||
       strcmp(b->relationship_id,s->relationship_id)||strcmp(b->build_id,s->active.build_id)||strcmp(b->sha256,s->active.sha256))
        return conflict(w,now,"PET_ACTIVE_BINDING_CONFLICT");
    if(!w->admitted){
        if(!w->config.rebind||!w->config.rebind(w->config.context,&w->cloud.context)){
            w->config.freeze(w->config.context);
            if(!w->config.activate(w->config.context,&w->cloud.context))return conflict(w,now,"PET_ACTIVE_UNAVAILABLE");
        }
        w->admitted=true;
    }
    return status(w,PET_REPLACE_CONTROL_READY,NULL);
}
pet_replace_control_status_t pet_replace_control_step(pet_replace_control_t *w,uint64_t now,bool allow_flash)
{
    if(!w||!w->store)return PET_REPLACE_CONTROL_RECOVERY;
    if(!allow_flash||!ready(w)||w->store->state.phase!=PET_REPLACE_DOWNLOADING||!w->cloud.has_operation||
       w->cloud.operation.cancel_requested)pet_control_download_end(&w->download,false);
    if((!allow_flash||!ready(w))&&w->admitted){w->config.freeze(w->config.context);w->admitted=false;}
    if(!ready(w))w->manifest_ready=w->verified=false;
    if(now<w->retry_at_ms)return w->status;
    if(!w->has_context||!w->next_poll_ms||now>=w->next_poll_ms)return poll(w,now);
    if(!ready(w))return status(w,PET_REPLACE_CONTROL_RECOVERY,"PET_JOURNAL_STORAGE");
    pet_replace_t next=w->store->state;pet_replace_operation_t *o=&w->cloud.operation;
    if(!w->cloud.has_operation){
        if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
        if(next.phase==PET_REPLACE_REQUESTED){
            if(strcmp(next.expected_revision,w->cloud.context.binding.revision))return conflict(w,now,"PET_CANCEL_CONFLICT");
            if(!pet_replace_cancel(&next)||!save(w,&next))return w->status;
        }
        if (w->cloud.removal_count)
        {
            const pet_replace_removal_t *r = &w->cloud.removals[0];
            if (!w->config.remove || w->removal_ack_count >= PET_REMOVAL_MAX ||
                (w->cloud.context.binding.assigned &&
                 !strcmp(w->cloud.context.binding.build_id, r->pack.build_id)) ||
                !w->config.remove(w->config.context, &r->pack))
                return conflict(w, now, "PET_REMOVAL_FAILED");
            strcpy(w->removal_ack[w->removal_ack_count++], r->id);
            --w->cloud.removal_count;
            memmove(w->cloud.removals, w->cloud.removals + 1,
                    w->cloud.removal_count * sizeof(*w->cloud.removals));
            if (!w->cloud.removal_count)
                w->next_poll_ms = 0;
            return status(w, PET_REPLACE_CONTROL_WORKING, NULL);
        }
        return mount(w,now);
    }
    /* Retry/supersession is explicit cloud authority, not interpretation of an
     * old recovery report as permission to resume erasing. */
    if(next.phase==PET_REPLACE_RECOVERY&&o->phase==PET_CLOUD_FENCED){
        pet_replace_report_t pending;
        if(local_target(&next,o)&&pet_replace_receipt_make(w->store,o,&pending)&&
           pet_replace_receipt_classify(o,&pending)==PET_RECEIPT_ADVANCE)return report(w,now,&pending);
        if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
        if(!o->cancel_requested&&!compatible(w))return conflict(w,now,"PET_RELEASE_INCOMPATIBLE");
        if(!o->fence_confirmed)return fence(w,now);
        if(o->supersedes&&strcmp(next.operation_id,o->id)){
            if(strcmp(next.operation_id,o->previous_id)||strcmp(next.fence_id,o->previous_fence_id)||
               w->store->journal.generation!=o->previous_generation)return conflict(w,now,"PET_SUPERSESSION_CONFLICT");
            pet_replace_t replacement;
            if(!pet_replace_empty(&replacement,next.capacity_bytes)||!pet_replace_request(&replacement,o->request_id,&o->release.pack,o->expected_revision)||
               !pet_replace_fenced(&replacement,o->id,o->fence_id,&o->release.pack)||
               !pet_replace_supersede(&next,o->previous_id,o->previous_fence_id,&replacement))return conflict(w,now,"PET_SUPERSESSION_CONFLICT");
        }else{
            pet_replace_report_t receipt;
            if(!local_target(&next,o)||!pet_replace_receipt_make(w->store,o,&receipt)||
               pet_replace_receipt_classify(o,&receipt)!=PET_RECEIPT_DUPLICATE)return conflict(w,now,"PET_RETRY_CONFLICT");
            if(!o->cancel_requested&&!pet_replace_retry(&next,o->id,o->fence_id,&o->release.pack))return conflict(w,now,"PET_RETRY_CONFLICT");
            /* A cancelled fresh retry needs a NEW recovery receipt to release
             * its reacquired lease; replaying the old generation has no effect. */
        }
        w->config.freeze(w->config.context);w->admitted=false;w->manifest_ready=w->verified=false;
        if(!save(w,&next))return w->status;
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    if(next.phase==PET_REPLACE_REQUESTED&&o->phase==PET_CLOUD_QUEUED&&strcmp(next.request_id,o->request_id)){
        /* A pre-fence cancellation and a new queue may happen between polls.
         * No erases were authorized by REQUESTED; keep the previous active
         * pack and reconcile only against the unchanged authenticated binding. */
        if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
        if(strcmp(next.expected_revision,w->cloud.context.binding.revision)||
           strcmp(o->expected_revision,w->cloud.context.binding.revision))return conflict(w,now,"PET_REQUEST_REPLACED_CONFLICT");
        if(!pet_replace_cancel(&next)||!save(w,&next))return w->status;
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    if(next.phase==PET_REPLACE_EMPTY||next.phase==PET_REPLACE_ACTIVE){
        if(o->phase==PET_CLOUD_QUEUED){
            if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
            if(strcmp(o->expected_revision,w->cloud.context.binding.revision)||!compatible(w))return conflict(w,now,"PET_REQUEST_INCOMPATIBLE");
            if(!pet_replace_request(&next,o->request_id,&o->release.pack,o->expected_revision)||!save(w,&next))return w->status;
            w->config.freeze(w->config.context);w->admitted=false;
            return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
        }
        if(next.phase!=PET_REPLACE_ACTIVE||(o->phase!=PET_CLOUD_ACTIVATING&&o->phase!=PET_CLOUD_INSTALLED))
            return conflict(w,now,"PET_OPERATION_CONFLICT");
    }else if(!local_target(&next,o))return conflict(w,now,"PET_OPERATION_CONFLICT");
    if(next.phase==PET_REPLACE_REQUESTED){
        if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
        if(o->phase==PET_CLOUD_CANCELLED){
            if(!pet_replace_cancel(&next)||!save(w,&next))return w->status;
            w->next_poll_ms=0;return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
        }
        if((o->phase!=PET_CLOUD_QUEUED&&o->phase!=PET_CLOUD_FENCED)||
           (!o->cancel_requested&&!compatible(w)))return conflict(w,now,"PET_FENCE_INCOMPATIBLE");
        if(!o->fence_confirmed)return fence(w,now);
        if(!pet_replace_fenced(&next,o->id,o->fence_id,&o->release.pack)||!save(w,&next))return w->status;
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    if(o->cancel_requested&&next.phase!=PET_REPLACE_RECOVERY&&next.phase!=PET_REPLACE_ACTIVE){
        if(next.phase==PET_REPLACE_INVALIDATED){
            pet_replace_report_t reset;
            if(!pet_replace_receipt_make(w->store,o,&reset))return conflict(w,now,"PET_RECEIPT_IDENTITY");
            if(!pet_replace_receipt_acknowledged(o,&reset)){
                if(pet_replace_receipt_classify(o,&reset)==PET_RECEIPT_REJECT)return conflict(w,now,"PET_RECEIPT_CONFLICT");
                return report(w,now,&reset);
            }
        }
        if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
        return recover(w,"PET_CANCELLED_RECOVERY");
    }
    if(next.phase==PET_REPLACE_FENCED){
        if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
        if(o->phase!=PET_CLOUD_FENCED||!o->fence_confirmed||!o->flash_reserved||!compatible(w))return conflict(w,now,"PET_INVALIDATION_CONFLICT");
        w->config.freeze(w->config.context);w->admitted=false;
        if(!pet_replace_invalidate(&next)||!save(w,&next))return w->status;
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    pet_replace_report_t receipt;
    if(!pet_replace_receipt_make(w->store,o,&receipt))return conflict(w,now,"PET_RECEIPT_IDENTITY");
    if(!pet_replace_receipt_acknowledged(o,&receipt)){
        if(pet_replace_receipt_classify(o,&receipt)==PET_RECEIPT_REJECT)return conflict(w,now,"PET_RECEIPT_CONFLICT");
        return report(w,now,&receipt);
    }
    if(next.phase==PET_REPLACE_RECOVERY)return status(w,PET_REPLACE_CONTROL_RECOVERY,"PET_REPLACEMENT_RECOVERY");
    if(!allow_flash)return status(w,PET_REPLACE_CONTROL_WAITING,"PET_FIRMWARE_BUSY");
    if(next.phase==PET_REPLACE_ACTIVE){w->next_poll_ms=0;return status(w,PET_REPLACE_CONTROL_WORKING,NULL);}
    if(!compatible(w))return conflict(w,now,"PET_RELEASE_INCOMPATIBLE");
    if(next.phase==PET_REPLACE_INVALIDATED){
        if(!pet_replace_writer_resume(w->writer))return recover(w,"PET_DETACH_FAILED");
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    if(next.phase==PET_REPLACE_DOWNLOADING){
        if(!w->writer->resume_checked){
            if(!pet_replace_writer_resume(w->writer)){
                if(ready(w)&&w->store->state.phase==PET_REPLACE_INVALIDATED)return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
                return recover(w,"PET_RESUME_FAILED");
            }
            return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
        }
        if(!w->manifest_ready){
            if(!record(w,next.downloaded_bytes==0))return recover(w,"PET_MANIFEST_STORAGE");
            w->manifest_ready=true;return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
        }
        if(next.downloaded_bytes==next.target.bytes){
            pet_control_download_end(&w->download,true);
            if(!verified(w))return recover(w,"PET_PACK_INVALID");
            if(!pet_replace_verified(&next,next.target.sha256)||!save(w,&next))return w->status;
            return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
        }
        uint32_t offset=next.downloaded_bytes;size_t bytes=next.target.bytes-offset;if(bytes>sizeof(w->chunk))bytes=sizeof(w->chunk);
        pet_control_http_result_t result={0};
        if(!pet_control_http_pet_range(&w->config.http,&w->download,o->id,o->release.pack.sha256,o->release.pack.bytes,
            offset,w->chunk,bytes,&result))return network_wait(w,now,&result);
        w->verified=false;
        if(!pet_replace_writer_block(w->writer,w->chunk,bytes))return recover(w,"PET_WRITE_FAILED");
        pet_control_download_wrote(&w->download);
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    if(next.phase==PET_REPLACE_VERIFIED){
        if(!verified(w))return recover(w,"PET_PACK_INVALID");
        if(!pet_replace_activate(&next)||!save(w,&next))return w->status;
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    if(next.phase==PET_REPLACE_ACTIVATING){
        if(!verified(w)||!o->has_result)return conflict(w,now,"PET_ACTIVATION_RECONCILE");
        if(!pet_replace_commit(&next,o->id,o->fence_id,&o->release.pack,o->binding.revision,o->binding.relationship_id,o->config.version)||
           !save(w,&next))return w->status;
        return status(w,PET_REPLACE_CONTROL_WORKING,NULL);
    }
    return conflict(w,now,"PET_STATE_INVALID");
}
