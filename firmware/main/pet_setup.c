#include "pet_setup.h"
#include "pet_board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <time.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pet_network.h"
#include "esp_timer.h"
#include "pet_deadline_transport.h"

#ifndef CONFIG_PET_DEVICE_RELINK
#define CONFIG_PET_DEVICE_RELINK 0 /* Kconfig leaves a disabled option undefined. */
#endif
#define SETUP_RESPONSE_MAX 4096
#define SETUP_ORIGIN_MAX 128
/* Without an account, how often the worker asks whether it was claimed. */
#define PROBE_MS 30000u
/* A typed code whose status call got no answer tries again this soon. */
#define HELD_MS 5000u
/* While the clock is unsynced on Wi-Fi, SNTP is asked at once, then again
 * after 5, 10 and 20 s and every 30 s. Left alone, lwIP waits 15 s for an
 * answer and 15 s more before asking again, so one lost packet on a phone
 * hotspot held every request for 37 s (1 Oct). */
#define CLOCK_ASK_FIRST_MS 5000u
#define CLOCK_ASK_MAX_MS 30000u
/* The cloud has link codes off: the typed code and {}, until status is asked again. */
#define LINK_OFF_MS 600000u

static QueueHandle_t s_codes;
static pet_setup_status_callback_t s_status;
static pet_setup_link_callback_t s_link_view;
static char s_origin[SETUP_ORIGIN_MAX];
static pet_enrollment_t s_enrollment;
static pet_link_t s_link;
static atomic_int s_link_action = -1;
static uint64_t s_next_probe, s_link_off_until;
/* No setup or link call before this: the cloud's last "wait" (429 or 503). */
static uint64_t s_quiet_until;
/* Completion's body while Link is tapped: the check number shown. And the
 * last setup refusal's code, which tells a changed claim from the rest. */
static char s_check_json[24], s_setup_error[33];
/* The last answer's Retry-After in seconds, 0 without one. */
static unsigned s_retry_seconds;
static atomic_bool s_accepting;
static atomic_bool s_complete;
static atomic_bool s_control_ready;
static char s_completed_id[PET_ENROLLMENT_DEVICE_ID_BYTES];
static char s_completed_credential[PET_ENROLLMENT_CREDENTIAL_BYTES];

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        event->header_value && !strcasecmp(event->header_key, "Retry-After")) {
        char *end = NULL;
        unsigned long seconds = strtoul(event->header_value, &end, 10);
        if (end != event->header_value && !*end && seconds <= 3600)
            s_retry_seconds = seconds < 5 ? 5 : (unsigned)seconds;
    }
    return ESP_OK;
}

static int read_record(void *context, unsigned slot, uint8_t *record)
{
    (void)context;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("pet_enroll", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (err != ESP_OK) return -1;
    size_t size = PET_ENROLLMENT_RECORD_BYTES;
    err = nvs_get_blob(nvs, slot ? "record_b" : "record_a", record, &size);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;
    return err == ESP_OK && size == PET_ENROLLMENT_RECORD_BYTES ? 1 : -1;
}

static bool write_record(void *context, unsigned slot, const uint8_t *record)
{
    (void)context;
    nvs_handle_t nvs;
    if (nvs_open("pet_enroll", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs, slot ? "record_b" : "record_a",
                                record, PET_ENROLLMENT_RECORD_BYTES);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool random_bytes(void *context, uint8_t *bytes, size_t length)
{
    (void)context;
    /* Called only after Wi-Fi has started (RF entropy source is enabled). */
    esp_fill_random(bytes, length);
    return true;
}

static bool string_equals(const cJSON *object, const char *key, const char *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && !strcmp(item->valuestring, value);
}

static bool bounded_json(const char *text, size_t bytes)
{
    /* Cap parser recursion before calling cJSON on the worker's small stack. */
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < bytes; ++i) {
        char c = text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 8) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    return !quoted && !depth;
}

static bool exact_keys(const cJSON *object, const char *a, const char *b, const char *c)
{
    unsigned seen = 0;
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *item = object->child; item; item = item->next) {
        unsigned bit = !strcmp(item->string, a) ? 1 : !strcmp(item->string, b) ? 2 :
                       c && !strcmp(item->string, c) ? 4 : 0;
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    return seen == (c ? 7u : 3u);
}

/* Clears every string an answer or request held: credentials, link codes. */
static void forget(cJSON *item)
{
    for (; item; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring)
            pet_enrollment_clear(item->valuestring, strlen(item->valuestring));
        forget(item->child);
    }
}

static void release(char *json)
{
    if (json) { pet_enrollment_clear(json, strlen(json)); free(json); }
}

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

/* The cloud asked the device to wait: nothing is sent until then. */
static void hold_off(unsigned seconds)
{
    const uint64_t until = now_ms() + (uint64_t)(seconds ? seconds : 30) * 1000u;
    if (until > s_quiet_until) s_quiet_until = until;
}

/* The identity a device without an account presents: to redeem a typed code,
 * or, without one, to offer its own link code. */
static char *identity_json(const char *device_id, const char *credential, const char *code)
{
    cJSON *body = cJSON_CreateObject();
    char *json = NULL;
    if (body && (!code || cJSON_AddStringToObject(body, "code", code)) &&
        cJSON_AddStringToObject(body, "deviceId", device_id) &&
        cJSON_AddStringToObject(body, "credential", credential) &&
        cJSON_AddStringToObject(body, "hardware", pet_board_current()->hardware) &&
        cJSON_AddStringToObject(body, "firmware", esp_app_get_description()->version))
        json = cJSON_PrintUnformatted(body);
    /* cJSON owns copies of request secrets as well as the serialized body. */
    if (body) forget(body->child);
    cJSON_Delete(body);
    return json;
}

/* One POST to the pinned origin, with X-Device-Id and the bearer credential
 * when a credential is given. Returns the answer's single JSON object of
 * version 1, or NULL; *http is its status, 0 without one. A Retry-After lands
 * in s_retry_seconds. */
static cJSON *post(const char *path, const char *json, const char *device_id,
                   const char *credential, int *http)
{
    *http = 0;
    s_retry_seconds = 0;
    char url[192];
    snprintf(url, sizeof(url), "%s%s", s_origin, path);
    char *response = calloc(1, SETUP_RESPONSE_MAX + 1);
    cJSON *parsed = NULL;
    esp_http_client_handle_t client = NULL;
    esp_transport_handle_t transport=NULL;
    if (!response) goto done;
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = http_event,
    };
#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    transport=pet_deadline_transport_create(esp_timer_get_time()+30000000);
    if(!transport)goto done;
    config.transport=transport;
#else
    goto done; /* Never send enrollment credentials on an unbounded fallback. */
#endif
    client = esp_http_client_init(&config);
    if (!client) goto done;
    if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK)
        goto done;
    if (credential) {
        char authorization[PET_ENROLLMENT_CREDENTIAL_BYTES + 8];
        snprintf(authorization, sizeof(authorization), "Bearer %s", credential);
        esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization);
        pet_enrollment_clear(authorization, sizeof(authorization));
        if (err != ESP_OK ||
            esp_http_client_set_header(client, "X-Device-Id", device_id) != ESP_OK) goto done;
    }
    size_t length = strlen(json);
    if (esp_http_client_open(client, (int)length) != ESP_OK) goto done;
    size_t sent = 0;
    while (sent < length) {
        int n = esp_http_client_write(client, json + sent, (int)(length - sent));
        if (n <= 0) goto done;
        sent += (size_t)n;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0 || content_length > SETUP_RESPONSE_MAX) goto done;
    *http = esp_http_client_get_status_code(client);
    size_t received = 0;
    while (received < SETUP_RESPONSE_MAX) {
        int n = esp_http_client_read(client, response + received,
                                     (int)(SETUP_RESPONSE_MAX - received));
        if (n < 0) goto done;
        if (!n) break;
        received += (size_t)n;
    }
    if (!esp_http_client_is_complete_data_received(client) ||
        memchr(response, 0, received) || !bounded_json(response, received)) goto done;
    /* Require the full single JSON value, not a valid prefix with trailing data. */
    parsed = cJSON_ParseWithLengthOpts(response, received + 1, NULL, true);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(parsed, "version");
    if (!cJSON_IsObject(parsed) || !cJSON_IsNumber(version) || version->valuedouble != 1) {
        forget(parsed);
        cJSON_Delete(parsed);
        parsed = NULL;
    }
done:
    if (client) esp_http_client_cleanup(client);
    if(transport)esp_transport_destroy(transport);
    if (response) { pet_enrollment_clear(response, SETUP_RESPONSE_MAX + 1); free(response); }
    return parsed;
}

static pet_enrollment_reply_t setup_request(void *context, bool redeem,
                                            const char *device_id,
                                            const char *credential, const char *code)
{
    (void)context;
    pet_enrollment_reply_t result = PET_ENROLL_REPLY_RETRY;
    char *json = redeem ? identity_json(device_id, credential, code) : NULL;
    if (redeem && !json) return result;
    int status;
    cJSON *parsed = redeem ? post("/v1/device/setup/redeem", json, NULL, NULL, &status) :
        post("/v1/device/setup/complete", s_check_json[0] ? s_check_json : "{}", device_id, credential, &status);
    release(json);
    s_setup_error[0] = 0;
    if (status == 429 || status == 503) hold_off(s_retry_seconds);
    else if (status == 200 && exact_keys(parsed, "version", "deviceId", "status") &&
        string_equals(parsed, "deviceId", device_id)) {
        if (string_equals(parsed, "status", "complete")) result = PET_ENROLL_REPLY_COMPLETE;
        else if (redeem && string_equals(parsed, "status", "confirm-required"))
            result = PET_ENROLL_REPLY_CONFIRM_REQUIRED;
    } else if (status >= 400 && status < 500 && status != 408) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(parsed, "error");
        const cJSON *code_item = cJSON_GetObjectItemCaseSensitive(error, "code");
        if (exact_keys(parsed, "version", "error", NULL) &&
            exact_keys(error, "code", "retryable", NULL) &&
            cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(error, "retryable"))) {
            if (!redeem && string_equals(error, "code", "SETUP_NOT_CLAIMED"))
                result = PET_ENROLL_REPLY_NOT_CLAIMED;
            else result = PET_ENROLL_REPLY_REJECTED;
            if (cJSON_IsString(code_item) && strlen(code_item->valuestring) < sizeof(s_setup_error))
                strcpy(s_setup_error, code_item->valuestring);
        }
    }
    forget(parsed);
    cJSON_Delete(parsed);
    return result;
}

static bool text(const cJSON *object, const char *key, size_t max, const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring[0] || strlen(item->valuestring) > max) return false;
    *out = item->valuestring;
    return true;
}

/* The link answers the cloud's contract names: version 1, this
 * device and each answer's fields. Other keys are ignored; anything else is no
 * answer. A route this cloud does not have is NOT_FOUND. */
static void link_parse(const cJSON *root, pet_link_call_t call, bool moving,
                       const char *device_id, pet_link_answer_t *out)
{
    const char *state, *value, *expires_at;
    if (!root) {
        if (out->http == 404) { out->reply = PET_LINK_REFUSED; strcpy(out->error, "NOT_FOUND"); }
        return;
    }
    if (out->http >= 400) {
        if (!text(cJSON_GetObjectItemCaseSensitive(root, "error"), "code", sizeof(out->error) - 1, &value) ||
            strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != strlen(value)) return;
        strcpy(out->error, value);
        /* A setup code that nobody has typed yet. */
        out->reply = !moving && call == PET_LINK_STATUS && !strcmp(value, "SETUP_NOT_CLAIMED") ?
            PET_LINK_WAITING : PET_LINK_REFUSED;
        return;
    }
    if (out->http != 200 || !string_equals(root, "deviceId", device_id) || !text(root, "status", 16, &state)) return;
    if (!strcmp(state, "offered") && call == PET_LINK_STATUS && moving) out->reply = PET_LINK_WAITING;
    else if (!strcmp(state, "offered") && call == PET_LINK_OFFER) {
        const cJSON *poll = cJSON_GetObjectItemCaseSensitive(root, "pollSeconds");
        int64_t expires;
        if (!text(root, "code", PET_LINK_CODE_LENGTH, &value) || strlen(value) != PET_LINK_CODE_LENGTH ||
            strspn(value, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789") != PET_LINK_CODE_LENGTH || !cJSON_IsNumber(poll) ||
            poll->valuedouble < 3 || poll->valuedouble > 60 || poll->valuedouble != (int)poll->valuedouble ||
            !text(root, "expiresAt", 40, &expires_at) || !pet_link_utc(expires_at, &expires)) return;
        /* The clock was set before any setup call. */
        const int64_t now = (int64_t)time(NULL), left = expires - now;
        strcpy(out->code, value);
        out->poll_seconds = (unsigned)poll->valuedouble;
        out->expires_in = left <= 0 ? 0 : left > 3600 ? 3600 : (unsigned)left;
        out->reply = PET_LINK_OFFERED;
    } else if (!strcmp(state, "claimed") && (call == PET_LINK_OFFER || call == PET_LINK_STATUS)) {
        /* The claiming account's name and the claim's check number, when the
         * cloud gives them. A name is up to 80 UTF-16 units, so up to 240
         * bytes: it is cut to the whole characters that fit. */
        if (text(root, "accountName", 240, &value)) {
            size_t n = strlen(value);
            if (n >= sizeof(out->account))
                for (n = sizeof(out->account) - 1; n && ((unsigned char)value[n] & 0xc0) == 0x80;) --n;
            memcpy(out->account, value, n);
        } else if (cJSON_GetObjectItemCaseSensitive(root, "accountName")) return;
        if (text(root, "check", PET_LINK_CHECK_LENGTH, &value) && strlen(value) == PET_LINK_CHECK_LENGTH &&
            strspn(value, "0123456789") == PET_LINK_CHECK_LENGTH) strcpy(out->check, value);
        else if (cJSON_GetObjectItemCaseSensitive(root, "check")) return;
        out->reply = PET_LINK_CLAIMED_BY;
    } else if (!strcmp(state, moving ? "moved" : "complete") &&
               (call == PET_LINK_CONFIRM || (!moving && call == PET_LINK_STATUS))) out->reply = PET_LINK_DONE;
    else if (!strcmp(state, "expired") && moving && call == PET_LINK_STATUS) out->reply = PET_LINK_EXPIRED;
    else if (!strcmp(state, "declined") && call == PET_LINK_DECLINE) out->reply = PET_LINK_DECLINED;
}

/* {"check":"472"}: the number the question showed, sent back with Link. */
static void check_json(void)
{
    s_check_json[0] = 0;
    if (strlen(s_link.check) == PET_LINK_CHECK_LENGTH)
        snprintf(s_check_json, sizeof(s_check_json), "{\"check\":\"%s\"}", s_link.check);
}

/* One link call. A device without an account offers its identity as it
 * redeems a code; every other call carries its credential. */
static void link_call(pet_link_call_t call, bool moving, pet_link_answer_t *out)
{
    memset(out, 0, sizeof(*out));
    char id[PET_ENROLLMENT_DEVICE_ID_BYTES], credential[PET_ENROLLMENT_CREDENTIAL_BYTES];
    if (!pet_enrollment_identity(&s_enrollment, id, credential)) return;
    const bool offer = call == PET_LINK_OFFER && !moving;
    char *json = offer ? identity_json(id, credential, NULL) : NULL;
    if (call == PET_LINK_CONFIRM) check_json();
    cJSON *root = offer && !json ? NULL :
        post(pet_link_paths[moving][call], offer ? json : call == PET_LINK_CONFIRM && s_check_json[0] ? s_check_json : "{}",
             id, offer ? NULL : credential, &out->http);
    s_check_json[0] = 0;
    pet_enrollment_clear(credential, sizeof(credential));
    release(json);
    out->retry_seconds = s_retry_seconds;
    link_parse(root, call, moving, id, out);
    forget(root);
    cJSON_Delete(root);
    /* Link codes off pause only linking; any other "wait" pauses every call. */
    if ((out->http == 429 || out->http == 503) && strcmp(out->error, "LINK_OFFERS_DISABLED") &&
        strcmp(out->error, "RELINK_DISABLED")) hold_off(out->retry_seconds);
}

static void link_show(void)
{
    if (s_link_view) s_link_view(&s_link);
}

/* The call due now. A code past its time, or the last one, shows at once. */
static pet_link_call_t link_next(void)
{
    const pet_link_phase_t before = s_link.phase;
    const pet_link_call_t call = pet_link_next(&s_link, now_ms());
    if (s_link.phase != before) link_show();
    return call;
}

static void link_answer(pet_link_call_t call, pet_link_answer_t *answer)
{
    pet_link_answer(&s_link, call, answer, now_ms());
    pet_enrollment_clear(answer, sizeof(*answer));
    link_show();
}

/* The owner on the link screen: asks for a code, taps an answer, or leaves. */
static void link_actions(bool moving)
{
    if (!s_enrollment.loaded) return;
    int action = atomic_exchange(&s_link_action, -1);
    if (action < 0) return;
    if (action == PET_SETUP_LINK_START) {
#if !CONFIG_PET_DEVICE_RELINK
        if (moving) return; /* Moving waits for the relink contract (Kconfig). */
#endif
        char id[PET_ENROLLMENT_DEVICE_ID_BYTES], credential[PET_ENROLLMENT_CREDENTIAL_BYTES];
        if (pet_enrollment_identity(&s_enrollment, id, credential)) memcpy(s_link.device, id + 30, 7);
        pet_enrollment_clear(credential, sizeof(credential));
        pet_link_start(&s_link, moving, now_ms());
        s_link_off_until = 0; /* The owner asks now: so does the worker. */
    } else if (action == PET_SETUP_LINK_LEAVE) pet_link_leave(&s_link, now_ms());
    else pet_link_tap(&s_link, action == PET_SETUP_LINK_YES, now_ms());
    link_show();
}

/* A device without an account. While the cloud gives
 * link codes it asks setup/status, and only a tap on Link, with the number
 * shown, completes a claim of its code. A typed code is redeemed directly
 * once status shows no claim, and while link codes are off. While they are
 * off, the worker also completes with {} every probe, as setup always did:
 * that finishes what its own typed code started, after a lost answer or a
 * restart. The cloud refuses {} for a claim with a number
 * (SETUP_CLAIM_CHANGED), so a claim of the device's own code still waits for
 * its tap. `quiet`: only link calls this pass. `held`: the typed code waits
 * for the next try. */
static pet_enrollment_result_t unlinked(const char *code, bool *quiet, bool *held)
{
    const uint64_t now = now_ms();
    pet_link_answer_t answer;
    pet_link_call_t call = link_next();
    if (call == PET_LINK_CONFIRM) {
        /* Link, with the number shown, or setup the cloud finished already:
         * completion persists exactly as after a typed code. A changed claim,
         * or none, goes back to the machine. */
        check_json();
        pet_enrollment_result_t result = pet_enrollment_resume(&s_enrollment);
        s_check_json[0] = 0;
        memset(&answer, 0, sizeof(answer));
        answer.reply = result == PET_ENROLL_COMPLETE ? PET_LINK_DONE : result == PET_ENROLL_NEEDS_CODE ||
            result == PET_ENROLL_REJECTED ? PET_LINK_REFUSED : PET_LINK_NO_ANSWER;
        answer.retry_seconds = s_retry_seconds;
        strcpy(answer.error, result == PET_ENROLL_NEEDS_CODE ? "SETUP_NOT_CLAIMED" : s_setup_error);
        link_answer(call, &answer);
        return result;
    }
    if (call != PET_LINK_NONE && !code[0]) {
        link_call(call, false, &answer);
        link_answer(call, &answer);
        if (call == PET_LINK_STATUS) s_next_probe = now + PROBE_MS;
        *quiet = true;
        return PET_ENROLL_NEEDS_CODE;
    }
    if (!code[0] && now + 1000 < s_next_probe) {
        *quiet = true;
        return PET_ENROLL_NEEDS_CODE;
    }
    s_next_probe = now + PROBE_MS;
    if (now >= s_link_off_until) {
        link_call(PET_LINK_STATUS, false, &answer);
        if (answer.reply == PET_LINK_CLAIMED_BY || answer.reply == PET_LINK_DONE) {
            /* Claimed: the question comes first, and a typed code cannot link
             * meanwhile. Complete already: the next pass records it. */
            link_answer(PET_LINK_STATUS, &answer);
            return PET_ENROLL_NEEDS_CODE;
        }
        /* Link codes off, or not on this cloud: the typed code alone for a
         * while. Anything else is "not now", and a typed code tries again. */
        if (answer.reply != PET_LINK_WAITING && strcmp(answer.error, "LINK_OFFERS_DISABLED") &&
            (answer.reply != PET_LINK_REFUSED || answer.http < 400 || answer.http >= 500 ||
             answer.http == 408 || answer.http == 429)) {
            *held = code[0] != 0;
            return PET_ENROLL_RETRY;
        }
        if (answer.reply != PET_LINK_WAITING) s_link_off_until = now + LINK_OFF_MS;
    }
    /* No claim waits for this device, or none can be told: a typed code is
     * redeemed directly. */
    s_enrollment.may_redeem = true;
    if (code[0]) return pet_enrollment_submit(&s_enrollment, code);
    if (now >= s_link_off_until) return PET_ENROLL_NEEDS_CODE;
    /* Link codes off: {} completes a claim without a number, which only this
     * device's typed code makes. A claim with one refuses it and waits for
     * its tap; the device still takes a typed code meanwhile. */
    const pet_enrollment_result_t result = pet_enrollment_resume(&s_enrollment);
    return result == PET_ENROLL_REJECTED ? PET_ENROLL_NEEDS_CODE : result;
}

/* Until the link machine's next call, but never before the cloud's wait. */
static TickType_t link_wait(uint64_t now, uint64_t due)
{
    if (due < s_quiet_until) due = s_quiet_until;
    return pdMS_TO_TICKS(due > now ? due - now : 0);
}

/* A device with an account moves only while the owner is on the deck's link
 * screen, and only with CONFIG_PET_DEVICE_RELINK: otherwise nothing calls
 * the link routes. Returns how long to wait for the next call or the
 * owner's action. */
static TickType_t moving(void)
{
    link_actions(true);
    /* A setup link still on the machine ended with setup itself. */
    if (!CONFIG_PET_DEVICE_RELINK || !s_link.moving || !pet_link_active(&s_link)) return portMAX_DELAY;
    if (!pet_network_wifi_is_ready()) return pdMS_TO_TICKS(1000);
    if (now_ms() >= s_quiet_until) {
        pet_link_call_t call = link_next();
        if (call != PET_LINK_NONE) {
            pet_link_answer_t answer;
            link_call(call, true, &answer);
            link_answer(call, &answer);
        }
    }
    if (!pet_link_active(&s_link)) return portMAX_DELAY;
    return link_wait(now_ms(), pet_link_due(&s_link));
}

static void setup_task(void *argument)
{
    (void)argument;
    const pet_enrollment_io_t io = {
        .read = read_record, .write = write_record,
        .random = random_bytes, .request = setup_request,
    };
    char code[PET_ENROLLMENT_CODE_BYTES] = {0}, item[PET_ENROLLMENT_CODE_BYTES];
    bool clock_synced = false;
    /* This join's first ask, the next one, and the wait before it. */
    uint64_t clock_first_ask = 0, clock_next_ask = 0;
    uint32_t clock_wait = 0;
    pet_enrollment_result_t reported = PET_ENROLL_RETRY;
    for (;;) {
        pet_enrollment_result_t status = PET_ENROLL_WAIT_WIFI;
        bool quiet = false, held = false;
        /* Wi-Fi radio was started before this worker, even while offline. Load
         * durable pairing for cached display; HTTP still waits for Wi-Fi/time. */
        if(!s_enrollment.loaded&&pet_enrollment_open(&s_enrollment,&io)==PET_ENROLL_STORAGE_ERROR)
            status=PET_ENROLL_STORAGE_ERROR;
        if(s_enrollment.loaded&&s_enrollment.complete&&!atomic_load(&s_complete)&&
           pet_enrollment_identity(&s_enrollment,s_completed_id,s_completed_credential))atomic_store(&s_complete,true);
        link_actions(s_enrollment.complete);
        if (status!=PET_ENROLL_STORAGE_ERROR&&pet_network_wifi_is_ready()) {
            /* No request goes out before the clock syncs. Offline, SNTP backs
             * off to 150 s between tries, so a join asks for the time at once:
             * after 2 h offline the first request came 152 s late (1 Oct). */
            uint64_t now = now_ms();
            if (!clock_synced && now >= clock_next_ask && esp_netif_sntp_start() == ESP_OK) {
                if (!clock_wait) clock_first_ask = now;
                clock_wait = !clock_wait ? CLOCK_ASK_FIRST_MS
                           : clock_wait * 2 < CLOCK_ASK_MAX_MS ? clock_wait * 2 : CLOCK_ASK_MAX_MS;
                clock_next_ask = now + clock_wait;
            }
            if (!clock_synced && esp_netif_sntp_sync_wait(0) == ESP_OK) {
                clock_synced = true;
                ESP_LOGI("pet_setup", "clock set %lu ms after the first ask",
                         (unsigned long)(now_ms() - clock_first_ask));
                (void)clock_first_ask; /* Only logged. */
            }
            status = clock_synced ? PET_ENROLL_RETRY : PET_ENROLL_WAIT_CLOCK;
            if (clock_synced && !s_enrollment.loaded) status = pet_enrollment_open(&s_enrollment, &io);
            /* The cloud's wait: nothing is sent, and a typed code waits too. */
            if (clock_synced && s_enrollment.loaded && now_ms() < s_quiet_until) quiet = held = true;
            else if (clock_synced && s_enrollment.loaded)
                status = s_enrollment.complete ? pet_enrollment_resume(&s_enrollment) : unlinked(code, &quiet, &held);
        }
        if (!pet_network_wifi_is_ready()) {
            clock_next_ask = 0;
            clock_wait = 0;
        }
        if (!held) pet_enrollment_clear(code, sizeof(code));
        /* A typed code the cloud asked to wait: the code screen says so. */
        else if (code[0] && now_ms() < s_quiet_until) {
            status = PET_ENROLL_WAIT_CLOUD;
            quiet = false;
        }
        if(status==PET_ENROLL_COMPLETE) {
            if(!atomic_load(&s_complete)&&pet_enrollment_identity(&s_enrollment,s_completed_id,s_completed_credential))
                atomic_store(&s_complete,true);
            atomic_store(&s_control_ready,true);
        }
        /* Only link calls, or none: the setup screen has nothing new. */
        if (quiet) status = reported;
        else s_status(status);
        reported = status;
        if (status == PET_ENROLL_COMPLETE || status == PET_ENROLL_STORAGE_ERROR) {
            /* No auto-rekey on corruption. The control worker can read the
             * immutable completed identity; no legacy socket is authorized. */
            atomic_store(&s_accepting, false);
            for (;;) {
                /* Drain transient submissions already in flight; never persist
                 * them or leave the UI accepting codes after terminal state. */
                xQueueReceive(s_codes, code, status == PET_ENROLL_COMPLETE ? moving() : portMAX_DELAY);
                pet_enrollment_clear(code, sizeof(code));
            }
        }
        const uint64_t now = now_ms();
        TickType_t wait = pdMS_TO_TICKS(1000);
        if (status != PET_ENROLL_WAIT_WIFI && status != PET_ENROLL_WAIT_CLOCK) {
            /* The next probe, a held code's next try (at the end of the
             * cloud's wait, or after no answer), or a setup link's next call,
             * if sooner. */
            uint64_t due = now + (!held ? PROBE_MS : now < s_quiet_until ? 0 : HELD_MS);
            if (!s_link.moving && pet_link_active(&s_link) && pet_link_due(&s_link) < due) due = pet_link_due(&s_link);
            wait = link_wait(now, due);
        }
        /* The owner's codes and actions are taken at once; the cloud's wait
         * only holds back calls. An empty item only wakes the worker. */
        if (xQueueReceive(s_codes, item, wait) == pdTRUE && item[0]) memcpy(code, item, sizeof(code));
        pet_enrollment_clear(item, sizeof(item));
    }
}

esp_err_t pet_setup_start(const char *https_origin, pet_setup_status_callback_t callback)
{
    /* Only a compile-time provisioned DNS origin and port, no path/userinfo/query.
     * This avoids accidental credential forwarding to a caller-supplied URL. */
    if (!https_origin || !callback || strncmp(https_origin, "https://", 8) ||
        !https_origin[8] || strlen(https_origin) >= sizeof(s_origin)) return ESP_ERR_INVALID_ARG;
    for (const char *p = https_origin + 8; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == ':'))
            return ESP_ERR_INVALID_ARG;
    if (s_codes) return ESP_ERR_INVALID_STATE;
    strcpy(s_origin, https_origin);
    s_status = callback;
    s_codes = xQueueCreate(1, PET_ENROLLMENT_CODE_BYTES);
    if (!s_codes) return ESP_ERR_NO_MEM;
    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&time_config) != ESP_OK) {
        vQueueDelete(s_codes);
        s_codes = NULL;
        return ESP_FAIL;
    }
    atomic_store(&s_accepting, true);
    if (xTaskCreate(setup_task, "pet_setup", 8192, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_codes);
        s_codes = NULL;
        atomic_store(&s_accepting, false);
        esp_netif_sntp_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pet_setup_on_link(pet_setup_link_callback_t callback)
{
    if (!s_codes) s_link_view = callback;
}

esp_err_t pet_setup_link(pet_setup_link_action_t action)
{
    if (!s_codes || (unsigned)action > PET_SETUP_LINK_LEAVE) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_link_action, (int)action);
    /* An empty item only wakes the worker; a full queue wakes it anyway. */
    const char wake[PET_ENROLLMENT_CODE_BYTES] = {0};
    xQueueSend(s_codes, wake, 0);
    return ESP_OK;
}

esp_err_t pet_setup_submit(const char *code)
{
    if (!pet_enrollment_code_valid(code)) return ESP_ERR_INVALID_ARG;
    if (!s_codes || !atomic_load(&s_accepting)) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_codes, code, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool pet_setup_identity(char device_id[PET_ENROLLMENT_DEVICE_ID_BYTES],
                         char credential[PET_ENROLLMENT_CREDENTIAL_BYTES])
{
    if(!device_id||!credential||!atomic_load(&s_complete))return false;
    memcpy(device_id,s_completed_id,sizeof(s_completed_id));
    memcpy(credential,s_completed_credential,sizeof(s_completed_credential));return true;
}

bool pet_setup_control_ready(void){return atomic_load(&s_control_ready);}
