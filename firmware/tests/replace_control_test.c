#include "pet_replace_control.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "mbedtls/sha256.h"

/* Real durable journal, flash writer and receipt transitions; authenticated
 * transport/signature boundaries have independent real-crypto test suites. */
static pet_replace_control_t worker;
static pet_replace_journal_t journal;
static pet_replace_writer_t writer;
static pet_replace_poll_t server,wire_poll;
static pet_replace_operation_t wire_operation;
static pet_replace_report_t submitted;
static uint8_t sectors[PET_JOURNAL_SECTORS][PET_JOURNAL_SECTOR_BYTES];
static uint8_t *flash,pack[0x10003];
static uint64_t now;
static unsigned requests,erases,writes,freezes,detaches,verifications,activations;
static bool detached,compatible=true,verify_ok=true,transport_ok=true,wire_ok=true,detach_ok=true;
static bool lose_fence,lose_report,write_fail,journal_fail,wrong_ack,cancel_after_poll;
static uint64_t transport_delay;
static unsigned downloads_completed,downloads_stopped,downloads_closed,download_writes;
static const char *device="pet-0123456789abcdef0123456789abcdef";
static const char *account="00000000-0000-4000-8000-000000000001";
static const char *request_id="00000000-0000-4000-8000-000000000002";
static const char *operation_id="00000000-0000-4000-8000-000000000003";
static const char *fence_id="00000000-0000-4000-8000-000000000004";
static const char *relationship="00000000-0000-4000-8000-000000000005";
static const char *boot="00000000-0000-4000-8000-000000000006";
static void freeze(void *unused){(void)unused;++freezes;}
/* A device that keeps its conversation across a selection (the Pocket's hooks). */
static bool with_hooks,rebind_ok;
static bool removal_ok = true;
static unsigned removal_calls, encoded_acks;
static bool remove_pet(void *unused, const pet_replace_pack_t *p)
{
    (void)unused;
    assert(p->bytes == sizeof(pack) && !worker.cloud.has_operation);
    ++removal_calls;
    return removal_ok;
}
static unsigned pauses,rebinds;
static void pause_session(void *unused){(void)unused;++pauses;}
static bool rebind_session(void *unused,const pet_control_context_t *c)
{
    (void)unused;assert(journal.state.phase==PET_REPLACE_ACTIVE&&!server.has_operation);
    assert(!strcmp(c->binding.revision,journal.state.binding_revision)&&!strcmp(c->binding.relationship_id,journal.state.relationship_id));
    ++rebinds;return rebind_ok;
}
static bool detach(void *unused){(void)unused;++detaches;detached=detach_ok;return detached;}
static uint64_t now_ms(void *unused){(void)unused;return now;}
static bool caps(void *unused,const pet_release_v2_t *r){(void)unused;assert(r->pack.bytes==sizeof(pack));return compatible;}
static bool verify(void *unused,const pet_release_v2_t *r)
{(void)unused;++verifications;assert(r->pack.bytes==sizeof(pack));return verify_ok&&!memcmp(flash,pack,sizeof(pack));}
static bool activate(void *unused,const pet_control_context_t *c)
{
    (void)unused;assert(journal.state.phase==PET_REPLACE_ACTIVE&&!server.has_operation);
    assert(!strcmp(c->binding.revision,journal.state.binding_revision));
    assert(!strcmp(c->binding.relationship_id,journal.state.relationship_id));
    ++activations;detached=false;return verify_ok&&!memcmp(flash,pack,sizeof(pack));
}
static bool journal_read(void *unused,unsigned sector,uint8_t *record)
{(void)unused;memcpy(record,sectors[sector],PET_JOURNAL_SECTOR_BYTES);return true;}
static bool journal_write(void *unused,unsigned sector,const uint8_t *record)
{(void)unused;if(journal_fail){memset(sectors[sector],0xff,PET_JOURNAL_SECTOR_BYTES);memcpy(sectors[sector],record,128);return false;}
 memcpy(sectors[sector],record,PET_JOURNAL_SECTOR_BYTES);return true;}
static bool read_flash(void *unused,uint32_t at,void *out,size_t n)
{(void)unused;assert(at+n<=PET_REPLACE_MAX_BYTES+PET_REPLACE_MANIFEST_BYTES);memcpy(out,flash+at,n);return true;}
static bool erase_flash(void *unused,uint32_t at,size_t n)
{
    (void)unused;assert(journal.state.phase==PET_REPLACE_DOWNLOADING&&detached);
    assert(server.operation.phase==PET_CLOUD_DOWNLOADING&&server.operation.fence_confirmed&&server.operation.flash_reserved);
    assert(server.operation.has_report&&server.operation.report.generation==journal.journal.generation);
    assert(at+n<=PET_REPLACE_MAX_BYTES+PET_REPLACE_MANIFEST_BYTES&&at%4096==0&&n%4096==0);
    ++erases;memset(flash+at,0xff,n);return true;
}
static bool write_flash(void *unused,uint32_t at,const void *data,size_t n)
{(void)unused;assert(detached&&at+n<=PET_REPLACE_MAX_BYTES+PET_REPLACE_MANIFEST_BYTES);++writes;
 memcpy(flash+at,data,write_fail?n/2:n);return !write_fail;}
static const pet_journal_io_t journal_io={journal_read,journal_write,NULL};
static const pet_replace_io_t pet_io={read_flash,erase_flash,write_flash,freeze,detach,NULL,PET_REPLACE_MAX_BYTES+PET_REPLACE_MANIFEST_BYTES};

bool pet_replace_wire_encode_poll(const char *id,char *out,size_t n)
{assert(!strcmp(id,boot));return snprintf(out,n,"{\"version\":2,\"bootId\":\"%s\"}",id)>0;}
bool pet_replace_wire_encode_removal_poll(const char *id, const char ack[][37], unsigned count,
                                          char *out, size_t n)
{
    assert(count <= PET_REMOVAL_MAX);
    for (unsigned i = 0; i < count; ++i)
        assert(!strcmp(ack[i], operation_id));
    encoded_acks = count;
    return pet_replace_wire_encode_poll(id, out, n);
}
bool pet_replace_wire_encode_report(const pet_replace_report_t *r,char *out,size_t n)
{submitted=*r;return snprintf(out,n,"{\"fixture\":true}")>0;}
/* Selection: the cloud's HTTP status and Retry-After, or a reply binding another pet. */
static int select_status;
static unsigned select_retry;
static bool select_other;
bool pet_replace_wire_encode_select(const char *id,const char *build,const char *sha,char *out,size_t n)
{assert(!strcmp(id,boot));return snprintf(out,n,"{\"version\":2,\"bootId\":\"%s\",\"buildId\":\"%s\",\"sha256\":\"%s\"}",id,build,sha)>0;}
bool pet_replace_wire_poll(const char *json,size_t n,const char *id,const char *expected,
                           const pet_pack_trust_key_t *keys,size_t count,pet_replace_poll_t *out)
{(void)keys;(void)count;assert(n==1&&json[0]=='x'&&!strcmp(id,device)&&(!expected||!strcmp(expected,account)));*out=wire_poll;return wire_ok;}
bool pet_replace_wire_operation(const char *json,size_t n,const char *id,const char *expected,
                                const pet_pack_trust_key_t *keys,size_t count,pet_replace_operation_t *out)
{(void)keys;(void)count;assert(n==1&&json[0]=='x'&&!strcmp(id,device)&&!strcmp(expected,account));*out=wire_operation;return wire_ok;}
bool pet_control_same_binding(const pet_control_binding_t *a,const pet_control_binding_t *b)
{return a->assigned==b->assigned&&!strcmp(a->revision,b->revision)&&!strcmp(a->relationship_id,b->relationship_id)&&
    !strcmp(a->build_id,b->build_id)&&!strcmp(a->sha256,b->sha256);}
bool pet_release_v2_record_encode(const cJSON *envelope,const char *expected,const pet_replace_pack_t *target,
                                  const pet_pack_trust_key_t *keys,size_t count,void *record,size_t n)
{
    (void)keys;(void)count;assert(!strcmp(expected,account)&&target->bytes==sizeof(pack)&&n==PET_REPLACE_MANIFEST_BYTES);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(envelope,"payload")->valuestring,server.operation.signed_payload));
    memset(record,0x51,n);return true;
}
static void activation_result(void)
{
    pet_replace_operation_t *o=&server.operation;o->has_result=true;o->binding.assigned=true;
    snprintf(o->binding.revision,sizeof(o->binding.revision),"binding-%c",o->id[35]);strcpy(o->binding.relationship_id,relationship);
    strcpy(o->binding.build_id,o->release.pack.build_id);strcpy(o->binding.sha256,o->release.pack.sha256);
    o->config=server.context.config;strcpy(o->config.version,"17");
}
bool pet_control_http_json(const pet_control_http_t *http,const char *path,const char *body,char *out,size_t n,pet_control_http_result_t *result)
{
    assert(http&&body&&n>1);++requests;now+=transport_delay;*result=(pet_control_http_result_t){transport_ok?200:503,30,1};out[0]='x';
    if(!transport_ok)return false;
    if(!strcmp(path,"/v2/device/pets/poll")){
        wire_poll=server;if(cancel_after_poll){server.operation.cancel_requested=true;cancel_after_poll=false;}
    }else if(!strcmp(path,"/v2/device/pets/fence")){
        cJSON *j=cJSON_Parse(body);assert(j);pet_replace_operation_t *o=&server.operation;
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"installationId")->valuestring,o->id));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"fenceId")->valuestring,o->fence_id));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"buildId")->valuestring,o->release.pack.build_id));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"sha256")->valuestring,o->release.pack.sha256));
        assert(cJSON_GetObjectItemCaseSensitive(j,"bytes")->valuedouble==o->release.pack.bytes);cJSON_Delete(j);
        assert(o->phase==PET_CLOUD_QUEUED||o->phase==PET_CLOUD_FENCED);o->phase=PET_CLOUD_FENCED;o->fence_confirmed=true;
        memset(&server.context.binding,0,sizeof(server.context.binding));strcpy(server.context.binding.revision,"fenced-binding");
        wire_operation=*o;if(lose_fence){lose_fence=false;return false;}
    }else if(!strcmp(path,"/v2/device/pets/select")){
        /* The cloud binds exactly the installed pet named, or answers why not. */
        if(select_status){
            result->status=select_status;if(select_retry)result->retry_seconds=select_retry;
            result->bytes=(size_t)snprintf(out,n,"{\"version\":2,\"error\":{\"code\":\"FIXTURE\",\"retryable\":false}}");
            return true;
        }
        cJSON *j=cJSON_Parse(body);assert(j);pet_control_binding_t *b=&server.context.binding;b->assigned=true;
        strcpy(b->build_id,cJSON_GetObjectItemCaseSensitive(j,"buildId")->valuestring);
        strcpy(b->sha256,cJSON_GetObjectItemCaseSensitive(j,"sha256")->valuestring);cJSON_Delete(j);
        strcpy(b->revision,"selected-binding");strcpy(b->relationship_id,relationship);strcpy(server.context.config.version,"18");
        if(select_other)b->build_id[35]^=1;
        wire_poll=server;
    }else{
        assert(!strcmp(path,"/v2/device/pets/report"));pet_replace_operation_t *o=&server.operation;
        pet_replace_receipt_action_t action=pet_replace_receipt_classify(o,&submitted);
        if(action==PET_RECEIPT_REJECT){result->status=409;return true;}
        if(action==PET_RECEIPT_ADVANCE){
            o->report=submitted;o->has_report=true;o->phase=submitted.phase;
            o->flash_reserved=o->phase!=PET_CLOUD_RECOVERY&&o->phase!=PET_CLOUD_INSTALLED;
            if(o->phase==PET_CLOUD_ACTIVATING)activation_result();
            if(o->phase==PET_CLOUD_INSTALLED){server.context.binding=o->binding;server.context.config=o->config;server.has_operation=false;}
        }
        wire_operation=*o;
        if(wrong_ack){wire_operation.report.generation++;wrong_ack=false;}
        if(lose_report){lose_report=false;return false;}
    }
    return true;
}
/* The worker's own session models the kept connection: open between ranges,
 * closed after the final range or any failure. */
bool pet_control_http_pet_range(const pet_control_http_t *http,pet_control_download_t *download,const char *id,const char *sha,
                                uint32_t total,uint32_t offset,void *out,size_t n,pet_control_http_result_t *result)
{
    assert(http&&download==&worker.download&&!strcmp(id,server.operation.id)&&!strcmp(sha,server.operation.release.pack.sha256)&&total==sizeof(pack));
    ++requests;now+=transport_delay;*result=(pet_control_http_result_t){206,30,n};
    if(!download->url[0])strcpy(download->url,"session");
    if(!transport_ok||server.operation.cancel_requested){result->status=503;download->client=NULL;return false;}
    assert(server.operation.phase==PET_CLOUD_DOWNLOADING&&offset+n<=sizeof(pack));memcpy(out,pack+offset,n);
    ++download->ranges;download->client=offset+n<total?(void *)&worker:NULL;return true;
}
void pet_control_download_wrote(pet_control_download_t *d){assert(d==&worker.download&&d->ranges);++download_writes;}
void pet_control_download_close(pet_control_download_t *d){assert(d==&worker.download);d->client=NULL;++downloads_closed;}
void pet_control_download_end(pet_control_download_t *d,bool complete)
{assert(d==&worker.download);if(d->url[0]){if(complete)++downloads_completed;else ++downloads_stopped;}memset(d,0,sizeof(*d));}
static pet_replace_control_config_t config(void)
{
    pet_replace_control_config_t c={.http={"https://fixture.invalid",device,"not-a-real-credential"},
        .compatible_healthy=caps,.verify=verify,.activate=activate,.freeze=freeze,.now_ms=now_ms};
    if(with_hooks){c.pause=pause_session;c.rebind=rebind_session;}
    strcpy(c.boot_id,boot);return c;
}
static void reboot(void)
{
    pet_replace_writer_close(&writer);assert(pet_replace_journal_open(&journal,&journal_io,PET_REPLACE_MAX_BYTES)==PET_JOURNAL_OK);
    assert(pet_replace_writer_init(&writer,&journal,&pet_io));pet_replace_control_config_t c=config();
    assert(pet_replace_control_init(&worker,&c,&journal,&writer));detached=false;
}
static void initialize(void)
{
    pet_replace_writer_close(&writer);memset(&worker,0,sizeof(worker));memset(&server,0,sizeof(server));
    memset(sectors,0xff,sizeof(sectors));memset(flash,0xff,PET_REPLACE_MAX_BYTES+PET_REPLACE_MANIFEST_BYTES);memset(pack,0x57,sizeof(pack));
    now=1000;requests=erases=writes=freezes=detaches=verifications=activations=0;transport_delay=0;
    downloads_completed=downloads_stopped=downloads_closed=download_writes=0;
    detached=false;compatible=verify_ok=transport_ok=wire_ok=detach_ok=true;
    lose_fence=lose_report=write_fail=journal_fail=wrong_ack=cancel_after_poll=false;
    strcpy(server.context.device_id,device);strcpy(server.context.account_id,account);strcpy(server.context.binding.revision,"r0");
    strcpy(server.context.config.version,"1");strcpy(server.context.config.face_id,"big-sal");strcpy(server.context.config.ai_pet_id,"big-sal");
    server.has_operation=true;pet_replace_operation_t *o=&server.operation;o->phase=PET_CLOUD_QUEUED;o->flash_reserved=true;
    strcpy(o->id,operation_id);strcpy(o->request_id,request_id);strcpy(o->fence_id,fence_id);strcpy(o->expected_revision,"r0");
    strcpy(o->release.pack.build_id,request_id);o->release.pack.bytes=sizeof(pack);strcpy(o->signed_payload,"signed-fixture");
    strcpy(o->key_id,"test-only");strcpy(o->signature,"fixture-signature");
    uint8_t sha[32];assert(!mbedtls_sha256(pack,sizeof(pack),sha,0));
    for(unsigned i=0;i<32;++i)snprintf(o->release.pack.sha256+2*i,3,"%02x",sha[i]);
    reboot();
}
static void step(bool allow)
{
    unsigned calls=requests,old_erases=erases,old_writes=writes,old_activations=activations;uint64_t generation=journal.journal.generation;
    pet_replace_control_step(&worker,now,allow);assert(requests-calls<=1);
    if(!allow)assert(erases==old_erases&&writes==old_writes&&activations==old_activations&&generation==journal.journal.generation);
    now+=100;
}
static void until(pet_replace_phase_t phase,bool acknowledged)
{
    for(unsigned i=0;i<200;++i){
        if(journal.state.phase==phase){pet_replace_report_t r;
            if(!acknowledged||(pet_replace_receipt_make(&journal,&server.operation,&r)&&pet_replace_receipt_acknowledged(&server.operation,&r)))return;
        }
        step(true);if(now<worker.retry_at_ms)now=worker.retry_at_ms;
    }
    fprintf(stderr,"stalled phase=%d wanted=%d cloud=%d error=%s\n",journal.state.phase,phase,server.operation.phase,worker.error_code);abort();
}
static void finish(void)
{
    for(unsigned i=0;i<250&&worker.status!=PET_REPLACE_CONTROL_READY;++i){step(true);if(now<worker.retry_at_ms)now=worker.retry_at_ms;}
    if(worker.status!=PET_REPLACE_CONTROL_READY)fprintf(stderr,"finish phase=%d cloud=%d error=%s\n",journal.state.phase,server.operation.phase,worker.error_code);
    assert(worker.status==PET_REPLACE_CONTROL_READY&&journal.state.phase==PET_REPLACE_ACTIVE&&!server.has_operation&&activations);
}
static void checkpoint(uint32_t bytes)
{
    for(unsigned i=0;i<100;++i){
        pet_replace_report_t r;
        if(journal.state.phase==PET_REPLACE_DOWNLOADING&&journal.state.downloaded_bytes==bytes&&
           pet_replace_receipt_make(&journal,&server.operation,&r)&&pet_replace_receipt_acknowledged(&server.operation,&r))return;
        step(true);
    }
    assert(!"checkpoint not reached");
}
static void supersede(void)
{
    pet_replace_operation_t *o=&server.operation;
    o->supersedes=true;strcpy(o->previous_id,o->id);strcpy(o->previous_fence_id,o->fence_id);o->previous_generation=journal.journal.generation;
    o->id[35]='8';o->fence_id[35]='9';o->request_id[35]='a';o->release.pack.build_id[35]='b';
    strcpy(o->expected_revision,server.context.binding.revision);strcpy(o->signed_payload,"superseded-signed-fixture");
    o->phase=PET_CLOUD_FENCED;o->flash_reserved=true;o->fence_confirmed=o->cancel_requested=o->has_report=o->has_result=false;
    memset(&o->report,0,sizeof(o->report));worker.next_poll_ms=0;
}
static void queue_new(char identity)
{
    pet_replace_operation_t *o=&server.operation;
    o->id[35]=identity;o->request_id[35]=identity;o->fence_id[35]=identity;o->release.pack.build_id[35]=identity;
    snprintf(o->signed_payload,sizeof(o->signed_payload),"signed-fixture-%c",identity);
    strcpy(o->expected_revision,server.context.binding.revision);o->phase=PET_CLOUD_QUEUED;
    o->flash_reserved=true;o->fence_confirmed=o->cancel_requested=o->has_report=o->has_result=o->supersedes=false;
    memset(&o->report,0,sizeof(o->report));memset(&o->binding,0,sizeof(o->binding));memset(&o->config,0,sizeof(o->config));
    server.has_operation=true;worker.next_poll_ms=0;
}
int main(void)
{
    flash=malloc(PET_REPLACE_MAX_BYTES+PET_REPLACE_MANIFEST_BYTES);assert(flash);
    initialize();finish();assert(erases==3&&writes==3&&verifications==1&&detaches==1);
    assert(downloads_completed==1&&!downloads_stopped&&download_writes==2&&!worker.download.client&&!worker.download.url[0]);
    /* Losing connectivity closes the kept connection but keeps the operation;
     * refused flash authority or a cancellation ends it without a summary of
     * completion, and completion ends it exactly once. */
    initialize();checkpoint(0x10000);assert(worker.download.client&&!downloads_closed);
    pet_replace_control_disconnect(&worker);assert(!worker.download.client&&worker.download.url[0]&&downloads_closed==1);
    finish();assert(downloads_completed==1&&!downloads_stopped&&!worker.download.url[0]);
    initialize();checkpoint(0x10000);assert(worker.download.client);step(false);
    assert(downloads_stopped==1&&!worker.download.client&&!worker.download.url[0]);
    finish();assert(downloads_completed==1&&downloads_stopped==1);
    reboot();finish();assert(activations==2); // ACTIVE/current binding after complete power loss.
    for(pet_replace_phase_t phase=PET_REPLACE_REQUESTED;phase<=PET_REPLACE_ACTIVATING;++phase){
        initialize();until(phase,false);reboot();finish();
    }
    initialize();until(PET_REPLACE_ACTIVE,false);assert(server.operation.phase==PET_CLOUD_ACTIVATING);reboot();finish();
    initialize();checkpoint(0x10000);reboot();finish();
    initialize();checkpoint(sizeof(pack));reboot();finish();
    initialize();lose_fence=true;finish();assert(erases==3);
    initialize();until(PET_REPLACE_VERIFIED,false);lose_report=true;finish();
    initialize();until(PET_REPLACE_ACTIVATING,false);lose_report=true;finish();
    initialize();until(PET_REPLACE_ACTIVE,false);lose_report=true;finish();
    initialize();until(PET_REPLACE_INVALIDATED,false);wrong_ack=true;finish();
    initialize();checkpoint(0x10000);cancel_after_poll=true;worker.next_poll_ms=0;step(true);
    /* Cancellation racing the authenticated poll is rechecked by binary GET;
     * no unauthenticated bytes are passed to the real writer. */
    until(PET_REPLACE_RECOVERY,true);assert(journal.state.downloaded_bytes==0x10000);
    assert(downloads_stopped==1&&!downloads_completed&&!worker.download.client&&!worker.download.url[0]);

    initialize();compatible=false;for(unsigned i=0;i<20;++i){step(true);now=worker.retry_at_ms+1;}assert(!erases&&journal.state.phase==PET_REPLACE_EMPTY);
    initialize();wire_ok=false;step(true);assert(!worker.has_context&&!erases);wire_ok=true;now=worker.retry_at_ms;finish();
    initialize();transport_ok=false;transport_delay=30000;uint64_t started=now;step(true);
    assert(worker.retry_at_ms==started+60000);unsigned calls=requests;step(true);assert(requests==calls);
    transport_ok=true;transport_delay=0;now=worker.retry_at_ms;finish();

    /* A temporary idle management outage preserves the admitted conversation,
     * while a malformed authenticated response still removes admission. */
    initialize();finish();
    server.has_operation = false;
    worker.next_poll_ms = 0;
    step(true);
    unsigned before_outage = freezes;
    assert(worker.admitted && !worker.cloud.has_operation);
    transport_ok = false;
    worker.next_poll_ms = 0;
    step(true);
    assert(worker.admitted && !worker.authenticated && freezes == before_outage);
    transport_ok = true;
    now = worker.retry_at_ms;
    step(true);
    assert(worker.admitted && worker.authenticated);
    wire_ok = false;
    worker.next_poll_ms = 0;
    step(true);
    assert(!worker.admitted && freezes > before_outage);

    initialize();for(unsigned i=0;i<10;++i)step(false);assert(journal.state.phase==PET_REPLACE_EMPTY&&!erases);finish();
    unsigned admitted_freezes=freezes;step(false);assert(!worker.admitted&&freezes==admitted_freezes+1);finish();
    initialize();until(PET_REPLACE_INVALIDATED,false);step(false);assert(server.operation.phase==PET_CLOUD_INVALIDATED&&!erases);
    for(unsigned i=0;i<5;++i)step(false);finish();
    initialize();until(PET_REPLACE_REQUESTED,false);server.has_operation=false;worker.next_poll_ms=0;step(true);step(true);
    assert(journal.state.phase==PET_REPLACE_EMPTY&&!erases);
    initialize();finish();pet_replace_pack_t previous=journal.state.active;unsigned previous_erases=erases;
    queue_new('7');until(PET_REPLACE_REQUESTED,false);
    queue_new('a');step(true);step(true);assert(journal.state.phase==PET_REPLACE_ACTIVE);
    assert(!strcmp(journal.state.active.build_id,previous.build_id)&&erases==previous_erases);
    step(true);assert(journal.state.phase==PET_REPLACE_REQUESTED&&journal.state.request_id[35]=='a'&&erases==previous_erases);
    finish();assert(journal.state.active.build_id[35]=='a');
    initialize();until(PET_REPLACE_REQUESTED,false);queue_new('a');strcpy(server.context.binding.revision,"unrelated-binding");
    step(true);step(true);assert(journal.state.phase==PET_REPLACE_REQUESTED&&!erases);
    initialize();until(PET_REPLACE_FENCED,false);queue_new('a');step(true);step(true);
    assert(journal.state.phase==PET_REPLACE_FENCED&&!erases); // Never discard an existing fence to take a different queue.

    for(pet_replace_phase_t phase=PET_REPLACE_FENCED;phase<=PET_REPLACE_VERIFIED;++phase){
        initialize();until(phase,false);server.operation.cancel_requested=true;worker.next_poll_ms=0;
        until(PET_REPLACE_RECOVERY,true);assert(!server.operation.flash_reserved&&!pet_replace_has_active(&journal.state));
        server.operation.phase=PET_CLOUD_FENCED;server.operation.flash_reserved=true;server.operation.cancel_requested=false;worker.next_poll_ms=0;
        finish();
    }
    initialize();checkpoint(0x10000);server.operation.cancel_requested=true;worker.next_poll_ms=0;until(PET_REPLACE_RECOVERY,true);
    uint64_t old_generation=journal.journal.generation;
    server.operation.phase=PET_CLOUD_FENCED;server.operation.flash_reserved=true;worker.next_poll_ms=0;
    until(PET_REPLACE_RECOVERY,true); // Initial equality is still old RECOVERY; explicitly process new FENCED retry.
    step(true);step(true);step(true);
    until(PET_REPLACE_RECOVERY,true);assert(journal.journal.generation>old_generation&&!server.operation.flash_reserved);

    initialize();checkpoint(0x10000);flash[0]^=1;reboot();until(PET_REPLACE_INVALIDATED,false);
    assert(server.operation.phase==PET_CLOUD_DOWNLOADING&&server.operation.report.downloaded_bytes==0x10000);
    server.operation.cancel_requested=true;worker.next_poll_ms=0;until(PET_REPLACE_RECOVERY,true);
    assert(server.operation.report.downloaded_bytes==0); // Reset ACK precedes cancelled recovery.
    initialize();checkpoint(0x10000);flash[0]^=1;reboot();finish();
    initialize();checkpoint(0x10000);flash[PET_REPLACE_MAX_BYTES]^=1;reboot();until(PET_REPLACE_RECOVERY,true);
    assert(journal.state.downloaded_bytes==0x10000&&!activations);

    initialize();checkpoint(0x10000);server.operation.cancel_requested=true;worker.next_poll_ms=0;until(PET_REPLACE_RECOVERY,true);
    supersede();finish();assert(journal.state.active.build_id[35]=='b');
    initialize();until(PET_REPLACE_REQUESTED,false);
    server.operation.phase=PET_CLOUD_FENCED;server.operation.cancel_requested=true;worker.next_poll_ms=0;
    compatible=false;worker.writer=NULL;until(PET_REPLACE_RECOVERY,true);assert(!erases&&!server.operation.flash_reserved);
    server.operation.phase=PET_CLOUD_FENCED;server.operation.flash_reserved=true;server.operation.fence_confirmed=false;worker.next_poll_ms=0;
    step(true);step(true);step(true);step(true);until(PET_REPLACE_RECOVERY,true);
    assert(!erases&&!server.operation.flash_reserved); // Cancelled retry does not need a usable writer or healthy firmware.
    initialize();until(PET_REPLACE_FENCED,false);server.operation.cancel_requested=true;worker.next_poll_ms=0;until(PET_REPLACE_RECOVERY,true);
    supersede();server.operation.cancel_requested=true;compatible=false;worker.writer=NULL;
    step(true);step(true);until(PET_REPLACE_RECOVERY,true);assert(!erases&&!server.operation.flash_reserved);
    initialize();checkpoint(0x10000);server.operation.cancel_requested=true;worker.next_poll_ms=0;until(PET_REPLACE_RECOVERY,true);
    supersede();++server.operation.previous_generation;unsigned old_erases=erases;
    for(unsigned i=0;i<10;++i){step(true);now=worker.retry_at_ms+1;}
    assert(erases==old_erases&&journal.state.phase==PET_REPLACE_RECOVERY&&!activations);
    initialize();until(PET_REPLACE_DOWNLOADING,true);detach_ok=false;reboot();until(PET_REPLACE_RECOVERY,true);assert(!erases);
    initialize();until(PET_REPLACE_DOWNLOADING,true);write_fail=true;until(PET_REPLACE_RECOVERY,true);assert(erases==1);
    initialize();until(PET_REPLACE_DOWNLOADING,true);verify_ok=false;until(PET_REPLACE_RECOVERY,true);assert(!activations);
    initialize();until(PET_REPLACE_INVALIDATED,false);journal_fail=true;step(true);step(true);
    assert(!journal.ready&&!activations&&!erases);journal_fail=false;reboot();finish();
    initialize();journal.ready=false;for(unsigned i=0;i<20;++i){step(true);now+=15000;}assert(requests>=2&&!erases&&!activations);
    for(unsigned fault=0;fault<3;++fault){
        initialize();finish();unsigned old_freezes=freezes,old_calls=requests,old_erases=erases;
        if(fault==0)journal.ready=false;else if(fault==1)journal.journal.loaded=false;else journal.state.capacity_bytes=1;
        worker.retry_at_ms=now+30000;step(true);assert(!worker.admitted&&freezes==old_freezes+1);
        now=worker.retry_at_ms+15000;step(true);step(true);
        assert(worker.status==PET_REPLACE_CONTROL_RECOVERY&&requests>old_calls&&erases==old_erases);
    }
    /* Selection (three-pet devices): only an authenticated worker outside its
     * backoff asks. A final refusal or a reply binding another pet changes
     * nothing. No answer, a 429 or a 5xx (Cloudflare's too) start the backoff,
     * with the answer's Retry-After, and change nothing else. A choice is
     * adopted like a poll, ending the admitted pet's session, and mounts once
     * the caller rebinds the journal. */
    {
        initialize();finish();
        pet_replace_pack_t chosen=server.operation.release.pack;chosen.build_id[35]='c';
        unsigned calls=requests,freezes_before=freezes,mounted=activations;int http=-1;
        worker.authenticated=false;
        assert(pet_replace_control_select(&worker,now,&chosen,&http)==PET_REPLACE_SELECT_UNAVAILABLE&&requests==calls&&!http);
        worker.authenticated=true;pet_control_context_t before=worker.cloud.context;const uint64_t idle=worker.retry_at_ms;
        for(int final=400;final<=409;final+=9){
            select_status=final;
            assert(pet_replace_control_select(&worker,now,&chosen,&http)==PET_REPLACE_SELECT_REFUSED&&http==final&&worker.retry_at_ms==idle);
        }
        select_status=0;select_other=true;
        assert(pet_replace_control_select(&worker,now,&chosen,&http)==PET_REPLACE_SELECT_REFUSED&&http==200&&worker.retry_at_ms==idle);
        select_other=false;transport_ok=false;
        assert(pet_replace_control_select(&worker,now,&chosen,&http)==PET_REPLACE_SELECT_UNAVAILABLE&&!http&&worker.retry_at_ms==now+30000);
        transport_ok=true;const unsigned asked=requests;
        assert(pet_replace_control_select(&worker,worker.retry_at_ms-1,&chosen,&http)==PET_REPLACE_SELECT_UNAVAILABLE&&requests==asked);
        const int retryable[]={429,503,522};const unsigned after[]={0,7,60};
        for(unsigned i=0;i<3;++i){
            now=worker.retry_at_ms;select_status=retryable[i];select_retry=after[i];
            assert(pet_replace_control_select(&worker,now,&chosen,&http)==PET_REPLACE_SELECT_UNAVAILABLE&&http==retryable[i]);
            assert(worker.retry_at_ms==now+(after[i]?after[i]:30)*1000ull);
        }
        select_status=0;select_retry=0;now=worker.retry_at_ms;
        assert(requests==calls+7&&!memcmp(&before,&worker.cloud.context,sizeof(before))&&worker.admitted&&worker.authenticated&&freezes==freezes_before);
        assert(pet_replace_control_select(&worker,now,&chosen,&http)==PET_REPLACE_SELECT_CHOSEN&&http==200&&requests==calls+8);
        assert(!strcmp(worker.cloud.context.binding.build_id,chosen.build_id)&&!strcmp(worker.cloud.context.binding.revision,"selected-binding"));
        assert(!worker.admitted&&freezes==freezes_before+1&&worker.status==PET_REPLACE_CONTROL_WORKING);
        pet_replace_t rebound=journal.state;const pet_control_context_t *cloud=&worker.cloud.context;
        assert(pet_replace_rebind(&rebound,&chosen,cloud->binding.revision,cloud->binding.relationship_id,cloud->config.version));
        assert(pet_replace_journal_commit(&journal,&rebound)==PET_JOURNAL_OK);
        step(true);assert(worker.status==PET_REPLACE_CONTROL_READY&&worker.admitted&&activations==mounted+1&&requests==calls+8);
    }
    /* The device moved to another account: the next
     * poll adopts it without a restart. The old account's conversation ends,
     * its pet stays installed but unbound and is never admitted again, and
     * polling goes on until the new account installs a pet. */
    {
        initialize();finish();
        const char *before=account;unsigned mounted=activations,frozen=freezes,calls=requests;
        account="00000000-0000-4000-8000-000000000009";
        strcpy(server.context.account_id,account);memset(&server.context.binding,0,sizeof(server.context.binding));
        strcpy(server.context.binding.revision,"moved");strcpy(server.context.config.version,"1");
        worker.next_poll_ms=0;step(true);
        assert(requests==calls+1&&!strcmp(worker.cloud.context.account_id,account)&&!worker.cloud.context.binding.assigned);
        assert(worker.has_context&&worker.authenticated&&!worker.admitted&&freezes==frozen+1&&!worker.manifest_ready&&!worker.verified);
        assert(worker.status==PET_REPLACE_CONTROL_WORKING&&!worker.error_code[0]);
        /* Unbound on the new account: nothing is admitted, nothing written. */
        uint64_t generation=journal.journal.generation;step(true);
        assert(!worker.admitted&&activations==mounted&&journal.journal.generation==generation&&journal.state.phase==PET_REPLACE_ACTIVE);
        now=worker.retry_at_ms;worker.next_poll_ms=0;step(true);assert(!strcmp(worker.cloud.context.account_id,account));
        /* The new account's installation completes on the same boot. */
        queue_new('d');finish();
        assert(activations==mounted+1&&journal.state.active.build_id[35]=='d'&&!strcmp(worker.cloud.context.account_id,account));
        account=before;
    }
    /* Adopted in the middle of an installation. The cloud refuses a move
     * while one is in flight; this is the device's own guard. The old
     * account's download stops at once, nothing more is written or admitted
     * for it, and the worker waits as it would after a restart. */
    {
        initialize();checkpoint(0x10000);
        const char *before=account;unsigned written=writes,erased=erases,mounted=activations,closed=downloads_closed;
        account="00000000-0000-4000-8000-00000000000a";
        strcpy(server.context.account_id,account);memset(&server.context.binding,0,sizeof(server.context.binding));
        strcpy(server.context.binding.revision,"moved");server.has_operation=false;
        worker.next_poll_ms=0;step(true);
        assert(!strcmp(worker.cloud.context.account_id,account)&&!worker.cloud.has_operation&&downloads_closed==closed+1);
        /* It polls on, and every pass ends in the same refusal to mount. */
        for(unsigned i=0;i<20||worker.status!=PET_REPLACE_CONTROL_RECOVERY;++i){
            assert(i<40);step(true);if(now<worker.retry_at_ms)now=worker.retry_at_ms;
        }
        assert(writes==written&&erases==erased&&activations==mounted&&journal.state.phase==PET_REPLACE_DOWNLOADING);
        assert(!worker.admitted&&!strcmp(worker.error_code,"PET_ACTIVE_BINDING_CONFLICT"));
        reboot();
        for(unsigned i=0;i<5||worker.status!=PET_REPLACE_CONTROL_RECOVERY;++i){
            assert(i<40);step(true);if(now<worker.retry_at_ms)now=worker.retry_at_ms;
        }
        assert(writes==written&&erases==erased&&!strcmp(worker.error_code,"PET_ACTIVE_BINDING_CONFLICT"));
        account=before;
    }
    /* With pause and rebind hooks (three-pet devices, session-rebind-v1) a
     * selection or a settings change pauses the conversation instead of
     * ending it, and the next step mounts over it: no freeze, no activation.
     * When it cannot move (no open session that can rebind) mount freezes and
     * activates as before. An operation change still ends the conversation. */
    {
        with_hooks=true;rebind_ok=false;initialize();finish(); /* Fresh: nothing open to move. */
        assert(worker.admitted&&rebinds==1&&!pauses);
        pet_replace_pack_t chosen=server.operation.release.pack;chosen.build_id[35]='c';int http=-1;
        unsigned freezes_before=freezes,mounted=activations;rebinds=0;rebind_ok=true;
        assert(pet_replace_control_select(&worker,now,&chosen,&http)==PET_REPLACE_SELECT_CHOSEN&&http==200);
        assert(!worker.admitted&&pauses==1&&freezes==freezes_before&&worker.status==PET_REPLACE_CONTROL_WORKING);
        pet_replace_t rebound=journal.state;const pet_control_context_t *cloud=&worker.cloud.context;
        assert(pet_replace_rebind(&rebound,&chosen,cloud->binding.revision,cloud->binding.relationship_id,cloud->config.version));
        assert(pet_replace_journal_commit(&journal,&rebound)==PET_JOURNAL_OK);
        step(true);
        assert(worker.status==PET_REPLACE_CONTROL_READY&&worker.admitted&&rebinds==1&&activations==mounted&&freezes==freezes_before);
        // Settings changed on the website: a poll with a new config version pauses too.
        strcpy(server.context.config.version,"19");now=worker.next_poll_ms;step(true);
        assert(!worker.admitted&&pauses==2&&freezes==freezes_before&&!strcmp(worker.cloud.context.config.version,"19"));
        // The session cannot move (a gateway without session-rebind-v1): freeze and activate.
        rebind_ok=false;step(true);
        assert(worker.admitted&&rebinds==2&&activations==mounted+1&&freezes==freezes_before+1);
        // A new installation is not a selection: its poll ends the conversation.
        queue_new('d');step(true);assert(!worker.admitted&&pauses==2&&freezes==freezes_before+2);
        with_hooks=false;
    }
    /* Capability-gated removal: callback success is the durable/detached
     * barrier. Failed or blocked work is never acknowledged; a lost HTTP
     * response retains the acknowledgement for an idempotent next poll. */
    initialize();
    server.has_operation = false;
    server.has_removals = true;
    server.removal_count = 1;
    server.removals[0].pack = server.operation.release.pack;
    strcpy(server.removals[0].id, operation_id);
    step(true);
    assert(worker.status == PET_REPLACE_CONTROL_RECOVERY && !removal_calls);
    worker.config.remove = remove_pet;
    now = worker.retry_at_ms;
    step(false); /* Negotiate even while firmware has the gate. */
    step(false);
    assert(!removal_calls && !worker.removal_ack_count && !encoded_acks);
    worker.cloud.has_operation = true; /* Defense even beyond the wire parser. */
    step(false);
    assert(!removal_calls);
    worker.cloud.has_operation = false;
    removal_ok = false;
    step(true);
    assert(removal_calls == 1 && !worker.removal_ack_count);
    removal_ok = true;
    now = worker.retry_at_ms;
    step(true);
    step(true);
    assert(removal_calls == 2 && worker.removal_ack_count == 1 && !erases && !writes);
    transport_ok = false;
    step(true);
    assert(encoded_acks == 1 && worker.removal_ack_count == 1);
    transport_ok = true;
    server.removal_count = 0;
    now = worker.retry_at_ms;
    step(true);
    assert(encoded_acks == 1 && !worker.removal_ack_count);
    pet_replace_writer_close(&writer);free(flash);
    puts("pet replacement worker: real durable writer, fence/reset ACK barriers, power loss, cancellation/retry, corrupt storage, no concurrent firmware writes, transport backoff, account moves and selections that keep the conversation passed");
}
