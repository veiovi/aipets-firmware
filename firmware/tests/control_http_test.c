#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pet_control_http.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "pet_deadline_transport.h"

static esp_http_client_config_t cfg;
static char auth[64],identity[81],request_range[64],if_range[68],request_url[256];
static const char *etag,*content_range,*retry,*encoding;
static int status=206;
static size_t declared=3,actual=3,received;
static bool complete=true,duplicate;
static int64_t now,clock_step;
static unsigned calls,cleanups,destroys;
esp_transport_handle_t pet_deadline_transport_create(int64_t deadline){(void)deadline;return (void *)1;}
esp_err_t esp_transport_destroy(esp_transport_handle_t t){(void)t;++destroys;return ESP_OK;}
/* A NULL download is one connection per request. JSON requests keep theirs
 * (control_download_test.c covers the reuse); here the kept one is only
 * handed back and replaced. */
static unsigned reuses;
void pet_deadline_transport_arm(esp_transport_handle_t t,int64_t deadline){(void)t;(void)deadline;++reuses;}
size_t pet_deadline_transport_received(esp_transport_handle_t t){(void)t;return received;}
int esp_transport_poll_read(esp_transport_handle_t t,int timeout){(void)t;(void)timeout;return 1;} /* the server closed it */
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t h,void *data){(void)h;cfg.user_data=data;return ESP_OK;}
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t h,const char *k){(void)h;(void)k;return ESP_OK;}
esp_err_t esp_http_client_set_url(esp_http_client_handle_t h,const char *url){(void)h;strcpy(request_url,url);return ESP_OK;}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t h,esp_http_client_method_t m){(void)h;(void)m;return ESP_OK;}
int64_t esp_timer_get_time(void){int64_t result=now;now+=clock_step;return result;}
esp_err_t esp_crt_bundle_attach(void *p){(void)p;return ESP_OK;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{cfg=*c;strcpy(request_url,c->url);++calls;received=0;assert(c->disable_auto_redirect&&c->crt_bundle_attach);assert(!strstr(c->url,"secret"));return &cfg;}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char *k,const char *v)
{(void)h;if(!strcmp(k,"Authorization"))strcpy(auth,v);if(!strcmp(k,"X-Device-Id"))strcpy(identity,v);if(!strcmp(k,"Range"))strcpy(request_range,v);if(!strcmp(k,"If-Range"))strcpy(if_range,v);return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n){(void)h;(void)n;return ESP_OK;}
int esp_http_client_write(esp_http_client_handle_t h,const char *b,int n){(void)h;(void)b;return n>2?2:n;}
static void header(const char *k,const char *v)
{if(!v)return;esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.header_key=(char *)k,.header_value=(char *)v,.user_data=cfg.user_data};cfg.event_handler(&e);}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h)
{(void)h;header("ETag",etag);if(duplicate)header("etag",etag);header("content-range",content_range);header("Retry-After",retry);header("Content-Encoding",encoding);return (int64_t)declared;}
int esp_http_client_get_status_code(esp_http_client_handle_t h){(void)h;return status;}
int esp_http_client_read(esp_http_client_handle_t h,char *b,int n)
{(void)h;size_t count=actual-received;if(count>(size_t)n)count=(size_t)n;memset(b,'a',count);received+=count;return (int)count;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h){(void)h;return complete&&received==actual;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h){(void)h;++cleanups;return ESP_OK;}

int main(void)
{
    pet_control_http_t h={"https://fixture.invalid","pet-0123456789abcdef0123456789abcdef","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    pet_control_operation_t op={.id="00000000-0000-4000-8000-000000000001",.sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.bytes=65539,
        .download_path="/v1/device/installations/00000000-0000-4000-8000-000000000001/pack"};
    etag="\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"";content_range="bytes 65536-65538/65539";
    char bytes[128];pet_control_http_result_t r;
    assert(pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));
    assert(!strcmp(auth,"Bearer aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")&&!strcmp(identity,h.device_id));
    assert(!strcmp(request_range,"bytes=65536-65538")&&!strcmp(if_range,etag));
    assert(pet_control_http_firmware_range(&h,NULL,op.id,op.sha256,op.bytes,65536,bytes,3,&r));
    unsigned firmware_calls=calls;
    assert(!pet_control_http_firmware_range(&h,NULL,"../00000-0000-4000-8000-000000000001",op.sha256,op.bytes,0,bytes,3,&r));
    assert(!pet_control_http_firmware_range(&h,NULL,op.id,op.sha256,0x400001,0,bytes,3,&r));
    assert(!pet_control_http_firmware_range(&h,NULL,op.id,op.sha256,op.bytes,65536,bytes,4,&r));
    assert(!pet_control_http_firmware_range(&h,NULL,op.id,op.sha256,op.bytes,0,bytes,65537,&r)&&calls==firmware_calls);
    assert(pet_control_http_pet_range(&h,NULL,op.id,op.sha256,op.bytes,65536,bytes,3,&r));
    assert(!strcmp(request_url,"https://fixture.invalid/v2/device/pets/00000000-0000-4000-8000-000000000001/pack"));
    unsigned pet_calls=calls;
    assert(!pet_control_http_pet_range(&h,NULL,"../00000-0000-4000-8000-000000000001",op.sha256,op.bytes,0,bytes,3,&r));
    assert(!pet_control_http_pet_range(&h,NULL,"00000000-0000-6000-8000-000000000001",op.sha256,op.bytes,0,bytes,3,&r));
    assert(!pet_control_http_pet_range(&h,NULL,"00000000-0000-4000-7000-000000000001",op.sha256,op.bytes,0,bytes,3,&r));
    assert(!pet_control_http_pet_range(&h,NULL,op.id,op.sha256,0xbbe001,0,bytes,3,&r));
    assert(!pet_control_http_pet_range(&h,NULL,op.id,op.sha256,0,0,bytes,3,&r));
    assert(!pet_control_http_pet_range(&h,NULL,op.id,op.sha256,op.bytes,65536,bytes,4,&r));
    assert(!pet_control_http_pet_range(&h,NULL,op.id,op.sha256,op.bytes,0,bytes,65537,&r)&&calls==pet_calls);
    for(unsigned fault=0;fault<8;++fault){
        status=fault==0?200:fault==1?302:206;duplicate=fault==2;
        content_range=fault==3?"bytes 0-2/65539":"bytes 65536-65538/65539";
        encoding=fault==4?"gzip":NULL;actual=fault==5?2:3;complete=fault!=6;clock_step=fault==7?31000000:0;
        assert(!pet_control_http_pet_range(&h,NULL,op.id,op.sha256,op.bytes,65536,bytes,3,&r));
    }
    status=206;duplicate=false;content_range="bytes 65536-65538/65539";encoding=NULL;actual=3;complete=true;clock_step=0;
    status=200;assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));
    status=302;assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));
    status=206;duplicate=true;assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));duplicate=false;
    content_range="bytes 0-2/65539";assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));content_range="bytes 65536-65538/65539";
    encoding="gzip";assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));encoding=NULL;
    actual=2;assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));actual=3;
    complete=false;assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));complete=true;
    clock_step=31000000;assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r));clock_step=0;
    status=429;retry="30";assert(!pet_control_http_range(&h,NULL,&op,65536,bytes,3,&r)&&r.retry_seconds==30);retry=NULL;
    status=200;etag=content_range=NULL;assert(pet_control_http_json(&h,"/v1/device/context",NULL,bytes,sizeof(bytes),&r)&&r.bytes==3);
    assert(pet_control_http_json(&h,"/v2/device/firmware/poll","{}",bytes,sizeof(bytes),&r));
    assert(pet_control_http_json(&h,"/v2/device/firmware/report","{}",bytes,sizeof(bytes),&r));
    assert(pet_control_http_json(&h,"/v2/device/pets/poll","{}",bytes,sizeof(bytes),&r));
    assert(pet_control_http_json(&h,"/v2/device/pets/fence","{}",bytes,sizeof(bytes),&r));
    assert(pet_control_http_json(&h,"/v2/device/pets/report","{}",bytes,sizeof(bytes),&r));
    assert(pet_control_http_json(&h,"/v2/device/pets/select","{}",bytes,sizeof(bytes),&r));
    firmware_calls=calls;
    assert(!pet_control_http_json(&h,"/v2/device/pets/select/extra","{}",bytes,sizeof(bytes),&r));
    assert(!pet_control_http_json(&h,"/v2/device/pets/fence?redirect=1","{}",bytes,sizeof(bytes),&r));
    assert(!pet_control_http_json(&h,"/v2/device/pets/../other","{}",bytes,sizeof(bytes),&r));
    assert(!pet_control_http_json(&h,"/internal/example/queue","{}",bytes,sizeof(bytes),&r));
    assert(!pet_control_http_json(&h,"/v2/device/pets/00000000-0000-4000-8000-000000000001/pack/extra",NULL,bytes,sizeof(bytes),&r));
    assert(!pet_control_http_json(&h,"/v2/device/firmware/report?redirect=1","{}",bytes,sizeof(bytes),&r));
    assert(!pet_control_http_json(&h,"/v2/device/firmware/../other","{}",bytes,sizeof(bytes),&r));
    assert(!pet_control_http_json(&h,"/v2/admin/firmware/queue","{}",bytes,sizeof(bytes),&r)&&calls==firmware_calls);
    unsigned before=calls;h.origin="https://fixture.invalid@evil.invalid";
    assert(!pet_control_http_json(&h,"/v1/device/context",NULL,bytes,sizeof(bytes),&r)&&calls==before);
    h.origin="https://fixture.invalid";h.credential="bad\r\nheader";
    assert(!pet_control_http_json(&h,"/v1/device/context",NULL,bytes,sizeof(bytes),&r)&&calls==before);
    char path[1701];assert(pet_control_http_library_path("a/b?c&d=é",path,sizeof(path)));
    assert(!strcmp(path,"/v1/device/library?limit=10&cursor=a%2Fb%3Fc%26d%3D%C3%A9"));
    assert(!pet_control_http_library_path("abc",path,5));
    pet_control_http_close_kept();
    assert(cleanups==calls&&destroys==calls&&reuses>0);
    puts("control HTTP: authenticated origin, exact ranges, bounds, redirects, deadlines and backoff passed");
}
