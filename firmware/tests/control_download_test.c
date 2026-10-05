/* Keep-alive range downloads against a scripted esp_http_client: counts
 * client inits, connects, request opens and cleanups; checks every range's
 * own absolute deadline and credential, the connection-failure fallback and
 * the one-line summary. The real pet_control_http.c is compiled in here so
 * its summary line can be captured. JSON requests (polls, the selection)
 * share a kept connection of their own: timed with the fake's 1.5 s connect,
 * a request on it costs only its exchange. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#undef ESP_LOGI
static char logged[512];
static unsigned lines;
static void summary(const char *format,...) __attribute__((format(printf,1,2)));
static void summary(const char *format,...)
{va_list args;va_start(args,format);vsnprintf(logged,sizeof(logged),format,args);va_end(args);++lines;}
#define ESP_LOGI(tag,...) ((void)(tag),summary(__VA_ARGS__))
#include "../main/pet_control_http.c"

typedef struct {int64_t deadline;size_t received;bool readable;} fake_transport_t;
typedef struct {
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    fake_transport_t *transport;void *user_data;
    bool connected,has_auth,typed,dead;char range[64],if_range[68],url[256];int status,method;size_t length,delivered;
} fake_client_t;
static int64_t now=1000000,connect_cost=1500000,transfer_cost=200000;
static unsigned inits,connects,opens,cleanups,transports,destroyed,arms,auth_sets,auth_cleared,auth_deletes;
static unsigned fail_connect,fail_send;
static int status=206,json_status=200;
static bool silent,close_header,mid_body_fail;
static const char *wrong_range,*auth_buffer;
static const uint32_t total=4*65536+3;

int64_t esp_timer_get_time(void){return now;}
esp_err_t esp_crt_bundle_attach(void *p){(void)p;return ESP_OK;}
/* Every range, new or reused, gets exactly its own start + 30 s; a JSON
 * request on the kept connection gets its start + 10 s (arm_us). */
static int64_t arm_us=30000000;
esp_transport_handle_t pet_deadline_transport_create(int64_t deadline)
{fake_transport_t *t=calloc(1,sizeof(*t));assert(t&&deadline==now+30000000);t->deadline=deadline;++transports;return t;}
void pet_deadline_transport_arm(esp_transport_handle_t p,int64_t deadline)
{fake_transport_t *t=p;assert(t&&deadline==now+arm_us);t->deadline=deadline;t->received=0;++arms;}
size_t pet_deadline_transport_received(esp_transport_handle_t p){return ((fake_transport_t *)p)->received;}
int esp_transport_poll_read(esp_transport_handle_t p,int timeout){assert(!timeout);return ((fake_transport_t *)p)->readable;}
esp_err_t esp_transport_destroy(esp_transport_handle_t p){free(p);++destroyed;return ESP_OK;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{
    assert(c->disable_auto_redirect&&c->crt_bundle_attach&&c->timeout_ms==10000&&c->event_handler&&c->user_data&&c->transport);
    assert(!strncmp(c->url,"https://device.fixture.invalid/v",32));
    fake_client_t *f=calloc(1,sizeof(*f));assert(f);f->event_handler=c->event_handler;f->transport=c->transport;
    f->user_data=c->user_data;f->method=c->method;snprintf(f->url,sizeof(f->url),"%s",c->url);++inits;return f;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char *k,const char *v)
{
    fake_client_t *f=h;
    if(auth_buffer){ /* The caller's Authorization buffer is wiped before its next header. */
        for(size_t i=0;i<52;++i)assert(!auth_buffer[i]);
        auth_buffer=NULL;++auth_cleared;
    }
    if(!strcmp(k,"Authorization")){
        assert(!f->has_auth&&!strcmp(v,"Bearer aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        f->has_auth=true;auth_buffer=v;++auth_sets;
    }else if(!strcmp(k,"Range"))snprintf(f->range,sizeof(f->range),"%s",v);
    else if(!strcmp(k,"Content-Type"))f->typed=true;
    else if(!strcmp(k,"If-Range"))snprintf(f->if_range,sizeof(f->if_range),"%s",v);
    return ESP_OK;
}
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t h,const char *k)
{
    fake_client_t *f=h;
    if(!strcmp(k,"Content-Type")){bool had=f->typed;f->typed=false;return had?ESP_OK:ESP_ERR_NOT_FOUND;}
    assert(!strcmp(k,"Authorization")&&f->has_auth);f->has_auth=false;++auth_deletes;return ESP_OK;
}
/* The same host keeps the connection; the path and method are the new request's. */
static unsigned url_sets;
esp_err_t esp_http_client_set_url(esp_http_client_handle_t h,const char *url)
{
    fake_client_t *f=h;assert(!strncmp(url,"https://device.fixture.invalid/v",32));
    snprintf(f->url,sizeof(f->url),"%s",url);++url_sets;return ESP_OK;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t h,esp_http_client_method_t method)
{((fake_client_t *)h)->method=method;return ESP_OK;}
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t h,void *data){((fake_client_t *)h)->user_data=data;return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int length)
{
    fake_client_t *f=h;assert(f->has_auth&&f->user_data&&length>=0);++opens;
    if(!f->connected){
        now+=connect_cost;
        if(fail_connect){--fail_connect;return ESP_FAIL;}
        f->connected=true;++connects;
    }else if(fail_send){--fail_send;f->connected=false;return ESP_FAIL;} /* Reset by the peer: no response byte. */
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t h,const char *b,int n){(void)h;(void)b;return n;}
static void header(fake_client_t *f,const char *k,const char *v)
{esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.header_key=(char *)k,.header_value=(char *)v,.user_data=f->user_data};f->event_handler(&e);}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h)
{
    fake_client_t *f=h;assert(f->connected&&f->user_data);
    f->status=-1; /* As esp_http_client: no status until an answer. */
    if(silent){now+=31000000;return -1;} /* Nothing arrived before the deadline. */
    if(f->dead){now=f->transport->deadline;return -1;} /* Dropped by the network: silent until the deadline. */
    f->transport->received+=120;f->delivered=0;
    if(!f->range[0]){
        f->status=json_status;if(close_header)header(f,"Connection","close");
        f->length=2;return 2;
    }
    unsigned first,last;char content_range[96];assert(sscanf(f->range,"bytes=%u-%u",&first,&last)==2);
    snprintf(content_range,sizeof(content_range),"bytes %u-%u/%u",first,last,(unsigned)total);
    f->status=status;header(f,"ETag",f->if_range);header(f,"Content-Range",wrong_range?wrong_range:content_range);
    if(close_header)header(f,"Connection","keep-alive, close");
    f->length=last-first+1;return (int64_t)f->length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t h){return ((fake_client_t *)h)->status;}
int esp_http_client_read(esp_http_client_handle_t h,char *b,int n)
{
    fake_client_t *f=h;size_t count=f->length-f->delivered;if(count>(size_t)n)count=(size_t)n;
    if(mid_body_fail){if(f->delivered)return -1;count=1;}
    now+=transfer_cost;memset(b,'p',count);f->delivered+=count;f->transport->received+=count;return (int)count;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h)
{fake_client_t *f=h;return f->delivered==f->length;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h){free(h);++cleanups;return ESP_OK;}

static const pet_control_http_t http={"https://device.fixture.invalid","pet-0123456789abcdef0123456789abcdef","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
static const char *pet="00000000-0000-4000-8000-000000000001",
    *sha="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static char chunk[65536];
static bool range(pet_control_download_t *d,uint32_t offset)
{
    pet_control_http_result_t r;size_t n=total-offset<sizeof(chunk)?total-offset:sizeof(chunk);
    bool ok=pet_control_http_pet_range(&http,d,pet,sha,total,offset,chunk,n,&r);
    assert(!ok||(r.status==206&&r.bytes==n&&chunk[0]=='p'));return ok;
}
static void idle(pet_control_download_t *d){assert(!d->client==!d->transport);}

int main(void)
{
    /* Reuse: five ranges share one connection; the final range closes it.
     * Connect, transfer and flash-write time add up in the summary line. */
    pet_control_download_t d={0};
    for(uint32_t at=0;at<total;at+=65536){assert(range(&d,at));idle(&d);now+=300000;pet_control_download_wrote(&d);}
    assert(inits==1&&connects==1&&opens==5&&arms==4&&cleanups==1&&transports==1&&destroyed==1&&!d.client);
    assert(d.ranges==5&&d.reused==4&&d.connections==1&&!d.failed&&!d.independent);
    assert(auth_sets==5&&auth_cleared==5&&auth_deletes==4); /* Set, wiped and dropped per request. */
    pet_control_download_end(&d,true);
    assert(lines==1&&!strcmp(logged,"pet download complete: 262147 of 262147 bytes in 4000 ms; ranges 5, connections 1, "
        "reused 4, failed 0, fallback no; connect 1500 ms, transfer 1000 ms, write 1500 ms"));
    pet_control_download_end(&d,true);assert(lines==1&&!d.url[0]); /* One line per operation. */

    /* HTTP 5xx and range deviations discard the connection but are not
     * connection failures: no fallback, and later ranges reuse again. */
    assert(range(&d,0)&&d.client);unsigned before=inits;
    status=503;for(unsigned i=0;i<3;++i){assert(!range(&d,65536)&&!d.client&&!d.failures);}status=206;
    assert(inits==before+2&&!d.independent&&d.failed==3);
    assert(range(&d,65536)&&d.client&&inits==before+3);
    assert(range(&d,131072)&&d.client&&inits==before+3&&d.reused==1);
    wrong_range="bytes 0-65535/262147";assert(!range(&d,196608)&&!d.client&&!d.failures);wrong_range=NULL;
    mid_body_fail=true;assert(!range(&d,196608)&&!d.client&&!d.failures);mid_body_fail=false;
    close_header=true;assert(range(&d,196608)&&!d.client);close_header=false; /* Connection: close. */
    pet_control_download_end(&d,false);assert(lines==2&&strstr(logged,"pet download stopped: 262144 of 262147 bytes")&&
        strstr(logged,"ranges 4, connections 6, reused 1, failed 5, fallback no"));

    /* Two consecutive connection failures (the reused connection resets,
     * then a new one fails to connect) switch the rest of the operation to
     * one new connection per range. Nothing already written is fetched again. */
    assert(range(&d,0)&&d.client);
    fail_send=1;assert(!range(&d,65536)&&!d.client&&d.failures==1&&!d.independent);
    fail_connect=1;assert(!range(&d,65536)&&d.failures==2&&d.independent);
    before=inits;unsigned closed=cleanups;
    for(uint32_t at=65536;at<total;at+=65536){assert(range(&d,at)&&!d.client);}
    assert(inits==before+4&&cleanups==closed+4&&d.independent&&d.ranges==5&&d.bytes==total);
    pet_control_download_end(&d,true);assert(lines==3&&strstr(logged,"pet download complete: 262147 of 262147")&&
        strstr(logged,"connections 5, reused 0, failed 2, fallback yes"));

    /* Only consecutive failures count: any response, even an error or a
     * broken body, restarts the count. No byte before the deadline counts. */
    fail_connect=1;assert(!range(&d,0)&&d.failures==1);
    assert(range(&d,0)&&!d.failures);
    fail_send=1;assert(!range(&d,65536)&&d.failures==1);
    status=503;assert(!range(&d,65536)&&!d.failures);status=206;
    silent=true;assert(!range(&d,65536)&&d.failures==1&&!d.independent);silent=false;
    mid_body_fail=true;assert(!range(&d,65536)&&!d.failures);mid_body_fail=false;
    silent=true;assert(!range(&d,65536)&&d.failures==1);assert(!range(&d,65536)&&d.failures==2&&d.independent);silent=false;
    pet_control_download_end(&d,false);

    /* Each range keeps its own absolute deadline: a reused range that runs
     * past it fails and is discarded, and the next range starts afresh. */
    assert(range(&d,0)&&d.client);
    transfer_cost=31000000;assert(!range(&d,65536)&&!d.client&&!d.failures);transfer_cost=200000;
    before=inits;assert(range(&d,65536)&&d.client&&inits==before+1);
    /* An idle connection the server closed (readable while idle) is
     * discarded before use and is not a connection failure. */
    ((fake_transport_t *)d.transport)->readable=true;
    before=inits;assert(range(&d,131072)&&inits==before+1&&!d.failures&&d.reused==0);

    /* Lost connectivity closes the connection but keeps the operation; the
     * worker's end on cancel frees it and logs once. */
    closed=cleanups;unsigned freed=destroyed;
    pet_control_download_close(&d);assert(!d.client&&cleanups==closed+1&&destroyed==freed+1&&d.url[0]&&d.ranges==3);
    assert(range(&d,196608)&&d.client&&d.connections==4);
    closed=cleanups;freed=destroyed;unsigned logged_lines=lines;
    pet_control_download_end(&d,false);
    assert(!d.client&&!d.url[0]&&cleanups==closed+1&&destroyed==freed+1&&lines==logged_lines+1&&
        strstr(logged,"pet download stopped: 262144 of 262147 bytes"));

    /* Another operation never reuses this one's connection. JSON requests
     * keep one of their own, never the download's; NULL downloads keep none. */
    assert(range(&d,0)&&d.client);void *kept=d.client;
    char json[16];pet_control_http_result_t r;before=inits;closed=cleanups;
    assert(pet_control_http_json(&http,"/v2/device/pets/report","{}",json,sizeof(json),&r)&&r.status==200);
    assert(inits==before+1&&cleanups==closed&&d.client==kept&&s_json.client&&s_json.client!=kept);
    assert(pet_control_http_pet_range(&http,NULL,pet,sha,total,65536,chunk,65536,&r)&&inits==before+2&&cleanups==closed+1);
    assert(range(&d,65536)&&d.client==kept&&inits==before+2);
    logged_lines=lines;before=inits;
    assert(pet_control_http_firmware_range(&http,&d,"00000000-0000-4000-8000-000000000002",sha,total,0,chunk,65536,&r));
    assert(lines==logged_lines+1&&strstr(logged,"pet download stopped: 131072 of 262147")&&d.client&&inits==before+1);
    assert(strstr(d.url,"/v2/device/firmware/00000000-0000-4000-8000-000000000002/binary")&&d.ranges==1);
    pet_control_download_end(&d,true);assert(strstr(logged,"firmware download complete: 65536 of 262147"));
    pet_control_operation_t v1={.id="00000000-0000-4000-8000-000000000003",.bytes=total,
        .download_path="/v1/device/installations/00000000-0000-4000-8000-000000000003/pack"};
    strcpy(v1.sha256,sha);
    assert(pet_control_http_range(&http,&d,&v1,0,chunk,65536,&r)&&d.client);
    assert(pet_control_http_range(&http,&d,&v1,65536,chunk,65536,&r)&&d.reused==1);
    pet_control_download_end(&d,false);
    pet_control_http_close_kept();
    assert(inits==cleanups&&transports==destroyed&&auth_sets==auth_cleared&&!d.client&&!s_json.client);
    /* JSON requests as the Pocket's control task makes them: a pets poll, a
     * firmware poll 15 s later, and the selection after a swipe 3 s later. */
    arm_us=10000000;
    int64_t started=now;unsigned i0=inits,c0=connects,o0=opens,k0=cleanups;
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&r.status==200);
    pet_control_http_timing_t timing=pet_control_http_last_timing();
    assert(!timing.reused&&timing.connect_ms==1500&&timing.total_ms==(uint32_t)((now-started)/1000));
    int64_t first=now-started;
    now+=15000000;started=now;
    assert(pet_control_http_json(&http,"/v2/device/firmware/poll","{}",json,sizeof(json),&r));
    timing=pet_control_http_last_timing();assert(timing.reused&&!timing.connect_ms);
    now+=3000000;started=now;unsigned sets=url_sets;
    assert(pet_control_http_json(&http,"/v2/device/pets/select","{}",json,sizeof(json),&r)&&r.status==200);
    assert(pet_control_http_last_timing().reused);
    int64_t select_kept=now-started;
    fake_client_t *shared=s_json.client;
    assert(inits==i0+1&&connects==c0+1&&opens==o0+3&&cleanups==k0&&url_sets==sets+1);
    assert(!strcmp(shared->url,"https://device.fixture.invalid/v2/device/pets/select")&&shared->method==HTTP_METHOD_POST);
    assert(!shared->has_auth&&!shared->user_data); /* Credential dropped between requests. */
    printf("select after a swipe: %lld ms on a new connection, %lld ms on the kept one (fake connect 1500 ms)\n",
           (long long)(first/1000),(long long)(select_kept/1000));
    assert(first-select_kept==connect_cost);
    /* A GET after a POST drops the stale Content-Type. */
    assert(pet_control_http_json(&http,"/v1/device/context",NULL,json,sizeof(json),&r)&&pet_control_http_last_timing().reused&&!shared->typed);
    assert(shared->method==HTTP_METHOD_GET&&inits==i0+1);
    /* Idle 25 s or more: closed unused, a new connection. */
    now+=25000000;
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&!pet_control_http_last_timing().reused);
    assert(inits==i0+2&&cleanups==k0+1);
    /* The server closed it while idle (readable): a new connection. */
    ((fake_transport_t *)s_json.transport)->readable=true;
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&!pet_control_http_last_timing().reused&&inits==i0+3);
    /* Reset before any response byte: the request goes again at once, once,
     * on a new connection, and succeeds. */
    unsigned o1=opens;fail_send=1;
    assert(pet_control_http_json(&http,"/v2/device/pets/select","{}",json,sizeof(json),&r)&&r.status==200);
    assert(!pet_control_http_last_timing().reused&&inits==i0+4&&opens==o1+2&&s_json.client);
    /* The new connection failing too ends the request: no second retry. */
    fail_send=1;fail_connect=1;o1=opens;unsigned i1=inits;
    assert(!pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r));
    assert(inits==i1+1&&opens==o1+2&&!s_json.client);
    /* A kept connection the network dropped without a word never answers: the
     * request gives it 10 s, then goes again at once on a new connection. On
     * 1 Oct a dead one held a pets poll for the whole 30 s. */
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&s_json.client);
    ((fake_client_t *)s_json.client)->dead=true;i1=inits;started=now;
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&r.status==200);
    assert(!pet_control_http_last_timing().reused&&inits==i1+1&&s_json.client);
    assert(now-started>=10000000&&now-started<10000000+connect_cost+1000000);
    /* No answer at all is status 0, never the client's -1: the pet worker
     * keeps an idle pet's session through a poll that got none. */
    pet_control_http_close_kept();silent=true;
    assert(!pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&r.status==0);
    silent=false;
    /* A 5xx answer or Connection: close is not kept; a 4xx answer is. */
    json_status=503;assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&r.status==503);
    assert(!s_json.client);json_status=409;
    assert(pet_control_http_json(&http,"/v2/device/pets/select","{}",json,sizeof(json),&r)&&r.status==409&&s_json.client);
    json_status=200;close_header=true;
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&pet_control_http_last_timing().reused&&!s_json.client);
    close_header=false;
    /* Another origin never reuses it. */
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&s_json.client);
    const pet_control_http_t other={"https://device2.fixture.invalid",http.device_id,http.credential};
    (void)other; /* The fake serves one origin; valid_endpoint checks the rest (control_http_test.c). */
    strcpy(s_json.origin,"https://device2.fixture.invalid");i1=inits;
    assert(pet_control_http_json(&http,"/v2/device/pets/poll","{}",json,sizeof(json),&r)&&!pet_control_http_last_timing().reused&&inits==i1+1);
    pet_control_http_close_kept();
    assert(!s_json.client&&inits==cleanups&&transports==destroyed&&auth_sets==auth_cleared);
    puts("control downloads: one kept connection per operation, per-range deadline and credential, discard on any deviation, "
         "fallback after two connection failures, and one summary line passed; control JSON requests share one kept "
         "connection, with a fresh connection when it is idle, closed or reset");
}
