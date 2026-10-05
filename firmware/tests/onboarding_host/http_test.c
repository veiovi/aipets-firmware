#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
/* What this build asked for: moves only with CONFIG_PET_DEVICE_RELINK. */
#ifdef CONFIG_PET_DEVICE_RELINK
#define RELINK_BUILD 1
#else
#define RELINK_BUILD 0
#endif
#include "../../main/pet_setup.c"
_Static_assert(CONFIG_PET_DEVICE_RELINK == RELINK_BUILD, "the setup worker moves only when the build asks");

static TickType_t ticks_now, ticks_delayed, run_until;
/* Waits of no time: a worker that spins on nothing fails here, not hangs. */
static unsigned long zero_waits;
static const char *response_body;
static size_t response_offset;
static int response_status = 200;
static bool response_complete = true;
static int64_t declared_length;
static char sent_body[1024], sent_auth[128], sent_id[64];
static char *retry_header;
static esp_http_client_config_t config;
static jmp_buf worker_exit;
static unsigned worker_reports, worker_stop_after, http_calls;
static pet_enrollment_result_t worker_last_status;
static bool simulate_submission, simulate_action;
static bool wifi_ready=true;
static esp_err_t clock_result = ESP_OK;
static const esp_app_desc_t app = {.version="host-fixture"};
/* Time follows the fake ticks (1 ms each), so the worker's schedule runs. */
int64_t esp_timer_get_time(void){return (int64_t)ticks_now*1000;}
esp_transport_handle_t pet_deadline_transport_create(int64_t deadline){assert(deadline==esp_timer_get_time()+30000000);return (void *)2;}
esp_err_t esp_transport_destroy(esp_transport_handle_t t){assert(t==(void *)2);return ESP_OK;}
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
esp_err_t esp_crt_bundle_attach(void *unused) { (void)unused; return ESP_OK; }
void esp_fill_random(void *bytes, size_t length) { memset(bytes, 7, length); }
QueueHandle_t xQueueCreate(unsigned count, unsigned size) { (void)count; (void)size; return (void *)1; }
int xQueueSend(QueueHandle_t q, const void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdTRUE; }
int xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{
    (void)q;
    if (simulate_submission || simulate_action) {
        /* A typed code, or the owner's action (an empty item), a second later. */
        strcpy(item, simulate_submission ? "A2B3" : "");
        if (simulate_action) atomic_store(&s_link_action, PET_SETUP_LINK_START);
        simulate_submission = simulate_action = false;
        ticks_now += 1000;
        return pdTRUE;
    }
    if (!t && ++zero_waits > 1000) longjmp(worker_exit, 9);
    if (run_until && (uint64_t)ticks_now + t >= run_until) { ticks_now = run_until; longjmp(worker_exit, 4); }
    ticks_now += t;
    return 0;
}
void vQueueDelete(QueueHandle_t q) { (void)q; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
void vTaskDelay(TickType_t ticks) { ticks_now += ticks; ticks_delayed += ticks; }
void vTaskDelete(void *task) { (void)task; }
TickType_t xTaskGetTickCount(void) { return ticks_now; }
bool pet_network_wifi_is_ready(void) { return wifi_ready; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *settings) { (void)settings; return ESP_OK; }
esp_err_t esp_netif_sntp_sync_wait(TickType_t timeout) { (void)timeout; return clock_result; }
static unsigned sntp_starts;
esp_err_t esp_netif_sntp_start(void) { ++sntp_starts; return ESP_OK; }
void esp_netif_sntp_deinit(void) {}
/* Nothing stored yet; a completion is written (and counted). */
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)name; (void)handle; return mode == NVS_READWRITE ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n)
{ (void)h; (void)key; (void)v; (void)n; return ESP_ERR_NVS_NOT_FOUND; }
static unsigned nvs_writes;
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n)
{ (void)h; (void)key; (void)v; (void)n; ++nvs_writes; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

/* A scripted cloud for the worker: each call must be the next path, and
 * gets its answer; one more call than scripted ends the worker. */
typedef struct { const char *path; int status; const char *body; char *retry; } scripted_t;
static const scripted_t *script;
static size_t script_count, script_next;
/* Or a cloud that answers by route, for long runs. */
static void (*cloud)(const char *path);
/* What each call sent: its path, whether it carried the credential, its body,
 * and when. */
static struct { char path[48]; bool authorized; char body[256]; TickType_t at; } sent[256];
static size_t sent_count;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *settings)
{
    config = *settings;
    ++http_calls;
    assert(config.disable_auto_redirect && config.crt_bundle_attach == esp_crt_bundle_attach);
    assert(config.transport==(void *)2);
    assert(config.method == HTTP_METHOD_POST);
    assert(!strncmp(config.url, "https://fixture.invalid/v1/device/", 34));
#if !CONFIG_PET_DEVICE_RELINK
    /* The default build never moves a device: no link route, ever. */
    assert(strncmp(config.url + 23, "/v1/device/link/", 16));
#endif
    if (cloud) cloud(config.url + 23);
    else if (script) {
        if (script_next == script_count) longjmp(worker_exit, 2);
        const scripted_t *answer = &script[script_next++];
        assert(!strcmp(config.url + 23, answer->path));
        response_body = answer->body;
        response_status = answer->status;
        retry_header = answer->retry;
    }
    response_offset = 0;
    sent_body[0] = sent_auth[0] = sent_id[0] = 0;
    return &config;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    (void)c;
    if (!strcmp(key, "Authorization")) strcpy(sent_auth, value);
    if (!strcmp(key, "X-Device-Id")) strcpy(sent_id, value);
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length) { (void)c; assert(length < 1024); return ESP_OK; }
int esp_http_client_write(esp_http_client_handle_t c, const char *bytes, int length)
{
    (void)c;
    int n = length < 7 ? length : 7; /* partial write regression */
    strncat(sent_body, bytes, (size_t)n);
    return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    (void)c;
    if (retry_header) {
        esp_http_client_event_t event = {.event_id=HTTP_EVENT_ON_HEADER,
            .header_key="rEtRy-AfTeR", .header_value=retry_header};
        config.event_handler(&event);
    }
    return declared_length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return response_status; }
int esp_http_client_read(esp_http_client_handle_t c, char *bytes, int capacity)
{
    (void)c;
    size_t remaining = strlen(response_body) - response_offset;
    size_t n = remaining < 13 ? remaining : 13;
    if (n > (size_t)capacity) n = (size_t)capacity;
    memcpy(bytes, response_body + response_offset, n);
    response_offset += n;
    return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{ (void)c; return response_complete && response_offset == strlen(response_body); }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    (void)c;
    if (sent_count < sizeof(sent) / sizeof(*sent)) {
        snprintf(sent[sent_count].path, sizeof(sent[0].path), "%s", config.url + 23);
        sent[sent_count].authorized = sent_auth[0] && sent_id[0];
        snprintf(sent[sent_count].body, sizeof(sent[0].body), "%s", sent_body);
        sent[sent_count].at = ticks_now;
        ++sent_count;
    }
    return ESP_OK;
}

static pet_enrollment_reply_t reply(bool redeem, const char *body, int status)
{
    response_body = body;
    response_status = status;
    return setup_request(NULL, redeem, "pet-fixture-device-id", "fixture-credential-not-a-real-secret",
                         redeem ? "A2B3" : NULL);
}

static unsigned waits_shown; /* PET_ENROLL_WAIT_CLOUD reports: "WAITING..." */
static void worker_status(pet_enrollment_result_t status)
{
    worker_last_status = status;
    waits_shown += status == PET_ENROLL_WAIT_CLOUD;
    if (++worker_reports == worker_stop_after) longjmp(worker_exit, 1);
}

static void worker_regressions(void)
{
    s_status = worker_status;
    worker_reports = 0;
    worker_stop_after = 1;
    s_enrollment.loaded=true;
    s_enrollment.io.request=setup_request;
    clock_result = ESP_ERR_TIMEOUT;
    unsigned before = http_calls;
    sntp_starts = 0;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(worker_last_status == PET_ENROLL_WAIT_CLOCK && http_calls == before);
    /* Waiting for the clock on a join asks SNTP at once, not after its backoff. */
    assert(sntp_starts == 1);
    /* Still unsynced, it asks again after 5, 10 and 20 s (at 5, 15 and 35 s):
     * lwIP alone waits 15 s for an answer and 15 s more before asking again,
     * 37 s after one lost packet on the hotspot (1 Oct). */
    worker_reports = 0;
    worker_stop_after = 36;
    ticks_now = 0;
    sntp_starts = 0;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(worker_last_status == PET_ENROLL_WAIT_CLOCK && http_calls == before && sntp_starts == 4);
    worker_stop_after = 1;
    wifi_ready=false;s_enrollment.complete=true;
    worker_reports=0;
    if(!setjmp(worker_exit))setup_task(NULL);
    assert(worker_last_status==PET_ENROLL_WAIT_WIFI&&http_calls==before&&!pet_setup_control_ready());
    s_enrollment.complete=false;wifi_ready=true;
    clock_result = ESP_OK;
    s_enrollment.loaded = true;
    s_enrollment.io.request = setup_request;
    worker_reports = 0;
    worker_stop_after = 2;
    ticks_now = ticks_delayed = 0;
    simulate_submission = true;
    retry_header = "30";
    response_body = "{}";
    response_status = 429;
    if (!setjmp(worker_exit)) setup_task(NULL);
    /* The code a second later is held for the cloud's 30 s, and the code
     * screen says WAITING; nothing sleeps. */
    assert(http_calls == before + 1);
    assert(!ticks_delayed && ticks_now == 1000);
    assert(worker_last_status == PET_ENROLL_WAIT_CLOUD);
}
/* The link calls: route, credential, body, and each answer checked against
 * the cloud's link contract. */
static char device[PET_ENROLLMENT_DEVICE_ID_BYTES], answer_body[512];
static pet_link_answer_t link_reply(pet_link_call_t call, bool moving, int status, const char *body)
{
    response_body = body;
    response_status = status;
    pet_link_answer_t answer;
    sent_count = 0;
    link_call(call, moving, &answer);
    assert(sent_count == 1 && !strcmp(sent[0].path, pet_link_paths[moving][call]));
    /* Only a device without an account offers its identity in the body; a
     * confirmation sends back the number its question showed. */
    const bool offer = call == PET_LINK_OFFER && !moving;
    char check[24] = "{}";
    if (call == PET_LINK_CONFIRM && s_link.check[0]) snprintf(check, sizeof(check), "{\"check\":\"%s\"}", s_link.check);
    assert(sent[0].authorized == !offer);
    assert(offer ? strstr(sent[0].body, device) && strstr(sent[0].body, "\"credential\"") &&
                   strstr(sent[0].body, "\"hardware\"") && !strstr(sent[0].body, "\"code\"")
                 : !strcmp(sent[0].body, check));
    return answer;
}
static const char *body(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(answer_body, sizeof(answer_body), format, arguments);
    va_end(arguments);
    return answer_body;
}
static void iso(char out[32], time_t at)
{
    strftime(out, 32, "%Y-%m-%dT%H:%M:%S.000Z", gmtime(&at));
}

static void link_transport(void)
{
    retry_header = NULL;
    declared_length = 0;
    s_quiet_until = 0;
    memset(&s_enrollment, 0, sizeof(s_enrollment));
    s_enrollment.loaded = true;
    for (unsigned i = 0; i < 16; ++i) s_enrollment.identity[i] = (uint8_t)(0xa0 + i);
    char credential[PET_ENROLLMENT_CREDENTIAL_BYTES];
    assert(pet_enrollment_identity(&s_enrollment, device, credential));
    char expires[32];
    iso(expires, time(NULL) + 600);
    pet_link_answer_t a = link_reply(PET_LINK_OFFER, false, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MPA\",\"expiresAt\":\"%s\","
        "\"linkUrl\":\"https://aipets.com/link\",\"pollSeconds\":5}", device, expires));
    assert(a.reply == PET_LINK_OFFERED && !strcmp(a.code, "K7QX9MPA") && a.poll_seconds == 5);
    assert(a.expires_in >= 595 && a.expires_in <= 600 && a.http == 200 && !a.retry_seconds);
    /* Claimed already, with and without the account's own name and number. */
    a = link_reply(PET_LINK_OFFER, false, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Alex\xe2\x80\x99s pets\",\"check\":\"472\"}", device));
    assert(a.reply == PET_LINK_CLAIMED_BY && !strcmp(a.account, "Alex\xe2\x80\x99s pets") && !strcmp(a.check, "472"));
    a = link_reply(PET_LINK_OFFER, false, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\"}", device));
    assert(a.reply == PET_LINK_CLAIMED_BY && !a.account[0] && !a.check[0]);
    /* A name of up to 80 UTF-16 units is up to 240 bytes: it is cut to the
     * whole characters that fit, never dropped. */
    char name[400] = "", expected[100] = "";
    for (int i = 0; i < 27; ++i) strcat(name, "\xe7\x8c\xab"); /* 81 bytes */
    for (int i = 0; i < 26; ++i) strcat(expected, "\xe7\x8c\xab");
    a = link_reply(PET_LINK_STATUS, false, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"%s\",\"check\":\"472\"}", device, name));
    assert(a.reply == PET_LINK_CLAIMED_BY && !strcmp(a.account, expected) && !strcmp(a.check, "472"));
    for (int i = 27; i < 80; ++i) strcat(name, "\xe7\x8c\xab"); /* 240 bytes: the most */
    a = link_reply(PET_LINK_STATUS, false, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"%s\",\"check\":\"472\"}", device, name));
    assert(a.reply == PET_LINK_CLAIMED_BY && !strcmp(a.account, expected));
    strcpy(name, "Abc");
    for (int i = 0; i < 30; ++i) strcat(name, "\xf0\x9f\x90\xb6"); /* four-byte characters */
    a = link_reply(PET_LINK_STATUS, false, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"%s\",\"check\":\"472\"}", device, name));
    assert(a.reply == PET_LINK_CLAIMED_BY && strlen(a.account) == 79 && !strncmp(a.account, name, 79));
    for (int i = 0; i < 60; ++i) strcat(name, "\xe7\x8c\xab"); /* over 240 bytes: not the contract */
    assert(link_reply(PET_LINK_STATUS, false, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"%s\",\"check\":\"472\"}", device,
        name)).reply == PET_LINK_NO_ANSWER);
    /* Not the contract: no answer. */
    static const char *const offers[] = {
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":7}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"check\":472}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"check\":\"47\"}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"check\":\"4721\"}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"check\":\"47a\"}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"k7qx9mpa\",\"expiresAt\":\"2099-01-01T00:00:00Z\",\"pollSeconds\":5}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MP0\",\"expiresAt\":\"2099-01-01T00:00:00Z\",\"pollSeconds\":5}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MP\",\"expiresAt\":\"2099-01-01T00:00:00Z\",\"pollSeconds\":5}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MPA\",\"expiresAt\":\"2099-01-01T00:00:00Z\",\"pollSeconds\":2}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MPA\",\"expiresAt\":\"2099-01-01T00:00:00Z\",\"pollSeconds\":5.5}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MPA\",\"expiresAt\":\"soon\",\"pollSeconds\":5}",
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"complete\"}",
        "{\"version\":2,\"deviceId\":\"%s\",\"status\":\"claimed\"}",
        "{\"version\":1,\"deviceId\":\"%s-other\",\"status\":\"claimed\"}",
    };
    for (size_t i = 0; i < sizeof(offers) / sizeof(*offers); ++i)
        assert(link_reply(PET_LINK_OFFER, false, 200, body(offers[i], device)).reply == PET_LINK_NO_ANSWER);
    /* Refusals carry the cloud's code and wait; a route this cloud does not
     * have is NOT_FOUND; a code that is not one is no answer. */
    a = link_reply(PET_LINK_OFFER, false, 409, "{\"version\":1,\"error\":{\"code\":\"SETUP_IDENTITY_TAKEN\",\"retryable\":false}}");
    assert(a.reply == PET_LINK_REFUSED && !strcmp(a.error, "SETUP_IDENTITY_TAKEN") && a.http == 409);
    retry_header = "30";
    a = link_reply(PET_LINK_OFFER, false, 503, "{\"version\":1,\"error\":{\"code\":\"LINK_OFFERS_DISABLED\",\"retryable\":true}}");
    assert(a.reply == PET_LINK_REFUSED && !strcmp(a.error, "LINK_OFFERS_DISABLED") && a.retry_seconds == 30);
    assert(!s_quiet_until); /* Link codes off pause only linking, never a typed code. */
    a = link_reply(PET_LINK_STATUS, false, 429, "{\"version\":1,\"error\":{\"code\":\"RATE_LIMITED\",\"retryable\":true}}");
    assert(a.reply == PET_LINK_REFUSED && a.retry_seconds == 30 && s_quiet_until == (uint64_t)ticks_now + 30000);
    s_quiet_until = 0;
    retry_header = NULL;
    a = link_reply(PET_LINK_OFFER, false, 404, "not found");
    assert(a.reply == PET_LINK_REFUSED && !strcmp(a.error, "NOT_FOUND"));
    assert(link_reply(PET_LINK_OFFER, false, 409, "{\"version\":1,\"error\":{\"code\":\"lower\",\"retryable\":false}}").reply ==
           PET_LINK_NO_ANSWER);
    /* Setup status: not claimed yet, claimed, or complete. */
    a = link_reply(PET_LINK_STATUS, false, 404, "{\"version\":1,\"error\":{\"code\":\"SETUP_NOT_CLAIMED\",\"retryable\":false}}");
    assert(a.reply == PET_LINK_WAITING);
    a = link_reply(PET_LINK_STATUS, false, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Alex\"}", device));
    assert(a.reply == PET_LINK_CLAIMED_BY && !strcmp(a.account, "Alex"));
    assert(link_reply(PET_LINK_STATUS, false, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"complete\"}", device)).reply ==
           PET_LINK_DONE);
    assert(link_reply(PET_LINK_STATUS, false, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\"}", device)).reply ==
           PET_LINK_NO_ANSWER);
    assert(link_reply(PET_LINK_DECLINE, false, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"declined\"}", device)).reply ==
           PET_LINK_DECLINED);
#if CONFIG_PET_DEVICE_RELINK
    /* Moving: the same answers on the link/ routes, with the credential. */
    a = link_reply(PET_LINK_OFFER, true, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"W2MN8QRT\",\"expiresAt\":\"%s\","
        "\"linkUrl\":\"https://aipets.com/link\",\"pollSeconds\":5}", device, expires));
    assert(a.reply == PET_LINK_OFFERED && !strcmp(a.code, "W2MN8QRT"));
    assert(link_reply(PET_LINK_STATUS, true, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\"}", device)).reply ==
           PET_LINK_WAITING);
    assert(link_reply(PET_LINK_STATUS, true, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"expired\"}", device)).reply ==
           PET_LINK_EXPIRED);
    a = link_reply(PET_LINK_STATUS, true, 200, body(
        "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Mia\",\"check\":\"093\"}", device));
    assert(a.reply == PET_LINK_CLAIMED_BY && !strcmp(a.account, "Mia") && !strcmp(a.check, "093"));
    strcpy(s_link.check, a.check); /* The question showed it: Move sends it back. */
    assert(link_reply(PET_LINK_CONFIRM, true, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"moved\"}", device)).reply ==
           PET_LINK_DONE);
    assert(link_reply(PET_LINK_CONFIRM, true, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"complete\"}", device)).reply ==
           PET_LINK_NO_ANSWER);
    a = link_reply(PET_LINK_CONFIRM, true, 409, "{\"version\":1,\"error\":{\"code\":\"LINK_NOT_CLAIMED\",\"retryable\":false}}");
    assert(a.reply == PET_LINK_REFUSED && !strcmp(a.error, "LINK_NOT_CLAIMED"));
    s_link.check[0] = 0;
    assert(link_reply(PET_LINK_CONFIRM, true, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"moved\"}", device)).reply ==
           PET_LINK_DONE);
    assert(link_reply(PET_LINK_DECLINE, true, 200, body("{\"version\":1,\"deviceId\":\"%s\",\"status\":\"declined\"}", device)).reply ==
           PET_LINK_DECLINED);
    a = link_reply(PET_LINK_STATUS, true, 401, "{\"version\":1,\"error\":{\"code\":\"DEVICE_AUTH_REQUIRED\",\"retryable\":false}}");
    assert(a.reply == PET_LINK_REFUSED && !strcmp(a.error, "DEVICE_AUTH_REQUIRED"));
#endif
}

/* What the worker told the screen about the link, and a finger on it. */
static pet_link_t link_seen;
static unsigned link_views;
static pet_setup_link_action_t tap_when_claimed = PET_SETUP_LINK_LEAVE;
static bool stop_when_linked;
static TickType_t asking_at;
static void link_view(const pet_link_t *link)
{
    link_seen = *link;
    ++link_views;
    if (link->phase == PET_LINK_ASKING && !asking_at) asking_at = ticks_now;
    if (link->phase == PET_LINK_CLAIMED && tap_when_claimed != PET_SETUP_LINK_LEAVE) {
        atomic_store(&s_link_action, (int)tap_when_claimed);
        tap_when_claimed = PET_SETUP_LINK_LEAVE;
    }
    if (link->phase == PET_LINK_LINKED && stop_when_linked) longjmp(worker_exit, 3);
}

static void worker_start(bool complete)
{
    memset(&s_enrollment, 0, sizeof(s_enrollment));
    s_enrollment.loaded = true;
    s_enrollment.complete = complete;
    s_enrollment.generation = 1;
    s_enrollment.io = (pet_enrollment_io_t){.read = read_record, .write = write_record, .random = random_bytes,
                                            .request = setup_request};
    for (unsigned i = 0; i < 16; ++i) s_enrollment.identity[i] = (uint8_t)(0xa0 + i);
    memset(&s_link, 0, sizeof(s_link));
    s_link_view = link_view;
    s_status = worker_status;
    s_next_probe = s_link_off_until = s_quiet_until = 0;
    /* A run ends by jumping out of a call: what that call held is reset here. */
    s_retry_seconds = 0;
    s_check_json[0] = 0;
    cloud = NULL;
    run_until = 0;
    zero_waits = 0;
    simulate_action = false;
    atomic_store(&s_link_action, -1);
    atomic_store(&s_complete, false);
    retry_header = NULL;
    simulate_submission = false;
    wifi_ready = true;
    clock_result = ESP_OK;
    worker_reports = 0;
    worker_stop_after = 100;
    script_next = 0;
    sent_count = 0;
    link_views = 0;
    waits_shown = 0;
    asking_at = 0;
    ticks_now = ticks_delayed = 0;
}

static void worker_link(void)
{
    char not_claimed[] = "{\"version\":1,\"error\":{\"code\":\"SETUP_NOT_CLAIMED\",\"retryable\":false}}";
    char off[] = "{\"version\":1,\"error\":{\"code\":\"LINK_OFFERS_DISABLED\",\"retryable\":true}}";
    char claimed[160], reclaimed[160], complete[160], offered[320], waiting[160], moved[160];
    char changed[] = "{\"version\":1,\"error\":{\"code\":\"SETUP_CLAIM_CHANGED\",\"retryable\":false}}";
    snprintf(claimed, sizeof(claimed),
             "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Alex\",\"check\":\"472\"}", device);
    snprintf(reclaimed, sizeof(reclaimed),
             "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Alex\",\"check\":\"815\"}", device);
    snprintf(complete, sizeof(complete), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"complete\"}", device);
    snprintf(waiting, sizeof(waiting), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\"}", device);
    snprintf(moved, sizeof(moved), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"moved\"}", device);
    char expires[32];
    iso(expires, time(NULL) + 600);
    snprintf(offered, sizeof(offered), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MPA\","
             "\"expiresAt\":\"%s\",\"linkUrl\":\"https://aipets.com/link\",\"pollSeconds\":5}", device, expires);

    /* Codes off (a cloud without link codes): {} every probe, as setup
     * always did, which finishes only a claim without a number. A claim of
     * the device's own code refuses it (SETUP_CLAIM_CHANGED) and waits for
     * its tap, and the device still takes a typed code. */
    const scripted_t codes_off[] = {
        {"/v1/device/setup/status", 503, off, "30"},
        {"/v1/device/setup/complete", 404, not_claimed, NULL},
        {"/v1/device/setup/complete", 409, changed, NULL},
        {"/v1/device/setup/complete", 404, not_claimed, NULL},
    };
    worker_start(false);
    script = codes_off;
    script_count = 4;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 4 && sent_count == 4 && !sent[1].at && sent[2].at == PROBE_MS && sent[3].at == 2 * PROBE_MS);
    for (size_t i = 1; i < 4; ++i) assert(sent[i].authorized && !strcmp(sent[i].body, "{}"));
    assert(worker_last_status == PET_ENROLL_NEEDS_CODE && !s_enrollment.complete);

    /* Codes on, nobody typed one: status alone, never setup/complete. Then
     * an owner types the code shown earlier: the question waits, polled, and
     * only a tap on Link completes and records it. */
    const scripted_t claim[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 200, claimed, NULL},
        {"/v1/device/setup/status", 200, claimed, NULL},
    };
    worker_start(false);
    script = claim;
    script_count = 3; /* No tap: the next call is status again, and the run ends. */
    tap_when_claimed = PET_SETUP_LINK_LEAVE;
    unsigned writes_before = nvs_writes;
    if (!setjmp(worker_exit)) setup_task(NULL);
    /* Ran out of script at the question: nothing completed. */
    assert(script_next == 3 && link_seen.phase == PET_LINK_CLAIMED && !strcmp(link_seen.account, "Alex"));
    assert(!s_enrollment.complete && nvs_writes == writes_before && !pet_setup_control_ready());
    const scripted_t tapped[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 200, claimed, NULL},
        {"/v1/device/setup/complete", 200, complete, NULL},
    };
    worker_start(false);
    script = tapped;
    script_count = 3;
    tap_when_claimed = PET_SETUP_LINK_YES;
    worker_stop_after = 3;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 3 && worker_last_status == PET_ENROLL_COMPLETE && s_enrollment.complete);
    assert(nvs_writes > writes_before && pet_setup_control_ready() && link_seen.phase == PET_LINK_LINKED);
    for (size_t i = 0; i < 3; ++i) assert(sent[i].authorized);
    assert(!strcmp(sent[2].body, "{\"check\":\"472\"}"));

    /* The claim changed while its question showed: Link finds another number,
     * status shows the new claim, and nothing completes without a new tap. */
    const scripted_t changed_claim[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 200, claimed, NULL},
        {"/v1/device/setup/complete", 409, changed, NULL},
        {"/v1/device/setup/status", 200, reclaimed, NULL},
    };
    atomic_store(&s_control_ready, false);
    worker_start(false);
    script = changed_claim;
    script_count = 4;
    tap_when_claimed = PET_SETUP_LINK_YES;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 4 && link_seen.phase == PET_LINK_CLAIMED && !strcmp(link_seen.check, "815"));
    assert(!strcmp(sent[2].body, "{\"check\":\"472\"}") && !s_enrollment.complete && !pet_setup_control_ready());

    /* The owner asks on the link screen: the code at once, then polls every
     * five seconds, which also stand in for the claim probe. */
    const scripted_t shown[] = {
        {"/v1/device/setup/offer", 200, offered, NULL},
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 404, not_claimed, NULL},
    };
    worker_start(false);
    script = shown;
    script_count = 3;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 3 && link_seen.phase == PET_LINK_SHOWING && !strcmp(link_seen.code, "K7QX9MPA"));
    assert(!strcmp(link_seen.device, device + 30) && !sent[0].authorized && sent[1].authorized && sent[2].authorized);
    /* Five seconds apart, and only link calls: the setup screen hears nothing new. */
    assert(ticks_now == 15000 && !worker_reports);

    /* Asked for a code while offline: the worker waits for Wi-Fi a second at
     * a time, never spinning on the link's schedule, and calls nothing. */
    worker_start(false);
    script = shown;
    script_count = 0;
    wifi_ready = false;
    worker_stop_after = 3;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(worker_last_status == PET_ENROLL_WAIT_WIFI && ticks_now == 2000 && !sent_count);
    assert(link_seen.phase == PET_LINK_ASKING);
    wifi_ready = true;

#if CONFIG_PET_DEVICE_RELINK
    /* A linked device moves only from the deck: offer, the question, Move. */
    const scripted_t move[] = {
        {"/v1/device/setup/complete", 200, complete, NULL},
        {"/v1/device/link/offer", 200, offered, NULL},
        {"/v1/device/link/status", 200, waiting, NULL},
        {"/v1/device/link/status", 200, claimed, NULL},
        {"/v1/device/link/confirm", 200, moved, NULL},
    };
    /* A moved device's enrollment stays valid: completion at boot answers
     * complete, and control starts. */
    worker_start(true);
    script = move;
    script_count = 5;
    tap_when_claimed = PET_SETUP_LINK_YES;
    stop_when_linked = true;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 5 && link_seen.phase == PET_LINK_LINKED && link_seen.moving && pet_setup_control_ready());
    for (size_t i = 1; i < 4; ++i) assert(sent[i].authorized && !strcmp(sent[i].body, "{}"));
    assert(sent[4].authorized && !strcmp(sent[4].body, "{\"check\":\"472\"}"));
    stop_when_linked = false;
#else
    /* The default build: a linked device asked to move (as the deck would,
     * had it the button) confirms its boot and then calls nothing for thirty
     * minutes. The fake client fails any link route outright. */
    const scripted_t stay[] = {{"/v1/device/setup/complete", 200, complete, NULL}};
    worker_start(true);
    script = stay;
    script_count = 1;
    run_until = 30 * 60000;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    simulate_action = true;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(sent_count == 1 && pet_setup_control_ready() && !pet_link_active(&s_link) && !s_link.moving);
#endif
    script = NULL;
}

/* A cloud for long runs, as the cloud now answers: an owner's claim of the
 * device's own code is pending, with a number, and setup/complete without
 * that number is refused (SETUP_CLAIM_CHANGED). This device's own redeem
 * makes a claim without a number, which setup/complete finishes. `codes_off`
 * is the switch; `lost_completes` completions of the redeem get no answer. */
static bool codes_off, redeemed;
static unsigned completes, refusals, redeems, offer_calls, status_calls, lost_completes;
static char cloud_offered[320], cloud_complete[160], cloud_confirm[160];
static const char cloud_off[] = "{\"version\":1,\"error\":{\"code\":\"LINK_OFFERS_DISABLED\",\"retryable\":true}}";
static const char cloud_not_claimed[] = "{\"version\":1,\"error\":{\"code\":\"SETUP_NOT_CLAIMED\",\"retryable\":false}}";
static const char cloud_changed[] = "{\"version\":1,\"error\":{\"code\":\"SETUP_CLAIM_CHANGED\",\"retryable\":false}}";
static bool switch_off_after_offer;
static void claimed_cloud(const char *path)
{
    retry_header = NULL;
    response_status = 200;
    if (!strcmp(path, "/v1/device/setup/complete") && redeemed && lost_completes) {
        --lost_completes;
        response_status = 502;
        response_body = "<html>bad gateway</html>";
    } else if (!strcmp(path, "/v1/device/setup/complete") && redeemed) {
        ++completes;
        response_body = cloud_complete;
    } else if (!strcmp(path, "/v1/device/setup/complete")) {
        /* Tests tap nothing here: every completion is {} (checked after). */
        ++refusals;
        response_status = 409;
        response_body = cloud_changed;
    } else if (!strcmp(path, "/v1/device/setup/redeem")) {
        ++redeems;
        redeemed = true;
        response_body = cloud_confirm;
    } else if (!strcmp(path, "/v1/device/setup/offer") && !codes_off) {
        ++offer_calls;
        response_body = cloud_offered;
        codes_off = switch_off_after_offer; /* The switch goes off with the code on screen. */
    } else if (!strcmp(path, "/v1/device/setup/status") && !codes_off) {
        ++status_calls;
        response_status = 404;
        response_body = cloud_not_claimed;
    } else {
        offer_calls += !strcmp(path, "/v1/device/setup/offer");
        status_calls += !strcmp(path, "/v1/device/setup/status");
        response_status = 503;
        response_body = cloud_off;
        retry_header = "30";
    }
}
static void run_cloud(bool off, TickType_t until)
{
    worker_start(false);
    cloud = claimed_cloud;
    codes_off = off;
    redeemed = switch_off_after_offer = false;
    completes = refusals = redeems = offer_calls = status_calls = lost_completes = 0;
    run_until = until;
    worker_stop_after = 100000;
}

/* The link's edge cases: nothing links without a tap, no
 * spinning, the cloud's waits, long names, and Move only with a number. */
static void review_regressions(void)
{
    char expires[32];
    iso(expires, time(NULL) + 600);
    snprintf(cloud_offered, sizeof(cloud_offered), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"offered\",\"code\":\"K7QX9MPA\","
             "\"expiresAt\":\"%s\",\"linkUrl\":\"https://aipets.com/link\",\"pollSeconds\":5}", device, expires);
    snprintf(cloud_complete, sizeof(cloud_complete), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"complete\"}", device);
    snprintf(cloud_confirm, sizeof(cloud_confirm), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"confirm-required\"}", device);

    /* A restart with the switch off and an owner's claim pending: status once
     * every ten minutes, {} every probe, which the cloud refuses, for thirty
     * minutes. Nothing completes, and the device waits for a typed code. */
    run_cloud(true, 30 * 60000);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(!completes && refusals == 60 && !redeems && status_calls == 3 && !s_enrollment.complete);
    assert(worker_last_status == PET_ENROLL_NEEDS_CODE);
    assert(sent_count < sizeof(sent) / sizeof(*sent)); /* every call is in the log */
    for (size_t i = 0; i < sent_count; ++i)
        assert(strcmp(sent[i].path, "/v1/device/setup/complete") || !strcmp(sent[i].body, "{}"));

    /* The code on screen, then the switch goes off with a claim pending. The
     * page stays open, then the owner leaves it: nothing completes. The code
     * runs out, and three offers without a claim stop linking. */
    run_cloud(false, 12 * 60000);
    switch_off_after_offer = true;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(!completes && offer_calls == 3 && link_seen.phase == PET_LINK_OFF && !strcmp(link_seen.error, "LINK_OFFERS_DISABLED"));
    run_until = 40 * 60000;
    atomic_store(&s_link_action, PET_SETUP_LINK_LEAVE);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(!completes && refusals && !s_enrollment.complete && zero_waits < 50);
    assert(sent_count < sizeof(sent) / sizeof(*sent)); /* every call is in the log */
    for (size_t i = 0; i < sent_count; ++i)
        assert(strcmp(sent[i].path, "/v1/device/setup/complete") || !strcmp(sent[i].body, "{}"));

    /* A typed code with codes on: status, then its own redeem, then
     * completion of that redeem. Never setup/complete before the redeem. */
    run_cloud(false, 120000);
    simulate_submission = true;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(s_enrollment.complete && redeems == 1 && completes == 1 && sent_count == 4);
    assert(!strcmp(sent[0].path, "/v1/device/setup/status") && !strcmp(sent[1].path, "/v1/device/setup/status"));
    assert(!strcmp(sent[2].path, "/v1/device/setup/redeem") && !strcmp(sent[3].path, "/v1/device/setup/complete"));
    assert(strstr(sent[2].body, "A2B3") && !strcmp(sent[3].body, "{}"));
    /* The same with codes off: the switch's answer and a refused {}, then
     * the redeem at once. */
    run_cloud(true, 120000);
    simulate_submission = true;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(s_enrollment.complete && sent_count == 4 && refusals == 1 && sent[2].at == 1000);
    assert(!strcmp(sent[1].path, "/v1/device/setup/complete") && !strcmp(sent[2].path, "/v1/device/setup/redeem"));
    assert(!strcmp(sent[3].path, "/v1/device/setup/complete") && completes == 1);

    /* More cases: with codes off, the device
     * finishes its own typed code by itself. The completion after its redeem
     * gets no answer: the next probe's {} completes it, 30 s later. */
    run_cloud(true, 30 * 60000);
    simulate_submission = true;
    lost_completes = 1;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(s_enrollment.complete && redeems == 1 && completes == 1 && sent_count == 5);
    assert(!strcmp(sent[2].path, "/v1/device/setup/redeem") && !strcmp(sent[4].path, "/v1/device/setup/complete"));
    assert(sent[4].at - sent[3].at == PROBE_MS && !strcmp(sent[4].body, "{}"));
    /* A restart between the redeem and its completion: the first probe's {}
     * completes it. */
    run_cloud(true, 30 * 60000);
    redeemed = true;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(s_enrollment.complete && completes == 1 && sent_count == 2 && !sent[1].at);
    assert(!strcmp(sent[1].path, "/v1/device/setup/complete") && !strcmp(sent[1].body, "{}"));

    char changed[] = "{\"version\":1,\"error\":{\"code\":\"SETUP_CLAIM_CHANGED\",\"retryable\":false}}";
    char limited[] = "{\"version\":1,\"error\":{\"code\":\"RATE_LIMITED\",\"retryable\":true}}";
    char route[] = "{\"version\":1,\"error\":{\"code\":\"ROUTE_NOT_FOUND\",\"retryable\":false}}";
    char not_claimed[] = "{\"version\":1,\"error\":{\"code\":\"SETUP_NOT_CLAIMED\",\"retryable\":false}}";
    char claimed[200], reclaimed[200], unnumbered[200], offered[320], complete[160], confirm[160];
    snprintf(claimed, sizeof(claimed), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Alex\",\"check\":\"472\"}", device);
    snprintf(reclaimed, sizeof(reclaimed), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Alex\",\"check\":\"815\"}", device);
    snprintf(unnumbered, sizeof(unnumbered), "{\"version\":1,\"deviceId\":\"%s\",\"status\":\"claimed\",\"accountName\":\"Mia\"}", device);
    strcpy(offered, cloud_offered);
    strcpy(complete, cloud_complete);
    strcpy(confirm, cloud_confirm);

    /* A changed claim, then a status the cloud rate-limits: no wait of no
     * time, and the next status exactly after the cloud's sixty seconds. */
    const scripted_t spin[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 200, claimed, NULL},
        {"/v1/device/setup/complete", 409, changed, NULL},
        {"/v1/device/setup/status", 429, limited, "60"},
        {"/v1/device/setup/status", 200, reclaimed, NULL},
    };
    worker_start(false);
    script = spin;
    script_count = 5;
    tap_when_claimed = PET_SETUP_LINK_YES;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 5 && sent[4].at == sent[3].at + 60000 && zero_waits < 5);
    assert(link_seen.phase == PET_LINK_CLAIMED && !strcmp(link_seen.check, "815") && !s_enrollment.complete);
    /* The same with a lost status answer, which asks no wait of the cloud:
     * status again after five seconds, still without a wait of no time. */
    const scripted_t lost[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 200, claimed, NULL},
        {"/v1/device/setup/complete", 409, changed, NULL},
        {"/v1/device/setup/status", 502, "<html>bad gateway</html>", NULL},
        {"/v1/device/setup/status", 200, reclaimed, NULL},
    };
    worker_start(false);
    script = lost;
    script_count = 5;
    tap_when_claimed = PET_SETUP_LINK_YES;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 5 && sent[4].at == sent[3].at + 5000 && zero_waits < 5 && !strcmp(link_seen.check, "815"));

    /* A 429 on Link: the same tap again after the cloud's sixty seconds,
     * with the same number, and nothing else meanwhile. */
    const scripted_t limited_link[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 200, claimed, NULL},
        {"/v1/device/setup/complete", 429, limited, "60"},
        {"/v1/device/setup/complete", 200, complete, NULL},
    };
    worker_start(false);
    script = limited_link;
    script_count = 4;
    tap_when_claimed = PET_SETUP_LINK_YES;
    stop_when_linked = true;
    if (!setjmp(worker_exit)) setup_task(NULL);
    stop_when_linked = false;
    assert(script_next == 4 && sent[3].at == sent[2].at + 60000 && s_enrollment.complete);
    assert(!strcmp(sent[2].body, "{\"check\":\"472\"}") && !strcmp(sent[3].body, "{\"check\":\"472\"}"));

    /* A JSON 404 from status (not SETUP_NOT_CLAIMED) means codes off: {} as
     * setup always did, and the typed code is redeemed directly. */
    const scripted_t routed[] = {
        {"/v1/device/setup/status", 404, route, NULL},
        {"/v1/device/setup/complete", 404, not_claimed, NULL},
        {"/v1/device/setup/redeem", 200, confirm, NULL},
        {"/v1/device/setup/complete", 200, complete, NULL},
    };
    worker_start(false);
    script = routed;
    script_count = 4;
    simulate_submission = true;
    run_until = 60000;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 4 && s_enrollment.complete && sent[2].at == 1000);

    /* The owner's action during the cloud's wait shows at once; the call
     * waits for the end of the wait. */
    const scripted_t waiting_action[] = {
        {"/v1/device/setup/status", 429, limited, "60"},
        {"/v1/device/setup/offer", 200, offered, NULL},
    };
    worker_start(false);
    script = waiting_action;
    script_count = 2;
    simulate_action = true;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 2 && asking_at == 1000 && sent[1].at == 60000);

    /* A typed code whose status the cloud asks to wait: the code is held,
     * the code screen says WAITING, and it goes when the wait ends. */
    const scripted_t held_code[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 429, limited, "60"},
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/redeem", 200, confirm, NULL},
        {"/v1/device/setup/complete", 200, complete, NULL},
    };
    worker_start(false);
    script = held_code;
    script_count = 5;
    simulate_submission = true;
    run_until = 120000;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 5 && s_enrollment.complete && waits_shown == 1);
    assert(sent[1].at == 1000 && sent[2].at == 61000 && strstr(sent[3].body, "A2B3"));
    /* The same with no answer: the typed code tries again five seconds later. */
    const scripted_t unanswered_code[] = {
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/status", 502, "<html>bad gateway</html>", NULL},
        {"/v1/device/setup/status", 404, not_claimed, NULL},
        {"/v1/device/setup/redeem", 200, confirm, NULL},
        {"/v1/device/setup/complete", 200, complete, NULL},
    };
    worker_start(false);
    script = unanswered_code;
    script_count = 5;
    simulate_submission = true;
    run_until = 120000;
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 5 && s_enrollment.complete && !waits_shown);
    assert(sent[1].at == 1000 && sent[2].at == 1000 + HELD_MS && strstr(sent[3].body, "A2B3"));

#if CONFIG_PET_DEVICE_RELINK
    char unavailable[] = "{\"version\":1,\"error\":{\"code\":\"CONTROL_UNAVAILABLE\",\"retryable\":true}}";
    /* A linked device whose boot completion the cloud asks to wait for, with
     * link mode opened early: no spin, the second completion after the
     * cloud's 45 seconds, and link calls only once setup is confirmed. */
    const scripted_t early[] = {
        {"/v1/device/setup/complete", 503, unavailable, "45"},
        {"/v1/device/setup/complete", 200, complete, NULL},
        {"/v1/device/link/offer", 200, offered, NULL},
    };
    worker_start(true);
    script = early;
    script_count = 3;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 3 && sent[1].at == 45000 && sent[2].at == 45000 && zero_waits < 5);
    /* The same with a refusal (the moved-device answer before the cloud's
     * fix): the next completion one probe later, never at once. */
    const scripted_t refused_boot[] = {
        {"/v1/device/setup/complete", 404, not_claimed, NULL},
        {"/v1/device/setup/complete", 200, complete, NULL},
    };
    worker_start(true);
    script = refused_boot;
    script_count = 2;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(sent_count >= 2 && sent[1].at == PROBE_MS && zero_waits < 5);
#endif

#if CONFIG_PET_DEVICE_RELINK
    /* A move claimed without a number: no Move, so never link/confirm. */
    const scripted_t unnumbered_move[] = {
        {"/v1/device/setup/complete", 200, complete, NULL},
        {"/v1/device/link/offer", 200, offered, NULL},
        {"/v1/device/link/status", 200, unnumbered, NULL},
        {"/v1/device/link/status", 200, unnumbered, NULL},
    };
    worker_start(true);
    script = unnumbered_move;
    script_count = 4;
    tap_when_claimed = PET_SETUP_LINK_YES;
    atomic_store(&s_link_action, PET_SETUP_LINK_START);
    if (!setjmp(worker_exit)) setup_task(NULL);
    assert(script_next == 4 && link_seen.phase == PET_LINK_CLAIMED && link_seen.moving && !link_seen.check[0]);
    for (size_t i = 0; i < sent_count; ++i) assert(strcmp(sent[i].path, "/v1/device/link/confirm"));
#endif
    script = NULL;
}

int main(void)
{
    strcpy(s_origin, "https://fixture.invalid");
    assert(reply(false, "{\"version\":1,\"deviceId\":\"pet-fixture-device-id\",\"status\":\"complete\"}", 200) == PET_ENROLL_REPLY_COMPLETE);
    assert(!strcmp(sent_body, "{}") && sent_auth[0] && !strcmp(sent_id, "pet-fixture-device-id"));
    assert(reply(true, "{\"version\":1,\"deviceId\":\"pet-fixture-device-id\",\"status\":\"confirm-required\"}", 200) == PET_ENROLL_REPLY_CONFIRM_REQUIRED);
    assert(strstr(sent_body, "A2B3") && !sent_auth[0]);
    assert(reply(false, "{\"version\":1,\"error\":{\"code\":\"SETUP_NOT_CLAIMED\",\"retryable\":false}}", 401) == PET_ENROLL_REPLY_NOT_CLAIMED);
    assert(reply(true, "{\"version\":1,\"error\":{\"code\":\"SETUP_NOT_CLAIMED\",\"retryable\":false}}", 401) == PET_ENROLL_REPLY_REJECTED);
    assert(reply(false, "{\"version\":1,\"deviceId\":\"pet-wrong-device\",\"status\":\"complete\"}", 200) == PET_ENROLL_REPLY_RETRY);
    assert(reply(false, "{\"version\":2,\"deviceId\":\"pet-fixture-device-id\",\"status\":\"complete\"}", 200) == PET_ENROLL_REPLY_RETRY);
    assert(reply(false, "{\"version\":1} trailing", 200) == PET_ENROLL_REPLY_RETRY);
    assert(reply(false, "{\"version\":1,\"version\":1,\"deviceId\":\"pet-fixture-device-id\",\"status\":\"complete\"}", 200) == PET_ENROLL_REPLY_RETRY);
    assert(!bounded_json("[[[[[[[[[0]]]]]]]]]", 19));
    assert(reply(false, "{\"version\":1}", 302) == PET_ENROLL_REPLY_RETRY);
    response_complete = false;
    assert(reply(false, "{\"version\":1,\"deviceId\":\"pet-fixture-device-id\",\"status\":\"complete\"}", 200) == PET_ENROLL_REPLY_RETRY);
    response_complete = true;
    declared_length = SETUP_RESPONSE_MAX + 1;
    assert(reply(false, "{}", 200) == PET_ENROLL_REPLY_RETRY);
    declared_length = 0; /* chunked is valid */
    retry_header = "30";
    assert(reply(false, "{}", 429) == PET_ENROLL_REPLY_RETRY);
    assert(s_retry_seconds == 30 && s_quiet_until == 30000); /* No call for 30 s. */
    retry_header = "90";
    assert(reply(false, "{}", 503) == PET_ENROLL_REPLY_RETRY && s_retry_seconds == 90 && s_quiet_until == 90000);
    retry_header = NULL;
    assert(reply(false, "{}", 503) == PET_ENROLL_REPLY_RETRY && !s_retry_seconds && s_quiet_until == 90000);
    s_quiet_until = 0;
    worker_regressions();
    link_transport();
    worker_link();
    review_regressions();
    puts(CONFIG_PET_DEVICE_RELINK ? "setup HTTP (relink on): auth scope, bounded parsing, partial I/O, redirects, response backoff, link codes and moves passed"
         : "setup HTTP (default, no moves): auth scope, bounded parsing, partial I/O, redirects, response backoff and link codes passed");
    return 0;
}
