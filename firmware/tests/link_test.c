/* The link code machine alone: offer, show, poll, the question, confirm and
 * decline, both ways in. Host C, no I/O. */
#include "pet_link.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static pet_link_t link;
static uint64_t now = 1000000;

static pet_link_answer_t answer(pet_link_reply_t reply)
{
    pet_link_answer_t a = {.reply = reply, .http = 200};
    return a;
}
static pet_link_answer_t offered(const char *code)
{
    pet_link_answer_t a = answer(PET_LINK_OFFERED);
    strcpy(a.code, code);
    a.poll_seconds = 5;
    a.expires_in = 600;
    return a;
}
static pet_link_answer_t claimed(const char *account)
{
    pet_link_answer_t a = answer(PET_LINK_CLAIMED_BY);
    strcpy(a.account, account);
    return a;
}
static pet_link_answer_t numbered(const char *account, const char *check)
{
    pet_link_answer_t a = claimed(account);
    strcpy(a.check, check);
    return a;
}
static pet_link_answer_t refused(int http, const char *error, unsigned retry)
{
    pet_link_answer_t a = {.reply = PET_LINK_REFUSED, .http = http, .retry_seconds = retry};
    strcpy(a.error, error);
    return a;
}
/* Nothing due until `at`, and the worker's wait (pet_link_due) agrees: a wait
 * of no time with no call due would spin the worker. */
static void idle_until(uint64_t at)
{
    for (; now < at; now += 250)
        assert(pet_link_next(&link, now) == PET_LINK_NONE && (!pet_link_active(&link) || pet_link_due(&link) > now));
}
/* Nothing is due before the machine's time; then exactly `expected`, and its
 * answer lands at that moment. */
static void call(pet_link_call_t expected, pet_link_answer_t a)
{
    if (now < link.next_ms) idle_until(link.next_ms);
    assert(pet_link_next(&link, now) == expected);
    pet_link_answer(&link, expected, &a, now);
}
static void show(const char *code, bool moving)
{
    pet_link_start(&link, moving, now);
    assert(link.phase == PET_LINK_ASKING && link.moving == moving && pet_link_active(&link));
    call(PET_LINK_OFFER, offered(code));
    assert(link.phase == PET_LINK_SHOWING && !strcmp(link.code, code) && link.poll_ms == 5000);
}

static void setup_journey(void)
{
    strcpy(link.device, "1a2b3c");
    show("K7QX9MPA", false);
    assert(!strcmp(link.device, "1a2b3c")); /* The screen's device name survives a start. */
    /* Nobody typed it yet: status every five seconds, never sooner. */
    idle_until(now + 5000);
    call(PET_LINK_STATUS, answer(PET_LINK_WAITING));
    assert(link.phase == PET_LINK_SHOWING && link.next_ms == now + 5000);
    idle_until(now + 5000);
    /* Typed: the code is used up and the question waits with the claim's
     * number, polling on. */
    call(PET_LINK_STATUS, numbered("Alex's pets", "472"));
    assert(link.phase == PET_LINK_CLAIMED && !link.code[0] && !strcmp(link.account, "Alex's pets"));
    assert(!strcmp(link.check, "472"));
    idle_until(now + 5000);
    call(PET_LINK_STATUS, numbered("Alex's pets", "472"));
    assert(link.phase == PET_LINK_CLAIMED);
    /* Leaving never answers it; starting again keeps it. */
    pet_link_leave(&link, now);
    pet_link_start(&link, false, now);
    assert(link.phase == PET_LINK_CLAIMED && !strcmp(link.account, "Alex's pets"));
    assert(!pet_link_tap(&(pet_link_t){.phase = PET_LINK_SHOWING}, true, now));
    assert(pet_link_tap(&link, true, now) && link.phase == PET_LINK_LINKING && !strcmp(link.check, "472"));
    /* The claim changed while the question showed: status, then the new one. */
    call(PET_LINK_CONFIRM, refused(409, "SETUP_CLAIM_CHANGED", 0));
    assert(link.phase == PET_LINK_SHOWING && !link.code[0]);
    call(PET_LINK_STATUS, numbered("Alex's pets", "815"));
    assert(link.phase == PET_LINK_CLAIMED && !strcmp(link.check, "815"));
    assert(pet_link_tap(&link, true, now) && !strcmp(link.check, "815"));
    /* No answer: the same tap, again. */
    call(PET_LINK_CONFIRM, answer(PET_LINK_NO_ANSWER));
    assert(link.phase == PET_LINK_LINKING);
    idle_until(now + 5000);
    call(PET_LINK_CONFIRM, answer(PET_LINK_DONE));
    assert(link.phase == PET_LINK_LINKED && !pet_link_active(&link) && pet_link_next(&link, now) == PET_LINK_NONE);
}

static void claimed_on_offer(void)
{
    memset(&link, 0, sizeof(link));
    /* An owner claimed this identity already: straight to the question, with
     * or without the account's name. */
    pet_link_start(&link, false, now);
    call(PET_LINK_OFFER, claimed(""));
    assert(link.phase == PET_LINK_CLAIMED && !link.account[0]);
    pet_link_start(&link, true, now); /* The other way in starts over. */
    assert(link.phase == PET_LINK_ASKING && link.moving);
    call(PET_LINK_OFFER, claimed("Mia"));
    assert(link.phase == PET_LINK_CLAIMED && !strcmp(link.account, "Mia"));
}

static void not_me_and_cancelled(void)
{
    memset(&link, 0, sizeof(link));
    show("K7QX9MPA", false);
    call(PET_LINK_STATUS, claimed("Stranger"));
    assert(pet_link_tap(&link, false, now) && link.phase == PET_LINK_DECLINING);
    call(PET_LINK_DECLINE, answer(PET_LINK_DECLINED));
    /* Nothing linked: a new code, the same identity. */
    assert(link.phase == PET_LINK_ASKING && !link.account[0]);
    call(PET_LINK_OFFER, offered("W2MN8QRT"));
    assert(!strcmp(link.code, "W2MN8QRT"));
    /* The owner cancelled first, so the decline finds nothing: a new code. */
    call(PET_LINK_STATUS, claimed("Stranger"));
    pet_link_tap(&link, false, now);
    call(PET_LINK_DECLINE, refused(404, "SETUP_NOT_CLAIMED", 0));
    assert(link.phase == PET_LINK_ASKING);
    call(PET_LINK_OFFER, offered("K7QX9MPA"));
    /* A decline refused because the link already completed (a lost answer to
     * Link): status says so, and completing records it. */
    call(PET_LINK_STATUS, numbered("Alex", "472"));
    pet_link_tap(&link, false, now);
    call(PET_LINK_DECLINE, refused(401, "DEVICE_AUTH_REQUIRED", 0));
    assert(link.phase == PET_LINK_SHOWING && !link.code[0]);
    call(PET_LINK_STATUS, answer(PET_LINK_DONE));
    assert(link.phase == PET_LINK_LINKING);
    call(PET_LINK_CONFIRM, answer(PET_LINK_DONE));
    assert(link.phase == PET_LINK_LINKED);
    show("K7QX9MPA", false);
    /* The owner cancelled on the website while the question showed. */
    call(PET_LINK_STATUS, claimed("Alex"));
    idle_until(now + 5000);
    call(PET_LINK_STATUS, answer(PET_LINK_WAITING));
    assert(link.phase == PET_LINK_ASKING);
    call(PET_LINK_OFFER, offered("W2MN8QRT"));
    assert(link.phase == PET_LINK_SHOWING);
}

static void expiry_refresh(void)
{
    memset(&link, 0, sizeof(link));
    show("K7QX9MPA", false);
    /* Ten minutes, renewed ten seconds early. */
    assert(link.expires_ms == now + 590000);
    uint64_t expires = link.expires_ms;
    while (now < expires) {
        pet_link_call_t due = pet_link_next(&link, now);
        if (due == PET_LINK_STATUS) pet_link_answer(&link, due, &(pet_link_answer_t){.reply = PET_LINK_WAITING}, now);
        else assert(due == PET_LINK_NONE);
        now += 1000;
    }
    call(PET_LINK_OFFER, offered("W2MN8QRT"));
    assert(link.phase == PET_LINK_SHOWING && !strcmp(link.code, "W2MN8QRT") && link.offers == 2);
    /* A short or odd life still renews in time, and never polls too fast. */
    pet_link_answer_t brief = offered("AAAA2222");
    brief.expires_in = 12;
    brief.poll_seconds = 1;
    now = link.expires_ms;
    call(PET_LINK_OFFER, brief);
    assert(link.expires_ms == now + 30000 && link.poll_ms == 3000 && link.offers == 3);
    /* Three codes that nobody typed: linking stops, the screen says why. */
    now = link.expires_ms;
    assert(pet_link_next(&link, now) == PET_LINK_NONE);
    assert(link.phase == PET_LINK_OFF && !link.code[0] && !strcmp(link.error, "LINK_CODE_EXPIRED"));
    /* A claim resets the count. */
    show("K7QX9MPA", false);
    call(PET_LINK_STATUS, claimed("Alex"));
    assert(!link.offers);
}

static void errors(void)
{
    memset(&link, 0, sizeof(link));
    static const struct { int http; const char *code; pet_link_phase_t phase; } offers[] = {
        {409, "SETUP_IDENTITY_TAKEN", PET_LINK_FAILED}, {409, "DEVICE_REVOKED", PET_LINK_FAILED},
        {409, "RELINK_UNSUPPORTED", PET_LINK_FAILED}, {401, "DEVICE_AUTH_REQUIRED", PET_LINK_FAILED},
        {400, "INVALID_REQUEST", PET_LINK_FAILED}, {404, "NOT_FOUND", PET_LINK_FAILED},
        {429, "RATE_LIMITED", PET_LINK_PAUSED}, {503, "LINK_OFFERS_DISABLED", PET_LINK_PAUSED},
        {503, "RELINK_DISABLED", PET_LINK_PAUSED}, {503, "ENROLLMENT_DISABLED", PET_LINK_PAUSED},
        {503, "CONTROL_UNAVAILABLE", PET_LINK_PAUSED}, {409, "DEVICE_BUSY", PET_LINK_PAUSED},
        {502, "UNKNOWN_GATEWAY", PET_LINK_PAUSED},
    };
    for (size_t i = 0; i < sizeof(offers) / sizeof(*offers); ++i) {
        pet_link_start(&link, i % 2, now);
        call(PET_LINK_OFFER, refused(offers[i].http, offers[i].code, 0));
        assert(link.phase == offers[i].phase && !strcmp(link.error, offers[i].code));
        if (link.phase == PET_LINK_FAILED) assert(!pet_link_active(&link) && pet_link_next(&link, now + 3600000) == PET_LINK_NONE);
    }
    /* Retry-After is honoured, and without one the wait is thirty seconds. */
    pet_link_start(&link, false, now);
    call(PET_LINK_OFFER, refused(429, "RATE_LIMITED", 60));
    idle_until(now + 60000);
    call(PET_LINK_OFFER, refused(503, "LINK_OFFERS_DISABLED", 0));
    idle_until(now + 30000);
    call(PET_LINK_OFFER, offered("K7QX9MPA"));
    assert(link.phase == PET_LINK_SHOWING && !link.error[0]);
    /* Status: not now keeps the code and its wait; a refusal ends linking. */
    call(PET_LINK_STATUS, refused(503, "CONTROL_UNAVAILABLE", 20));
    assert(link.phase == PET_LINK_SHOWING);
    idle_until(now + 20000);
    call(PET_LINK_STATUS, answer(PET_LINK_NO_ANSWER));
    assert(link.phase == PET_LINK_SHOWING);
    idle_until(now + 5000);
    call(PET_LINK_STATUS, refused(401, "DEVICE_AUTH_REQUIRED", 0));
    assert(link.phase == PET_LINK_FAILED && !link.code[0]);
    /* A lost offer answer asks again, soon; the retry gets a new code. */
    pet_link_start(&link, false, now);
    call(PET_LINK_OFFER, answer(PET_LINK_NO_ANSWER));
    assert(link.phase == PET_LINK_ASKING);
    idle_until(now + 10000);
    call(PET_LINK_OFFER, offered("W2MN8QRT"));
    assert(!strcmp(link.code, "W2MN8QRT"));
}

static void moving_journey(void)
{
    memset(&link, 0, sizeof(link));
    show("K7QX9MPA", true);
    call(PET_LINK_STATUS, answer(PET_LINK_WAITING)); /* link/status "offered" */
    assert(link.phase == PET_LINK_SHOWING && !strcmp(link.code, "K7QX9MPA"));
    idle_until(now + 5000);
    call(PET_LINK_STATUS, answer(PET_LINK_EXPIRED));
    assert(link.phase == PET_LINK_ASKING);
    call(PET_LINK_OFFER, offered("W2MN8QRT"));
    call(PET_LINK_STATUS, numbered("Mia's pets", "093"));
    assert(!strcmp(link.check, "093"));
    pet_link_tap(&link, true, now);
    /* Not claimed after all: status says where it stands. */
    call(PET_LINK_CONFIRM, refused(409, "LINK_NOT_CLAIMED", 0));
    assert(link.phase == PET_LINK_SHOWING);
    /* Move needs the claim's number, which link/confirm carries. */
    call(PET_LINK_STATUS, claimed("Mia's pets"));
    assert(!pet_link_tap(&link, true, now) && link.phase == PET_LINK_CLAIMED);
    call(PET_LINK_STATUS, numbered("Mia's pets", "093"));
    assert(pet_link_tap(&link, true, now));
    /* Busy installing: the question stays for another tap. */
    call(PET_LINK_CONFIRM, refused(409, "DEVICE_BUSY", 0));
    assert(link.phase == PET_LINK_CLAIMED && !strcmp(link.error, "DEVICE_BUSY"));
    assert(pet_link_tap(&link, true, now) && !link.error[0]);
    /* The new account cannot take the device yet: nothing moved, and the
     * question stays. */
    call(PET_LINK_CONFIRM, refused(409, "LINK_UNAVAILABLE", 0));
    assert(link.phase == PET_LINK_CLAIMED && !strcmp(link.error, "LINK_UNAVAILABLE") && !strcmp(link.check, "093"));
    call(PET_LINK_STATUS, answer(PET_LINK_EXPIRED));
    call(PET_LINK_OFFER, offered("K7QX9MPA"));
    call(PET_LINK_STATUS, numbered("Mia's pets", "093"));
    pet_link_tap(&link, true, now);
    call(PET_LINK_CONFIRM, refused(503, "RELINK_DISABLED", 15));
    assert(link.phase == PET_LINK_LINKING);
    idle_until(now + 15000);
    call(PET_LINK_CONFIRM, answer(PET_LINK_DONE)); /* moved */
    assert(link.phase == PET_LINK_LINKED);
}

static void leaving(void)
{
    memset(&link, 0, sizeof(link));
    /* A moving device's code dies at once; then linking is off. */
    show("K7QX9MPA", true);
    pet_link_leave(&link, now);
    assert(link.phase == PET_LINK_DECLINING && link.leaving && !link.code[0]);
    call(PET_LINK_DECLINE, refused(503, "CONTROL_UNAVAILABLE", 0));
    assert(link.phase == PET_LINK_OFF && !pet_link_active(&link));
    /* Not me, then leaving while the decline is on its way: off after it. */
    show("K7QX9MPA", true);
    call(PET_LINK_STATUS, claimed("Mia"));
    pet_link_tap(&link, false, now);
    pet_link_leave(&link, now);
    call(PET_LINK_DECLINE, answer(PET_LINK_DECLINED));
    assert(link.phase == PET_LINK_OFF);
    /* A setup code cannot be declined unclaimed: it simply stops. */
    show("K7QX9MPA", false);
    pet_link_leave(&link, now);
    assert(link.phase == PET_LINK_OFF && !link.code[0] && pet_link_next(&link, now) == PET_LINK_NONE);
    pet_link_start(&link, false, now);
    call(PET_LINK_OFFER, refused(503, "LINK_OFFERS_DISABLED", 30));
    pet_link_leave(&link, now);
    assert(link.phase == PET_LINK_OFF);
    /* Failed: leaving clears it for the next start. */
    pet_link_start(&link, true, now);
    call(PET_LINK_OFFER, refused(409, "RELINK_UNSUPPORTED", 0));
    pet_link_leave(&link, now);
    assert(link.phase == PET_LINK_OFF);
}

static void background(void)
{
    /* Off the link screen, only a claim or a finished setup changes anything:
     * nothing ever links without the owner's tap. */
    memset(&link, 0, sizeof(link));
    pet_link_answer(&link, PET_LINK_STATUS, &(pet_link_answer_t){.reply = PET_LINK_WAITING}, now);
    pet_link_answer_t refusal = refused(503, "LINK_OFFERS_DISABLED", 0);
    pet_link_answer(&link, PET_LINK_STATUS, &refusal, now);
    assert(link.phase == PET_LINK_OFF && pet_link_next(&link, now) == PET_LINK_NONE);
    pet_link_answer_t claim = claimed("Alex");
    pet_link_answer(&link, PET_LINK_STATUS, &claim, now);
    assert(link.phase == PET_LINK_CLAIMED && link.poll_ms == 5000 && pet_link_active(&link));
    idle_until(now + 5000);
    call(PET_LINK_STATUS, claimed("Alex"));
    /* Setup finished already (a restart): completing records it. */
    memset(&link, 0, sizeof(link));
    pet_link_answer(&link, PET_LINK_STATUS, &(pet_link_answer_t){.reply = PET_LINK_DONE}, now);
    assert(link.phase == PET_LINK_LINKING);
    call(PET_LINK_CONFIRM, answer(PET_LINK_DONE));
    assert(link.phase == PET_LINK_LINKED);
    /* Found in the background, so no code was ever shown: a changed claim
     * asks status, never a new code. */
    memset(&link, 0, sizeof(link));
    pet_link_answer_t first = numbered("Alex", "472");
    pet_link_answer(&link, PET_LINK_STATUS, &first, now);
    pet_link_tap(&link, true, now);
    call(PET_LINK_CONFIRM, refused(409, "SETUP_CLAIM_CHANGED", 0));
    call(PET_LINK_STATUS, numbered("Alex", "815"));
    assert(link.phase == PET_LINK_CLAIMED && !strcmp(link.check, "815"));
    /* The review's case: a claim found in the background (no code ever
     * shown), Link, a changed claim, then a status the cloud rate-limits. The
     * next call waits the cloud's sixty seconds, and the wait agrees. */
    memset(&link, 0, sizeof(link));
    first = numbered("Alex", "472");
    pet_link_answer(&link, PET_LINK_STATUS, &first, now);
    pet_link_tap(&link, true, now);
    call(PET_LINK_CONFIRM, refused(409, "SETUP_CLAIM_CHANGED", 0));
    call(PET_LINK_STATUS, refused(429, "RATE_LIMITED", 60));
    assert(link.phase == PET_LINK_SHOWING && !link.code[0] && pet_link_due(&link) == now + 60000);
    idle_until(now + 60000);
    call(PET_LINK_STATUS, answer(PET_LINK_NO_ANSWER));
    assert(pet_link_due(&link) == now + 5000);
    idle_until(now + 5000);
    /* A confirm the cloud refuses because nothing is claimed: a new code. */
    memset(&link, 0, sizeof(link));
    show("K7QX9MPA", false);
    call(PET_LINK_STATUS, claimed("Alex"));
    pet_link_tap(&link, true, now);
    call(PET_LINK_CONFIRM, refused(404, "SETUP_NOT_CLAIMED", 0));
    assert(link.phase == PET_LINK_ASKING);
}

/* Offers without a claim stop after PET_LINK_MAX_OFFERS, whatever went
 * wrong: lost or unreadable answers, or the cloud's "not now". */
static void bounded(void)
{
    memset(&link, 0, sizeof(link));
    pet_link_start(&link, false, now);
    call(PET_LINK_OFFER, answer(PET_LINK_NO_ANSWER));
    call(PET_LINK_OFFER, answer(PET_LINK_NO_ANSWER));
    assert(link.phase == PET_LINK_ASKING);
    call(PET_LINK_OFFER, answer(PET_LINK_NO_ANSWER));
    assert(link.phase == PET_LINK_OFF && !strcmp(link.error, "LINK_NO_ANSWER") && pet_link_next(&link, now + 3600000) == PET_LINK_NONE);
    pet_link_start(&link, true, now);
    call(PET_LINK_OFFER, refused(503, "RELINK_DISABLED", 30));
    call(PET_LINK_OFFER, refused(429, "RATE_LIMITED", 60));
    assert(link.phase == PET_LINK_PAUSED);
    call(PET_LINK_OFFER, refused(503, "RELINK_DISABLED", 30));
    assert(link.phase == PET_LINK_OFF && !strcmp(link.error, "RELINK_DISABLED") && !pet_link_active(&link));
    /* A code, then answers without one: three in all. */
    show("K7QX9MPA", false);
    now = link.expires_ms;
    call(PET_LINK_OFFER, answer(PET_LINK_NO_ANSWER));
    call(PET_LINK_OFFER, refused(503, "CONTROL_UNAVAILABLE", 0));
    assert(link.phase == PET_LINK_OFF && !strcmp(link.error, "CONTROL_UNAVAILABLE"));
    /* The owner asks again: three more. */
    pet_link_start(&link, false, now);
    assert(link.phase == PET_LINK_ASKING && !link.offers && !link.error[0]);
}

static void utc(void)
{
    int64_t seconds;
    assert(pet_link_utc("1970-01-01T00:00:00Z", &seconds) && seconds == 0);
    assert(pet_link_utc("2026-09-25T14:52:00.000Z", &seconds) && seconds == 1790347920);
    assert(pet_link_utc("2000-02-29T12:00:00Z", &seconds) && seconds == 951825600);
    assert(pet_link_utc("2100-03-01T00:00:00.5Z", &seconds) && seconds == 4107542400);
    assert(pet_link_utc("2024-12-31T23:59:59.999999Z", &seconds) && seconds == 1735689599);
    static const char *const bad[] = {"", "2026-09-25", "2026-09-25T14:52:00", "2026-09-25T14:52:00+00:00",
        "2026-09-25 14:52:00Z", "2026-13-25T14:52:00Z", "2026-09-25T24:00:00Z", "1969-12-31T23:59:59Z",
        "2026-09-25T14:52:00.Z", "2026-09-25T14:52:00Zjunk", "2026-9-25T14:52:00Z"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) assert(!pet_link_utc(bad[i], &seconds));
    assert(!pet_link_utc(NULL, &seconds));
}

int main(void)
{
    assert(!strcmp(pet_link_paths[0][PET_LINK_OFFER], "/v1/device/setup/offer"));
    assert(!strcmp(pet_link_paths[0][PET_LINK_STATUS], "/v1/device/setup/status"));
    assert(!strcmp(pet_link_paths[0][PET_LINK_CONFIRM], "/v1/device/setup/complete"));
    assert(!strcmp(pet_link_paths[0][PET_LINK_DECLINE], "/v1/device/setup/decline"));
    assert(!strcmp(pet_link_paths[1][PET_LINK_OFFER], "/v1/device/link/offer"));
    assert(!strcmp(pet_link_paths[1][PET_LINK_STATUS], "/v1/device/link/status"));
    assert(!strcmp(pet_link_paths[1][PET_LINK_CONFIRM], "/v1/device/link/confirm"));
    assert(!strcmp(pet_link_paths[1][PET_LINK_DECLINE], "/v1/device/link/decline"));
    setup_journey();
    claimed_on_offer();
    not_me_and_cancelled();
    expiry_refresh();
    errors();
    moving_journey();
    leaving();
    background();
    bounded();
    utc();
    puts("link code: offer, poll, question, link/move, not me, cancel, expiry, errors, backoff and leaving passed");
    return 0;
}
