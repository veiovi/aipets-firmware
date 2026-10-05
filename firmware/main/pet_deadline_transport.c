#include "pet_deadline_transport.h"
#include <errno.h>
#include <stdlib.h>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct { esp_transport_handle_t tls;int64_t deadline;size_t received; } deadline_t;
static int remaining(deadline_t *d,int requested)
{
    int64_t us=d->deadline-esp_timer_get_time();
    if(us<=0){errno=ETIMEDOUT;return -1;}
    int ms=(int)((us+999)/1000);
    return requested>=0&&requested<ms?requested:ms;
}
static int connect_to(esp_transport_handle_t t,const char *host,int port,int timeout)
{
    deadline_t *d=esp_transport_get_context_data(t);
    for(;;) {
        int ms=remaining(d,timeout);if(ms<0)return -1;
        /* Async connect keeps cfg.non_block=true after the handshake. A
         * blocking TLS record read could otherwise outlive an outer poll. */
        int result=esp_transport_connect_async(d->tls,host,port,ms);
        if(result<0)return -1;
        if(result==1)return remaining(d,timeout)<0?-1:0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static int read_from(esp_transport_handle_t t,char *buffer,int bytes,int timeout)
{
    deadline_t *d=esp_transport_get_context_data(t);
    for(;;) {
        int ms=remaining(d,timeout);if(ms<0)return -1;
        int result=esp_transport_read(d->tls,buffer,bytes,ms);
        if(result!=0&&result!=ESP_TLS_ERR_SSL_WANT_READ&&result!=ESP_TLS_ERR_SSL_WANT_WRITE){
            if(result>0)d->received+=(size_t)result;
            return result;
        }
        vTaskDelay(1);
    }
}
static int write_to(esp_transport_handle_t t,const char *buffer,int bytes,int timeout)
{
    deadline_t *d=esp_transport_get_context_data(t);
    for(;;) {
        int ms=remaining(d,timeout);if(ms<0)return -1;
        int result=esp_transport_write(d->tls,buffer,bytes,ms);
        if(result!=0&&result!=ESP_TLS_ERR_SSL_WANT_READ&&result!=ESP_TLS_ERR_SSL_WANT_WRITE)return result;
        vTaskDelay(1);
    }
}
static int poll_read(esp_transport_handle_t t,int timeout)
{deadline_t *d=esp_transport_get_context_data(t);int ms=remaining(d,timeout);return ms<0?-1:esp_transport_poll_read(d->tls,ms);}
static int poll_write(esp_transport_handle_t t,int timeout)
{deadline_t *d=esp_transport_get_context_data(t);int ms=remaining(d,timeout);return ms<0?-1:esp_transport_poll_write(d->tls,ms);}
static int close_transport(esp_transport_handle_t t)
{deadline_t *d=esp_transport_get_context_data(t);return esp_transport_close(d->tls);}
static int destroy_transport(esp_transport_handle_t t)
{deadline_t *d=esp_transport_get_context_data(t);esp_transport_destroy(d->tls);free(d);return 0;}

esp_transport_handle_t pet_deadline_transport_create(int64_t deadline_us)
{
    deadline_t *d=calloc(1,sizeof(*d));if(!d)return NULL;
    d->deadline=deadline_us;d->tls=esp_transport_ssl_init();
    esp_transport_handle_t t=d->tls?esp_transport_init():NULL;
    if(!t){if(d->tls)esp_transport_destroy(d->tls);free(d);return NULL;}
    esp_transport_ssl_crt_bundle_attach(d->tls,esp_crt_bundle_attach);
    esp_transport_set_default_port(t,443);esp_transport_set_context_data(t,d);
    esp_transport_set_func(t,connect_to,read_from,write_to,close_transport,poll_read,poll_write,destroy_transport);
    return t;
}
void pet_deadline_transport_arm(esp_transport_handle_t t,int64_t deadline_us)
{deadline_t *d=esp_transport_get_context_data(t);if(d){d->deadline=deadline_us;d->received=0;}}
size_t pet_deadline_transport_received(esp_transport_handle_t t)
{deadline_t *d=esp_transport_get_context_data(t);return d?d->received:0;}
