#include "pet_link.h"

#include <string.h>

const char *const pet_link_paths[2][PET_LINK_NONE] = {
    {"/v1/device/setup/offer", "/v1/device/setup/status", "/v1/device/setup/complete", "/v1/device/setup/decline"},
    {"/v1/device/link/offer", "/v1/device/link/status", "/v1/device/link/confirm", "/v1/device/link/decline"},
};

static void wipe(char *text, size_t bytes)
{
    volatile char *p = text;
    while (bytes--) *p++ = 0;
}

/* strnlen, which strict C11 hosts do not declare. */
static size_t length_within(const char *text, size_t max)
{
    size_t length = 0;
    while (length < max && text[length]) ++length;
    return length;
}

static void copy(char *out, size_t capacity, const char *in)
{
    size_t length = length_within(in, capacity - 1);
    memcpy(out, in, length);
    out[length] = 0;
}

/* "Not now" from the cloud: ask again after its wait. */
static bool temporary(const pet_link_answer_t *a)
{
    static const char *const codes[] = {"RATE_LIMITED", "LINK_OFFERS_DISABLED", "RELINK_DISABLED",
                                        "ENROLLMENT_DISABLED", "CONTROL_UNAVAILABLE", "DEVICE_BUSY"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); ++i)
        if (!strcmp(a->error, codes[i])) return true;
    return a->http == 408 || a->http == 429 || a->http >= 500;
}

static uint64_t after(uint64_t now, unsigned seconds, unsigned fallback)
{
    return now + (uint64_t)(seconds ? seconds : fallback) * 1000u;
}

static void off(pet_link_t *l)
{
    wipe(l->code, sizeof(l->code));
    wipe(l->account, sizeof(l->account));
    wipe(l->check, sizeof(l->check));
    l->phase = PET_LINK_OFF;
    l->leaving = false;
    l->offers = 0;
}

/* A new code: it replaces the old one, which can no longer link. */
static void ask(pet_link_t *l, uint64_t now)
{
    wipe(l->code, sizeof(l->code));
    wipe(l->account, sizeof(l->account));
    wipe(l->check, sizeof(l->check));
    l->phase = PET_LINK_ASKING;
    l->next_ms = now;
}

/* Linking stops until the owner asks again, and the screen says why. */
static void stop(pet_link_t *l, const char *error)
{
    off(l);
    copy(l->error, sizeof(l->error), error);
}

static void fail(pet_link_t *l, const char *error)
{
    stop(l, error[0] ? error : "LINK_REFUSED");
    l->phase = PET_LINK_FAILED;
}

/* A claimed code is used up; the question waits, and status keeps being
 * asked so a link the owner cancels on the website ends it. */
static void claimed(pet_link_t *l, const pet_link_answer_t *a, uint64_t now)
{
    wipe(l->code, sizeof(l->code));
    copy(l->account, sizeof(l->account), a->account);
    copy(l->check, sizeof(l->check), a->check);
    if (l->phase != PET_LINK_CLAIMED) l->error[0] = 0;
    if (!l->poll_ms) l->poll_ms = 5000;
    l->phase = PET_LINK_CLAIMED;
    l->offers = 0;
    l->next_ms = now + l->poll_ms;
}

void pet_link_start(pet_link_t *l, bool moving, uint64_t now)
{
    if (l->moving == moving && (l->phase == PET_LINK_CLAIMED || l->phase == PET_LINK_LINKING)) return;
    char device[sizeof(l->device)];
    memcpy(device, l->device, sizeof(device));
    off(l);
    memset(l, 0, sizeof(*l));
    memcpy(l->device, device, sizeof(device));
    l->moving = moving;
    l->poll_ms = 5000;
    ask(l, now);
}

bool pet_link_tap(pet_link_t *l, bool yes, uint64_t now)
{
    /* A move sends the number the question showed: none, no Move. */
    if (l->phase != PET_LINK_CLAIMED || (yes && l->moving && strlen(l->check) != PET_LINK_CHECK_LENGTH)) return false;
    l->phase = yes ? PET_LINK_LINKING : PET_LINK_DECLINING;
    l->leaving = false;
    l->error[0] = 0;
    l->next_ms = now;
    return true;
}

void pet_link_leave(pet_link_t *l, uint64_t now)
{
    switch (l->phase) {
        case PET_LINK_CLAIMED:
        case PET_LINK_LINKING:
            return; /* The question stays until the owner answers it. */
        case PET_LINK_DECLINING:
            l->leaving = true;
            return;
        case PET_LINK_ASKING:
        case PET_LINK_SHOWING:
            /* A moving device's code dies at once. A setup code cannot be
             * declined unclaimed: it runs out, and a claim still waits for a
             * tap. */
            if (l->moving) {
                wipe(l->code, sizeof(l->code));
                l->phase = PET_LINK_DECLINING;
                l->leaving = true;
                l->next_ms = now;
                return;
            }
            off(l);
            return;
        default:
            off(l);
            return;
    }
}

pet_link_call_t pet_link_next(pet_link_t *l, uint64_t now)
{
    /* Showing without a code: status first says where the claim stands. */
    if (l->phase == PET_LINK_SHOWING && l->code[0] && now >= l->expires_ms) {
        if (l->offers >= PET_LINK_MAX_OFFERS) {
            stop(l, "LINK_CODE_EXPIRED");
            return PET_LINK_NONE;
        }
        ask(l, now);
    }
    if (now < l->next_ms) return PET_LINK_NONE;
    switch (l->phase) {
        case PET_LINK_ASKING:
        case PET_LINK_PAUSED:
            return PET_LINK_OFFER;
        case PET_LINK_SHOWING:
        case PET_LINK_CLAIMED:
            return PET_LINK_STATUS;
        case PET_LINK_LINKING:
            return PET_LINK_CONFIRM;
        case PET_LINK_DECLINING:
            return PET_LINK_DECLINE;
        default:
            return PET_LINK_NONE;
    }
}

uint64_t pet_link_due(const pet_link_t *l)
{
    return l->phase == PET_LINK_SHOWING && l->code[0] && l->expires_ms < l->next_ms ? l->expires_ms : l->next_ms;
}

static void offered(pet_link_t *l, const pet_link_answer_t *a, uint64_t now)
{
    /* Codes last ten minutes: a new one ten seconds before, never later. */
    unsigned life = a->expires_in < 600 ? a->expires_in : 600;
    life = life > 40 ? life - 10 : 30;
    unsigned poll = a->poll_seconds < 3 ? 3 : a->poll_seconds > 60 ? 60 : a->poll_seconds;
    copy(l->code, sizeof(l->code), a->code);
    l->error[0] = 0;
    l->poll_ms = poll * 1000u;
    l->expires_ms = now + (uint64_t)life * 1000u;
    l->next_ms = now + l->poll_ms;
    l->phase = PET_LINK_SHOWING;
    ++l->offers;
}

void pet_link_answer(pet_link_t *l, pet_link_call_t call, const pet_link_answer_t *a, uint64_t now)
{
    const pet_link_reply_t reply = a->reply;
    if (call == PET_LINK_DECLINE) {
        /* Declined or not, that code is over: a new offer replaces it. A setup
         * decline refused because the link already completed (a lost answer
         * to Link) asks status, which says so. */
        if (l->leaving) off(l);
        else if (!l->moving && reply == PET_LINK_REFUSED && !strcmp(a->error, "DEVICE_AUTH_REQUIRED")) {
            wipe(l->code, sizeof(l->code));
            l->phase = PET_LINK_SHOWING;
            l->next_ms = now;
        } else ask(l, now);
        return;
    }
    if (call == PET_LINK_OFFER) {
        const bool paused = reply == PET_LINK_REFUSED;
        if (reply == PET_LINK_OFFERED) offered(l, a, now);
        else if (reply == PET_LINK_CLAIMED_BY) claimed(l, a, now);
        else if (paused && !temporary(a)) fail(l, a->error);
        else if (++l->offers >= PET_LINK_MAX_OFFERS) stop(l, paused ? a->error : "LINK_NO_ANSWER");
        else {
            /* "Not now", or a lost or unreadable answer: ask again after the wait. */
            copy(l->error, sizeof(l->error), a->error);
            l->phase = paused ? PET_LINK_PAUSED : PET_LINK_ASKING;
            l->next_ms = after(now, a->retry_seconds, paused ? 30 : 10);
        }
        return;
    }
    if (call == PET_LINK_STATUS) {
        /* Asked in the background, with no code on screen, status only ever
         * raises the question: nothing links without the owner's tap. */
        if (l->phase == PET_LINK_OFF && reply != PET_LINK_CLAIMED_BY && reply != PET_LINK_DONE) return;
        if (reply == PET_LINK_WAITING) {
            /* Claimed, then no longer: the owner cancelled on the website. */
            if (l->phase == PET_LINK_CLAIMED || !l->code[0]) ask(l, now);
            else l->next_ms = now + l->poll_ms;
        } else if (reply == PET_LINK_CLAIMED_BY) claimed(l, a, now);
        else if (reply == PET_LINK_EXPIRED) ask(l, now);
        else if (reply == PET_LINK_DONE) {
            /* Setup finished already (a restart): completing it records it. */
            l->phase = PET_LINK_LINKING;
            l->next_ms = now;
        } else if (reply == PET_LINK_REFUSED && !temporary(a)) fail(l, a->error);
        else l->next_ms = after(now, a->retry_seconds, l->poll_ms / 1000u ? l->poll_ms / 1000u : 5);
        return;
    }
    if (call == PET_LINK_CONFIRM) {
        if (reply == PET_LINK_DONE) {
            wipe(l->code, sizeof(l->code));
            l->phase = PET_LINK_LINKED;
        } else if (reply != PET_LINK_REFUSED) l->next_ms = after(now, a->retry_seconds, 5); /* The same tap, again. */
        else if (!strcmp(a->error, "LINK_NOT_CLAIMED") || !strcmp(a->error, "SETUP_CLAIM_CHANGED")) {
            /* Not this claim (another number now), or none: status says. */
            l->phase = PET_LINK_SHOWING;
            l->next_ms = now;
        } else if (!strcmp(a->error, "SETUP_NOT_CLAIMED")) ask(l, now);
        else if (!strcmp(a->error, "DEVICE_BUSY") || !strcmp(a->error, "LINK_UNAVAILABLE")) {
            /* Busy installing, or the new account cannot take the device yet:
             * nothing moved, and the question stays for another tap. */
            l->phase = PET_LINK_CLAIMED;
            copy(l->error, sizeof(l->error), a->error);
            l->next_ms = now + l->poll_ms;
        } else if (temporary(a)) l->next_ms = after(now, a->retry_seconds, 30);
        else fail(l, a->error);
    }
}

bool pet_link_active(const pet_link_t *l)
{
    return l->phase != PET_LINK_OFF && l->phase != PET_LINK_LINKED && l->phase != PET_LINK_FAILED;
}

static bool digits(const char *text, unsigned count, unsigned *value)
{
    *value = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        *value = *value * 10 + (unsigned)(text[i] - '0');
    }
    return true;
}

bool pet_link_utc(const char *s, int64_t *seconds)
{
    unsigned year, month, day, hour, minute, second;
    if (!s || !seconds || length_within(s, 32) < 20 || !digits(s, 4, &year) || s[4] != '-' || !digits(s + 5, 2, &month) ||
        s[7] != '-' || !digits(s + 8, 2, &day) || s[10] != 'T' || !digits(s + 11, 2, &hour) || s[13] != ':' ||
        !digits(s + 14, 2, &minute) || s[16] != ':' || !digits(s + 17, 2, &second)) return false;
    const char *p = s + 19;
    if (*p == '.') {
        if (p[1] < '0' || p[1] > '9') return false;
        for (++p; *p >= '0' && *p <= '9'; ++p) {}
    }
    if (p[0] != 'Z' || p[1] || year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60) return false;
    /* Days since 1970 (H. Hinnant's days_from_civil), from March-based years. */
    const unsigned y = year - (month <= 2), era = y / 400, of_era = y - era * 400;
    const unsigned of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const int64_t days = (int64_t)era * 146097 + of_era * 365 + of_era / 4 - of_era / 100 + of_year - 719468;
    *seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}
