#include "pet_deadline_transport.h"
#include "esp_transport_ssl.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/task.h"

typedef struct { void *context;connect_func connect;io_read_func read;io_func write;trans_func close,destroy;poll_func poll_read,poll_write;bool tls; } transport_t;
static int64_t now;
static unsigned reads,closes,destroys;
static bool bundle;
int64_t esp_timer_get_time(void){return now;}
void vTaskDelay(TickType_t ticks){now+=(int64_t)ticks*1000;}
esp_err_t esp_crt_bundle_attach(void *p){(void)p;return ESP_OK;}
esp_transport_handle_t esp_transport_init(void){return calloc(1,sizeof(transport_t));}
esp_transport_handle_t esp_transport_ssl_init(void){transport_t *t=esp_transport_init();t->tls=true;return t;}
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t p,esp_err_t (*attach)(void *)){assert(((transport_t *)p)->tls&&attach==esp_crt_bundle_attach);bundle=true;}
esp_err_t esp_transport_destroy(esp_transport_handle_t p){transport_t *t=p;if(t->destroy)t->destroy(t);++destroys;free(t);return ESP_OK;}
esp_err_t esp_transport_set_default_port(esp_transport_handle_t p,int port){(void)p;assert(port==443);return ESP_OK;}
esp_err_t esp_transport_set_context_data(esp_transport_handle_t p,void *c){((transport_t *)p)->context=c;return ESP_OK;}
void *esp_transport_get_context_data(esp_transport_handle_t p){return ((transport_t *)p)->context;}
esp_err_t esp_transport_set_func(esp_transport_handle_t p,connect_func c,io_read_func r,io_func w,trans_func close,poll_func pr,poll_func pw,trans_func d)
{transport_t *t=p;t->connect=c;t->read=r;t->write=w;t->close=close;t->poll_read=pr;t->poll_write=pw;t->destroy=d;return ESP_OK;}
int esp_transport_connect(esp_transport_handle_t p,const char *h,int port,int timeout){(void)p;(void)h;(void)port;assert(timeout<=10000);return 0;}
int esp_transport_connect_async(esp_transport_handle_t p,const char *h,int port,int timeout){return esp_transport_connect(p,h,port,timeout)+1;}
int esp_transport_read(esp_transport_handle_t p,char *b,int n,int timeout)
{assert(((transport_t *)p)->tls&&n>0&&timeout>0&&timeout<=10000);++reads;int elapsed=timeout<9000?timeout:9000;now+=(int64_t)elapsed*1000;b[0]='x';return 1;}
int esp_transport_write(esp_transport_handle_t p,const char *b,int n,int timeout){(void)p;(void)b;assert(timeout>0);return n;}
int esp_transport_poll_read(esp_transport_handle_t p,int timeout){(void)p;assert(timeout>=0);return 1;}
int esp_transport_poll_write(esp_transport_handle_t p,int timeout){return esp_transport_poll_read(p,timeout);}
int esp_transport_close(esp_transport_handle_t p){(void)p;++closes;return 0;}
int main(void)
{
    transport_t *t=pet_deadline_transport_create(30000000);assert(t&&bundle);
    char byte;unsigned count=0;
    /* Models IDF's internal repeated transport-read loop: progress every9s
     * used to reset the timeout indefinitely. The wrapper stops at30s. */
    while(t->read(t,&byte,1,10000)>0)++count;
    assert(count==4&&reads==4&&now==30000000&&errno==ETIMEDOUT);
    assert(t->write(t,"x",1,10000)<0&&t->poll_read(t,10000)<0&&t->poll_write(t,10000)<0);
    assert(t->connect(t,"fixture.invalid",443,10000)<0);
    /* A kept connection's next request is re-armed with its own absolute
     * budget, and counts only its own response bytes. */
    assert(pet_deadline_transport_received(t)==4);
    pet_deadline_transport_arm(t,now+30000000);assert(!pet_deadline_transport_received(t));
    assert(t->poll_read(t,0)==1&&t->write(t,"x",1,10000)==1);
    for(count=0;t->read(t,&byte,1,10000)>0;)++count;
    assert(count==4&&reads==8&&now==60000000&&errno==ETIMEDOUT&&pet_deadline_transport_received(t)==4);
    t->close(t);esp_transport_destroy(t);assert(closes==1&&destroys==2);
    puts("TLS deadline: trickling internal reads consume one absolute budget, re-armed per kept request; cleanup is single-owner");
}
