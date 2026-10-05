#include "pet_control_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pet_deadline_transport.h"
#include "pet_replace.h"

static const char *TAG="pet_download";
typedef struct {
    char etag[68],range[96];
    bool bad,has_etag,has_range,has_retry,encoded,close;
    unsigned retry;
} headers_t;
typedef struct {const char *sha256;uint32_t bytes;pet_control_download_t *download;} range_request_t;

static bool firmware_path(const char *path)
{
    if(!strcmp(path,"/v2/device/firmware/poll")||!strcmp(path,"/v2/device/firmware/report"))return true;
    const char *prefix="/v2/device/firmware/";size_t n=strlen(prefix);
    if(strncmp(path,prefix,n)||strlen(path)!=n+36+7||strcmp(path+n+36,"/binary"))return false;
    for(unsigned i=0;i<36;++i){char c=path[n+i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return false;}
        else if(!strchr("0123456789abcdef",c))return false;
    }
    return true;
}

static bool pet_path(const char *path)
{
    if(!strcmp(path,"/v2/device/pets/poll")||!strcmp(path,"/v2/device/pets/fence")||
       !strcmp(path,"/v2/device/pets/report")||!strcmp(path,"/v2/device/pets/select"))return true;
    const char *prefix="/v2/device/pets/";size_t n=strlen(prefix);
    if(strncmp(path,prefix,n)||strlen(path)!=n+36+5||strcmp(path+n+36,"/pack"))return false;
    for(unsigned i=0;i<36;++i){char c=path[n+i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return false;}
        else if(!strchr("0123456789abcdef",c))return false;
    }
    return path[n+14]>='1'&&path[n+14]<='5'&&strchr("89ab",path[n+19]);
}

static esp_err_t header_event(esp_http_client_event_t *event)
{
    headers_t *h=event->user_data;
    if(event->event_id!=HTTP_EVENT_ON_HEADER||!h||!event->header_key||!event->header_value)return ESP_OK;
    const char *v=event->header_value;
    if(!strcasecmp(event->header_key,"ETag")) {
        if(h->has_etag||strlen(v)>=sizeof(h->etag))h->bad=true;
        else {h->has_etag=true;strcpy(h->etag,v);}
    }else if(!strcasecmp(event->header_key,"Content-Range")) {
        if(h->has_range||strlen(v)>=sizeof(h->range))h->bad=true;
        else {h->has_range=true;strcpy(h->range,v);}
    }else if(!strcasecmp(event->header_key,"Retry-After")) {
        char *end;unsigned long seconds=strtoul(v,&end,10);
        if(h->has_retry||!v[0]||*end||strspn(v,"0123456789")!=strlen(v)||seconds>3600)h->bad=true;
        else {h->has_retry=true;h->retry=seconds<5?5:(unsigned)seconds;}
    }else if(!strcasecmp(event->header_key,"Content-Encoding")&&strcasecmp(v,"identity"))h->encoded=true;
    else if(!strcasecmp(event->header_key,"Connection"))
        for(const char *p=v;*p;++p)if(!strncasecmp(p,"close",5))h->close=true;
    return ESP_OK;
}

static bool valid_endpoint(const pet_control_http_t *h,const char *path)
{
    if(!h||!h->origin||!h->device_id||!h->credential||!path||
       strncmp(h->origin,"https://",8)||!h->origin[8]||strlen(h->origin)>=128||
       (strncmp(path,"/v1/device/",11)&&!firmware_path(path)&&!pet_path(path))||strlen(path)>1700)return false;
    for(const char *p=h->origin+8;*p;++p)
        if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='.'||*p==':'))return false;
    if(strlen(h->device_id)!=36||strncmp(h->device_id,"pet-",4)||
       strspn(h->device_id+4,"0123456789abcdef")!=32||strlen(h->credential)!=43||
       strspn(h->credential,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")!=43)return false;
    for(const unsigned char *p=(const unsigned char *)path;*p;++p)
        if(*p<=32||*p>=127||*p=='#'||*p=='\\')return false;
    const char *query=strchr(path,'?'),*traversal=strstr(path,"..");
    return !traversal||(query&&traversal>query);
}

static void discard(pet_control_download_t *d)
{
    if(d->client)esp_http_client_cleanup(d->client);
    if(d->transport)esp_transport_destroy(d->transport);
    d->client=d->transport=NULL;
}

/* The control task's JSON requests (polls, selection, fences, reports) share
 * one kept HTTPS connection to the pinned origin while it is idle less than
 * JSON_KEEP_IDLE_US; a new TLS handshake cost more than most of those calls.
 * Each request keeps its own absolute deadline, credential and checks. */
#define JSON_KEEP_IDLE_US 25000000
/* A kept connection the network dropped without a word never answers: a
 * request gets this long on it, then goes again at once on a new one. Kept
 * requests took 1-2.5 s on a phone hotspot, 8.4 s at worst; a dead one held
 * a pets poll for its whole 30 s deadline (1 Oct). */
#define JSON_KEPT_ANSWER_US 10000000
static struct {void *client,*transport;char origin[64];int64_t idle_since;} s_json; /* A longer origin keeps none. */
static void json_discard(void)
{
    if(s_json.client)esp_http_client_cleanup(s_json.client);
    if(s_json.transport)esp_transport_destroy(s_json.transport);
    s_json.client=s_json.transport=NULL;
}
void pet_control_http_close_kept(void){json_discard();}
static pet_control_http_timing_t s_timing;
pet_control_http_timing_t pet_control_http_last_timing(void){return s_timing;}

static bool request(const pet_control_http_t *h,const char *path,const char *body,
                      const range_request_t *op,uint32_t offset,
                      void *response,size_t capacity,pet_control_http_result_t *result,bool retried)
{
    if(result)*result=(pet_control_http_result_t){.retry_seconds=30};
    if(!result||!response||!capacity||!valid_endpoint(h,path)||
       (body&&strlen(body)>8192)||(!op&&capacity>PET_CONTROL_RESPONSE_MAX))return false;
    char url[1830],authorization[52],range[64],etag[68],expected_range[96];
    snprintf(url,sizeof(url),"%s%s",h->origin,path);
    headers_t headers={.retry=30};
    int64_t started=esp_timer_get_time(),deadline=started+30000000,opened=started;
    pet_control_download_t *d=op?op->download:NULL;
    if(d&&(strcmp(d->url,url)||strcmp(d->sha256,op->sha256)||d->total!=op->bytes)){
        pet_control_download_end(d,false); /* A different operation: the previous one is over. */
        if(strlen(url)>=sizeof(d->url))d=NULL; /* Unreachable for valid paths: a connection of its own. */
        else {strcpy(d->url,url);strcpy(d->sha256,op->sha256);d->total=op->bytes;d->started_us=started;}
    }
    esp_http_client_handle_t client=NULL;esp_transport_handle_t transport=NULL;
    bool ok=false,reused=false,attempted=false;esp_err_t opening=ESP_FAIL;
    if(d&&d->client){
        /* A kept connection is idle: anything readable (a close, a reset or an
         * unrequested byte) discards it. This range gets its own deadline. */
        client=d->client;transport=d->transport;d->client=d->transport=NULL;
        pet_deadline_transport_arm(transport,deadline);
        reused=!esp_transport_poll_read(transport,0)&&esp_http_client_set_user_data(client,&headers)==ESP_OK;
        if(!reused){esp_http_client_cleanup(client);esp_transport_destroy(transport);client=NULL;}
    }else if(!op&&s_json.client){
        /* The same checks, and it must be the same origin and recently used;
         * the new request's path and method replace the last one's. */
        client=s_json.client;transport=s_json.transport;s_json.client=s_json.transport=NULL;
        pet_deadline_transport_arm(transport,started+JSON_KEPT_ANSWER_US);
        esp_err_t typed=body?ESP_OK:esp_http_client_delete_header(client,"Content-Type");
        reused=!strcmp(s_json.origin,h->origin)&&started-s_json.idle_since<JSON_KEEP_IDLE_US&&
            !esp_transport_poll_read(transport,0)&&esp_http_client_set_user_data(client,&headers)==ESP_OK&&
            esp_http_client_set_url(client,url)==ESP_OK&&
            esp_http_client_set_method(client,body?HTTP_METHOD_POST:HTTP_METHOD_GET)==ESP_OK&&
            (typed==ESP_OK||typed==ESP_ERR_NOT_FOUND);
        if(!reused){esp_http_client_cleanup(client);esp_transport_destroy(transport);client=NULL;}
    }
    if(!client){
        transport=pet_deadline_transport_create(deadline);
        if(!transport)return false;
        esp_http_client_config_t config={.url=url,.method=body?HTTP_METHOD_POST:HTTP_METHOD_GET,
            .timeout_ms=10000,.crt_bundle_attach=esp_crt_bundle_attach,.disable_auto_redirect=true,
            .event_handler=header_event,.user_data=&headers};
#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
        config.transport=transport;
#else
        /* Never silently fall back to a transport without the absolute deadline. */
        esp_transport_destroy(transport);return false;
#endif
        client=esp_http_client_init(&config);
        if(!client){esp_transport_destroy(transport);return false;}
    }
    snprintf(authorization,sizeof(authorization),"Bearer %s",h->credential);
    esp_err_t auth=esp_http_client_set_header(client,"Authorization",authorization);
    pet_enrollment_clear(authorization,sizeof(authorization));
    if(auth!=ESP_OK||esp_http_client_set_header(client,"X-Device-Id",h->device_id)!=ESP_OK||
       esp_http_client_set_header(client,"Accept-Encoding","identity")!=ESP_OK)goto done;
    if(body&&esp_http_client_set_header(client,"Content-Type","application/json")!=ESP_OK)goto done;
    if(op) {
        snprintf(range,sizeof(range),"bytes=%u-%u",(unsigned)offset,(unsigned)(offset+capacity-1));
        snprintf(etag,sizeof(etag),"\"%s\"",op->sha256);
        snprintf(expected_range,sizeof(expected_range),"bytes %u-%u/%u",(unsigned)offset,
            (unsigned)(offset+capacity-1),(unsigned)op->bytes);
        if(esp_http_client_set_header(client,"Range",range)!=ESP_OK||
           esp_http_client_set_header(client,"If-Range",etag)!=ESP_OK)goto done;
    }
    size_t length=body?strlen(body):0;
    attempted=true;opening=esp_http_client_open(client,(int)length);opened=esp_timer_get_time();
    if(opening!=ESP_OK)goto done;
    for(size_t sent=0;sent<length;) {
        if(esp_timer_get_time()>deadline)goto done;
        int n=esp_http_client_write(client,body+sent,(int)(length-sent));
        if(n<=0||(size_t)n>length-sent)goto done;
        sent+=(size_t)n;
    }
    int64_t length_header=esp_http_client_fetch_headers(client);
    /* No answer is status 0, as for a request that never went out. The
     * client says -1, which the pet worker took for an answer: a pets poll
     * that timed out dropped an idle pet's session (1 Oct). */
    result->status=length_header<0?0:esp_http_client_get_status_code(client);result->retry_seconds=headers.retry;
    if(length_header<0||length_header>(int64_t)capacity||headers.bad||headers.encoded||
       result->status<200||result->status>=600||(result->status>=300&&result->status<400))goto done;
    if(op&&(result->status!=206||length_header!=(int64_t)capacity||!headers.has_etag||
        !headers.has_range||strcmp(headers.etag,etag)||strcmp(headers.range,expected_range)))goto done;
    while(result->bytes<capacity) {
        if(esp_timer_get_time()>deadline)goto done;
        int n=esp_http_client_read(client,(char *)response+result->bytes,(int)(capacity-result->bytes));
        if(n<0||(size_t)n>capacity-result->bytes)goto done;
        if(!n)break;
        result->bytes+=(size_t)n;
    }
    ok=esp_http_client_is_complete_data_received(client)&&esp_timer_get_time()<=deadline&&
        (!op||result->bytes==capacity);
done:
    s_timing=(pet_control_http_timing_t){.connect_ms=reused?0u:(uint32_t)((opened-started)/1000),
        .total_ms=(uint32_t)((esp_timer_get_time()-started)/1000),.reused=reused};
    /* Each JSON request's time tells a slow handshake from a kept connection
     * that never answered: one cloud step held the Pocket 13 s on 1 Oct. */
    if(!op){
        int shown=(int)strcspn(path,"?");
        if(shown>64)shown=64;
        /* A failed request's status may be the kept connection's last answer. */
        if(ok)ESP_LOGI(TAG,"%s %.*s: HTTP %d, %lu ms (connect %lu ms, %s)",body?"POST":"GET",shown,path,
            result->status,(unsigned long)s_timing.total_ms,(unsigned long)s_timing.connect_ms,reused?"kept":"new");
        else ESP_LOGW(TAG,"%s %.*s: failed after %lu ms (connect %lu ms, %s)",body?"POST":"GET",shown,path,
            (unsigned long)s_timing.total_ms,(unsigned long)s_timing.connect_ms,reused?"kept":"new");
    }
    if(d){
        int64_t now=esp_timer_get_time();
        if(!reused)d->connect_us+=opened-started;
        d->transfer_us+=now-(reused?started:opened);
        d->connections+=!reused&&opening==ESP_OK;
        if(ok){++d->ranges;d->bytes+=(uint32_t)capacity;d->reused+=reused;d->returned_us=now;}
        else ++d->failed;
        /* Only a request that reached the network and got no response byte
         * is a connection failure; any response restarts the count. */
        if(ok||pet_deadline_transport_received(transport))d->failures=0;
        else if(attempted&&++d->failures>=2)d->independent=true;
        /* Keep only a fully consumed exchange that more ranges will follow. */
        if(ok&&!d->independent&&!headers.close&&offset+capacity<op->bytes&&
           esp_http_client_delete_header(client,"Authorization")==ESP_OK&&
           esp_http_client_set_user_data(client,NULL)==ESP_OK){d->client=client;d->transport=transport;client=NULL;}
    }
    /* A kept JSON connection that failed before any response byte was one the
     * server had already closed: the request goes again, once, at once, on a
     * new connection. The protocol's requests are safe to repeat. */
    bool stale=!op&&reused&&!ok&&transport&&!pet_deadline_transport_received(transport)&&esp_timer_get_time()<deadline;
    if(!op&&client&&ok&&!headers.close&&result->status<500&&strlen(h->origin)<sizeof(s_json.origin)&&
       esp_http_client_delete_header(client,"Authorization")==ESP_OK&&esp_http_client_set_user_data(client,NULL)==ESP_OK){
        json_discard(); /* Never two: a request's own connection replaces an older one. */
        s_json.client=client;s_json.transport=transport;strcpy(s_json.origin,h->origin);
        s_json.idle_since=esp_timer_get_time();client=NULL;
    }
    if(client){esp_http_client_cleanup(client);esp_transport_destroy(transport);}
    if(stale&&!retried)return request(h,path,body,op,offset,response,capacity,result,true);
    return ok;
}

bool pet_control_http_json(const pet_control_http_t *h,const char *path,const char *body,
                            char *response,size_t capacity,pet_control_http_result_t *result)
{ return request(h,path,body,NULL,0,response,capacity,result,false); }

bool pet_control_http_range(const pet_control_http_t *h,pet_control_download_t *download,const pet_control_operation_t *op,
                             uint32_t offset,void *response,size_t bytes,pet_control_http_result_t *result)
{
    if(!op||!memchr(op->id,0,sizeof(op->id))||strlen(op->id)!=36||
       !memchr(op->sha256,0,sizeof(op->sha256))||strlen(op->sha256)!=64||
       strspn(op->sha256,"0123456789abcdef")!=64||op->bytes>PET_INSTALL_PACK_MAX||
       offset>=op->bytes||!bytes||bytes>PET_INSTALL_CHECKPOINT_BYTES||bytes>op->bytes-offset)return false;
    char path[100];snprintf(path,sizeof(path),"/v1/device/installations/%s/pack",op->id);
    if(!memchr(op->download_path,0,sizeof(op->download_path))||strcmp(path,op->download_path))return false;
    range_request_t range={.sha256=op->sha256,.bytes=op->bytes,.download=download};
    return request(h,path,NULL,&range,offset,response,bytes,result,false);
}

bool pet_control_http_firmware_range(const pet_control_http_t *h,pet_control_download_t *download,const char *operation,
                                      const char *sha,uint32_t total,uint32_t offset,
                                      void *response,size_t bytes,pet_control_http_result_t *result)
{
    if(result)*result=(pet_control_http_result_t){.retry_seconds=30};
    if(!operation||!sha||strlen(operation)!=36||strlen(sha)!=64||strspn(sha,"0123456789abcdef")!=64||
       !total||total>0x400000||offset>=total||!bytes||bytes>PET_INSTALL_CHECKPOINT_BYTES||bytes>total-offset)return false;
    char path[80];snprintf(path,sizeof(path),"/v2/device/firmware/%s/binary",operation);
    if(!firmware_path(path))return false;
    range_request_t range={.sha256=sha,.bytes=total,.download=download};
    return request(h,path,NULL,&range,offset,response,bytes,result,false);
}

bool pet_control_http_pet_range(const pet_control_http_t *h,pet_control_download_t *download,const char *operation,
                                 const char *sha,uint32_t total,uint32_t offset,
                                 void *response,size_t bytes,pet_control_http_result_t *result)
{
    if(result)*result=(pet_control_http_result_t){.retry_seconds=30};
    if(!operation||!sha||strlen(operation)!=36||strlen(sha)!=64||strspn(sha,"0123456789abcdef")!=64||
       !total||total>PET_REPLACE_MAX_BYTES||offset>=total||!bytes||bytes>PET_REPLACE_CHECKPOINT_BYTES||bytes>total-offset)return false;
    char path[80];snprintf(path,sizeof(path),"/v2/device/pets/%s/pack",operation);
    if(!pet_path(path))return false;
    range_request_t range={.sha256=sha,.bytes=total,.download=download};
    return request(h,path,NULL,&range,offset,response,bytes,result,false);
}

void pet_control_download_wrote(pet_control_download_t *d)
{if(d&&d->returned_us){d->write_us+=esp_timer_get_time()-d->returned_us;d->returned_us=0;}}

void pet_control_download_close(pet_control_download_t *d)
{if(d)discard(d);}

void pet_control_download_end(pet_control_download_t *d,bool complete)
{
    if(!d)return;
    discard(d);
    if(!d->url[0])return;
    (void)complete; /* Read only by the log line. */
    ESP_LOGI(TAG,"%s download %s: %lu of %lu bytes in %lu ms; ranges %lu, connections %lu, reused %lu, failed %lu, "
        "fallback %s; connect %lu ms, transfer %lu ms, write %lu ms",strstr(d->url,"/firmware/")?"firmware":"pet",
        complete?"complete":"stopped",(unsigned long)d->bytes,(unsigned long)d->total,
        (unsigned long)((esp_timer_get_time()-d->started_us)/1000),(unsigned long)d->ranges,(unsigned long)d->connections,
        (unsigned long)d->reused,(unsigned long)d->failed,d->independent?"yes":"no",(unsigned long)(d->connect_us/1000),
        (unsigned long)(d->transfer_us/1000),(unsigned long)(d->write_us/1000));
    memset(d,0,sizeof(*d));
}

bool pet_control_http_library_path(const char *cursor,char *path,size_t capacity)
{
    const char *prefix="/v1/device/library?limit=10";
    if(!cursor||strlen(cursor)>512||!path||capacity<strlen(prefix)+1)return false;
    strcpy(path,prefix);size_t n=strlen(path);
    if(!cursor[0])return true;
    if(capacity-n<9)return false;
    memcpy(path+n,"&cursor=",8);
    n+=8;
    const char *hex="0123456789ABCDEF";
    for(const unsigned char *p=(const unsigned char *)cursor;*p;++p) {
        bool safe=(*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||strchr("-_.~",*p);
        if(capacity-n<(safe?2u:4u))return false;
        if(safe)path[n++]=(char)*p;else {path[n++]='%';path[n++]=hex[*p>>4];path[n++]=hex[*p&15];}
    }
    path[n]=0;return true;
}
