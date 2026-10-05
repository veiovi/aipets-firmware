#include "pet_sync.h"
#include "pet_board.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static void freeze(pet_sync_t *s)
{ s->runtime.freeze(s->runtime.context);s->admitted=false; }

static cJSON *call(pet_sync_t *s,const char *path,const cJSON *body)
{
    s->http_status=0;s->error_code[0]=0;
    char *request=body?cJSON_PrintUnformatted(body):NULL;
    if(body&&!request)return NULL;
    char *buffer=malloc(PET_CONTROL_RESPONSE_MAX);
    pet_control_http_result_t result={.retry_seconds=30};cJSON *root=NULL;
    if(buffer&&pet_control_http_json(&s->http,path,request,buffer,PET_CONTROL_RESPONSE_MAX,&result))
        root=pet_control_json(buffer,result.bytes,PET_CONTROL_RESPONSE_MAX);
    free(buffer);free(request);
    s->retry_seconds=result.retry_seconds;
    s->http_status=result.status;
    if(root&&pet_control_version(root)&&result.status>=400) {
        const cJSON *error=cJSON_GetObjectItemCaseSensitive(root,"error");
        const cJSON *code=cJSON_GetObjectItemCaseSensitive(error,"code");
        if(cJSON_IsString(code)&&strlen(code->valuestring)<sizeof(s->error_code)&&
           cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(error,"retryable")))strcpy(s->error_code,code->valuestring);
    }
    if(result.status==401||result.status==403) {freeze(s);s->status=PET_SYNC_AUTH;}
    if(root&&(!pet_control_version(root)||result.status<200||result.status>=300)){cJSON_Delete(root);root=NULL;}
    return root;
}

static bool context(pet_sync_t *s)
{
    cJSON *root=call(s,"/v1/device/context",NULL);
    pet_control_context_t cloud;
    bool ok=pet_control_context(root,&cloud)&&!strcmp(cloud.device_id,s->http.device_id);
    cJSON_Delete(root);
    if(!ok){freeze(s);return false;}
    if(s->cloud.account_id[0]&&strcmp(s->cloud.account_id,cloud.account_id)) {
        freeze(s);s->status=PET_SYNC_AUTH;return false;
    }
    if(s->has_context&&(!pet_control_same_binding(&s->cloud.binding,&cloud.binding)||
        strcmp(s->cloud.config.version,cloud.config.version)))freeze(s);
    s->cloud=cloud;s->has_context=true;s->runtime.healthy(s->runtime.context);
    return true;
}

static bool hello(pet_sync_t *s)
{
    cJSON *body=cJSON_CreateObject();if(!body)return false;
    cJSON_AddStringToObject(body,"firmware",s->firmware);
    cJSON_AddStringToObject(body,"hardware",pet_board_current()->hardware);
    cJSON_AddNumberToObject(body,"maxPackBytes",PET_INSTALL_PACK_MAX);
    cJSON_AddNumberToObject(body,"availableBytes",PET_INSTALL_PACK_MAX);
    cJSON *caps=cJSON_AddArrayToObject(body,"capabilities");
    const char *names[]={"asset-install-v1","frame-player-cloud-0.3","config-v2",
        "device-control-v1","session-binding-v1","physical-inventory-v1","speechMouthOffset"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)cJSON_AddItemToArray(caps,cJSON_CreateString(names[i]));
    cJSON *root=call(s,"/v1/device/hello",body);
    bool ok=root&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"ok"));
    cJSON_Delete(root);cJSON_Delete(body);return ok;
}

static bool operation(pet_sync_t *s,const char *id)
{
    char path[100];snprintf(path,sizeof(path),"/v1/device/installations/%s",id);
    cJSON *root=call(s,path,NULL);
    bool ok=root&&pet_control_operation(cJSON_GetObjectItemCaseSensitive(root,"operation"),&s->operation);
    cJSON_Delete(root);return ok&&strcmp(s->operation.id,id)==0;
}

static bool persist(pet_sync_t *s,const pet_install_t *next)
{
    if(pet_asset_store_commit(s->store,next)==ESP_OK)return true;
    freeze(s);s->status=PET_SYNC_STORAGE;return false;
}

static bool report(pet_sync_t *s,const char *status)
{
    cJSON *body=cJSON_CreateObject();if(!body)return false;
    cJSON_AddStringToObject(body,"installationId",s->operation.id);
    cJSON_AddStringToObject(body,"sha256",s->operation.sha256);
    cJSON_AddStringToObject(body,"status",status);
    cJSON_AddNumberToObject(body,"downloadedBytes",s->store->state.downloaded_bytes);
    cJSON *root=call(s,"/v1/device/installations/report",body);
    /* Re-fetch exact operation/current context for commit, never trust a stale
     * successful response as authority over a newer cloud binding. */
    pet_control_operation_t *ack=calloc(1,sizeof(*ack));
    pet_control_binding_t binding;pet_control_config_t config;
    bool ok=root&&ack&&pet_control_operation(cJSON_GetObjectItemCaseSensitive(root,"operation"),ack)&&
        !strcmp(ack->id,s->operation.id)&&!strcmp(ack->build_id,s->operation.build_id)&&
        !strcmp(ack->sha256,s->operation.sha256)&&ack->bytes==s->operation.bytes&&
        ack->downloaded_bytes==s->store->state.downloaded_bytes&&
        ack->state==(!strcmp(status,"downloading")?PET_OP_DOWNLOADING:!strcmp(status,"verified")?PET_OP_VERIFIED:
                      !strcmp(status,"activating")?PET_OP_ACTIVATING:PET_OP_INSTALLED)&&
        pet_control_binding(cJSON_GetObjectItemCaseSensitive(root,"binding"),&binding)&&
        pet_control_config(cJSON_GetObjectItemCaseSensitive(root,"config"),&config)&&
        cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root,"reconnect"));
    free(ack);cJSON_Delete(root);cJSON_Delete(body);return ok;
}

static bool prepare(pet_sync_t *s,unsigned slot)
{
    if(s->prepared_slot==(int)slot)return true;
    pet_control_operation_t *saved=calloc(1,sizeof(*saved));
    pet_pack_verified_manifest_t manifest;const void *pack=NULL;size_t bytes=0;
    bool ok=saved&&pet_asset_store_load_manifest(s->store,slot,saved)==ESP_OK&&
        pet_pack_verify_manifest(saved,s->cloud.account_id,s->keys,s->key_count,&manifest)&&
        pet_asset_store_map(s->store,slot,&pack,&bytes)==ESP_OK&&
        pet_pack_verify_bytes(pack,bytes,saved->sha256)&&
        s->runtime.prepare(s->runtime.context,slot,pack,bytes,&manifest);
    free(saved);
    if(ok)s->prepared_slot=(int)slot;else {freeze(s);s->status=PET_SYNC_UNTRUSTED;}
    return ok;
}

static bool inventory(pet_sync_t *s)
{
    if(s->inventory_generation==s->store->journal.generation)return true;
    cJSON *body=cJSON_CreateObject();if(!body)return false;
    char generation[24];snprintf(generation,sizeof(generation),"%" PRIu64,s->store->journal.generation);
    cJSON_AddStringToObject(body,"generation",generation);
    int active=s->store->state.active_slot;
    if(active<0)cJSON_AddNullToObject(body,"activeSlot");
    else cJSON_AddStringToObject(body,"activeSlot",active?"b":"a");
    cJSON *slots=cJSON_AddArrayToObject(body,"slots");
    const char *states[]={"empty","partial","verified","active"};
    for(unsigned i=0;i<2;++i) {
        const pet_install_slot_t *slot=&s->store->state.slots[i];
        cJSON *item=cJSON_CreateObject();cJSON_AddStringToObject(item,"slot",i?"b":"a");
        cJSON_AddStringToObject(item,"state",states[slot->state]);cJSON_AddNumberToObject(item,"bytes",slot->bytes);
        if(slot->state==PET_SLOT_EMPTY) {cJSON_AddNullToObject(item,"buildId");cJSON_AddNullToObject(item,"sha256");}
        else {cJSON_AddStringToObject(item,"buildId",slot->build_id);cJSON_AddStringToObject(item,"sha256",slot->sha256);}
        cJSON_AddItemToArray(slots,item);
    }
    cJSON *root=call(s,"/v1/device/inventory",body);
    bool ok=root&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"ok"));
    cJSON_Delete(body);cJSON_Delete(root);
    if(ok)s->inventory_generation=s->store->journal.generation;
    return ok;
}

bool pet_sync_init(pet_sync_t *s,pet_asset_store_t *store,const pet_control_http_t *http,
                   const pet_pack_trust_key_t *keys,size_t count,
                   const pet_sync_runtime_t *runtime,const char *firmware)
{
    if(!s||!store||!store->initialized||!store->journal.loaded||!http||!runtime||
       !runtime->freeze||!runtime->prepare||!runtime->activate||!runtime->release||!runtime->busy||!runtime->healthy||
       !firmware||!firmware[0]||strlen(firmware)>80||(!keys&&count))return false;
    memset(s,0,sizeof(*s));s->store=store;s->http=*http;s->keys=keys;s->key_count=count;
    s->runtime=*runtime;s->firmware=firmware;s->prepared_slot=-1;s->retry_seconds=30;
    return true;
}

bool pet_sync_library(pet_sync_t *s,const char *cursor,pet_control_library_t *library)
{
    char path[1600];if(!s||!s->has_context||!library||!pet_control_http_library_path(cursor,path,sizeof(path)))return false;
    cJSON *root=call(s,path,NULL);bool ok=root&&pet_control_library(root,library);
    cJSON_Delete(root);return ok;
}

bool pet_sync_show_cached(pet_sync_t *s)
{
    if(!s||!s->store->journal.loaded||s->store->state.active_slot<0||s->store->state.phase==PET_INSTALL_ACTIVATING)return false;
    cJSON *root=pet_control_json(s->store->state.cached_context,strlen(s->store->state.cached_context),2048);
    pet_control_context_t cached;bool ok=pet_control_context(root,&cached)&&!strcmp(cached.device_id,s->http.device_id);
    cJSON_Delete(root);if(!ok)return false;
    s->cloud=cached;freeze(s);
    return prepare(s,(unsigned)s->store->state.active_slot)&&
        s->runtime.activate(s->runtime.context,(unsigned)s->store->state.active_slot,&cached,false);
}

bool pet_sync_select(pet_sync_t *s,const pet_control_library_item_t *item,const char *request_uuid)
{
    if(!s||!item||!s->has_context||!s->cloud.selection_allowed||!item->installable||
       s->store->state.phase!=PET_INSTALL_IDLE||s->runtime.busy(s->runtime.context))return false;
    pet_install_t next=s->store->state;
    if(!pet_install_request(&next,request_uuid,item->build_id,item->sha256,item->bytes,s->cloud.binding.revision))return false;
    if(!s->runtime.release(s->runtime.context,(unsigned)next.candidate_slot))return false;
    pet_asset_store_unmap(s->store,(unsigned)next.candidate_slot);
    if(s->prepared_slot==next.candidate_slot)s->prepared_slot=-1;
    return persist(s,&next);
}

void pet_sync_step(pet_sync_t *s)
{
    if(!s)return;
    if(!s->store->journal.loaded||s->store->state.phase!=PET_INSTALL_DOWNLOADING)pet_control_download_end(&s->download,false);
    s->status=PET_SYNC_WAIT;s->retry_seconds=30;
    if(!s->store->journal.loaded){freeze(s);s->status=PET_SYNC_STORAGE;return;}
    if(!s->hello_sent){if(!hello(s))return;s->hello_sent=true;}
    if(!context(s))return;
    pet_install_t next=s->store->state;
    if(next.phase==PET_INSTALL_IDLE) {
        if(s->cloud.pending_id[0]) {
            if(s->runtime.busy(s->runtime.context)||!operation(s,s->cloud.pending_id))return;
            if(s->operation.state==PET_OP_FAILED||s->operation.state==PET_OP_CANCELLED)return;
            /* Website selection is already durable. Its operation UUID is a
             * local recovery key, never a second device selection POST. */
            if(!pet_install_request(&next,s->operation.id,s->operation.build_id,s->operation.sha256,
                s->operation.bytes,s->cloud.binding.revision)||!pet_install_attach(&next,s->operation.id,
                s->operation.build_id,s->operation.sha256,s->operation.bytes))return;
            if(!s->runtime.release(s->runtime.context,(unsigned)next.candidate_slot))return;
            pet_asset_store_unmap(s->store,(unsigned)next.candidate_slot);
            s->prepared_slot=-1;
            if(persist(s,&next))s->retry_seconds=0;
            return;
        }
        if(next.active_slot<0||!s->cloud.binding.assigned) {freeze(s);s->status=PET_SYNC_LIBRARY;inventory(s);return;}
        const pet_install_slot_t *slot=&next.slots[next.active_slot];
        if(strcmp(slot->build_id,s->cloud.binding.build_id)||strcmp(slot->sha256,s->cloud.binding.sha256)) {
            freeze(s);s->status=PET_SYNC_RECONCILE;return;
        }
        if(!s->admitted) {
            freeze(s);
            if(!prepare(s,(unsigned)next.active_slot))return;
            strcpy(next.binding_revision,s->cloud.binding.revision);strcpy(next.relationship_id,s->cloud.binding.relationship_id);
            strcpy(next.config_version,s->cloud.config.version);
            if(!pet_control_context_encode(&s->cloud,next.cached_context,sizeof(next.cached_context)))return;
            /* Write-ahead recovery context; no applied acknowledgment/socket
             * is allowed until the runtime has also successfully applied it. */
            if(memcmp(&next,&s->store->state,sizeof(next))&&!persist(s,&next))return;
            if(!s->runtime.activate(s->runtime.context,(unsigned)next.active_slot,&s->cloud,true))return;
            s->admitted=true;
        }
        s->status=PET_SYNC_READY;inventory(s);s->retry_seconds=s->cloud.next_poll_seconds;return;
    }
    if(s->runtime.busy(s->runtime.context))return;
    if(next.phase==PET_INSTALL_REQUESTED) {
        cJSON *body=cJSON_CreateObject();if(!body)return;
        cJSON_AddStringToObject(body,"requestId",next.request_id);
        cJSON_AddStringToObject(body,"buildId",next.slots[next.candidate_slot].build_id);
        cJSON_AddStringToObject(body,"expectedBindingRevision",next.expected_revision);
        cJSON *root=call(s,"/v1/device/installations/select",body);
        bool ok=root&&pet_control_operation(cJSON_GetObjectItemCaseSensitive(root,"operation"),&s->operation)&&
            pet_install_attach(&next,s->operation.id,s->operation.build_id,s->operation.sha256,s->operation.bytes);
        cJSON_Delete(root);cJSON_Delete(body);
        if(ok&&persist(s,&next))s->retry_seconds=0;
        else if(s->store->journal.loaded&&s->http_status>=400&&s->http_status<500&&
            (!strcmp(s->error_code,"BINDING_MISMATCH")||!strcmp(s->error_code,"SELECTION_DISABLED")||
             !strcmp(s->error_code,"BUILD_NOT_AVAILABLE")||!strcmp(s->error_code,"INSUFFICIENT_CAPACITY")||
             !strcmp(s->error_code,"FIRMWARE_UPDATE_REQUIRED")||!strcmp(s->error_code,"INSTALLATION_PENDING"))) {
            /* Canonical selection resolves prior request IDs BEFORE these
             * definitive no-commit errors. Timeouts/5xx never take this path. */
            if(pet_install_cancel(&next)&&persist(s,&next))s->status=PET_SYNC_LIBRARY;
        }
        return;
    }
    if(!operation(s,next.installation_id))return;
    const pet_install_slot_t *candidate=&next.slots[next.candidate_slot];
    if(strcmp(s->operation.build_id,candidate->build_id)||strcmp(s->operation.sha256,candidate->sha256)||
       s->operation.bytes!=candidate->bytes){freeze(s);s->status=PET_SYNC_RECONCILE;return;}
    if(s->operation.state==PET_OP_FAILED||s->operation.state==PET_OP_CANCELLED) {
        if(next.phase==PET_INSTALL_ACTIVATING){freeze(s);s->status=PET_SYNC_RECONCILE;return;}
        if(!s->runtime.release(s->runtime.context,(unsigned)next.candidate_slot))return;
        pet_asset_store_unmap(s->store,(unsigned)next.candidate_slot);s->prepared_slot=-1;
        if(pet_install_cancel(&next)&&persist(s,&next))s->retry_seconds=0;
        return;
    }
    if(next.phase==PET_INSTALL_DOWNLOADING) {
        s->status=PET_SYNC_DOWNLOAD;
        pet_pack_verified_manifest_t manifest;
        if(!pet_pack_verify_manifest(&s->operation,s->cloud.account_id,s->keys,s->key_count,&manifest)) {
            s->status=PET_SYNC_UNTRUSTED;return;
        }
        if(next.downloaded_bytes<candidate->bytes) {
            if(!next.downloaded_bytes&&pet_asset_store_save_manifest(s->store,&s->operation)!=ESP_OK) {
                s->status=PET_SYNC_STORAGE;return;
            }
            size_t bytes=candidate->bytes-next.downloaded_bytes;
            if(bytes>PET_INSTALL_CHECKPOINT_BYTES)bytes=PET_INSTALL_CHECKPOINT_BYTES;
            void *block=malloc(bytes);pet_control_http_result_t result={.retry_seconds=30};
            bool ok=block&&pet_control_http_range(&s->http,&s->download,&s->operation,next.downloaded_bytes,block,bytes,&result);
            if(ok&&pet_asset_store_write_block(s->store,block,bytes)!=ESP_OK) {ok=false;s->status=PET_SYNC_STORAGE;}
            if(ok)pet_control_download_wrote(&s->download);
            free(block);s->retry_seconds=ok?1:result.retry_seconds;
            if(result.status==401||result.status==403){freeze(s);s->status=PET_SYNC_AUTH;}
            /* Never regress a cloud checkpoint after an interrupted local write. */
            if(ok&&s->operation.state<=PET_OP_DOWNLOADING&&s->store->state.downloaded_bytes>=s->operation.downloaded_bytes)
                if(report(s,"downloading"))s->retry_seconds=1;
            return;
        }
        pet_control_download_end(&s->download,true);
        s->status=PET_SYNC_VERIFY;
        /* A lost final progress request must not skip queued->downloading on
         * the cloud side and wedge the later verified transition. */
        if(s->operation.state<=PET_OP_DOWNLOADING&&!report(s,"downloading"))return;
        if(prepare(s,(unsigned)next.candidate_slot)&&pet_install_verified(&next)&&persist(s,&next))s->retry_seconds=0;
        return;
    }
    freeze(s);s->status=PET_SYNC_ACTIVATE;
    if(!prepare(s,(unsigned)next.candidate_slot))return;
    if(next.phase==PET_INSTALL_VERIFIED) {
        if(s->operation.state<PET_OP_VERIFIED&&!report(s,"verified"))return;
        if(pet_install_activate(&next)&&persist(s,&next))s->retry_seconds=0;
        return;
    }
    if(s->operation.state!=PET_OP_INSTALLED) {
        if(s->operation.state<PET_OP_ACTIVATING&&!report(s,"activating"))return;
        if(!report(s,"installed")||!operation(s,next.installation_id))return;
    }
    if(!context(s))return;
    if(!pet_control_commit_install(&next,&s->operation,&s->cloud)) {s->status=PET_SYNC_RECONCILE;return;}
    if(!pet_control_context_encode(&s->cloud,next.cached_context,sizeof(next.cached_context)))return;
    if(!persist(s,&next))return;
    /* An activation crash now boots the committed slot and verifies it again.
     * Voice stays frozen until the runtime has applied matching configuration. */
    if(!s->runtime.activate(s->runtime.context,(unsigned)next.active_slot,&s->cloud,true))return;
    s->admitted=true;s->status=PET_SYNC_READY;s->retry_seconds=0;
}
