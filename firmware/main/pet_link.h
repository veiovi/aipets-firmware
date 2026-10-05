#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The link code. The device shows eight characters, an
 * owner types them at aipets.com/link, the device asks "Link to <account>?"
 * beside the claim's check number, which aipets.com shows the owner who
 * claimed, and only a tap on Link (Move, for a device that has an account)
 * links it, sending that number back.
 * One small machine serves both ways in:
 *  - a device without an account: setup/offer, setup/status, setup/complete
 *    and setup/decline. The typed four-character code stays the fallback;
 *  - a linked device moving to another account, only from the control deck:
 *    link/offer, link/status, link/confirm and link/decline.
 * The worker makes the call pet_link_next() names, then hands the checked
 * answer to pet_link_answer(). No I/O, clock or logging here: the code is a
 * short-lived secret that only the screen shows. */

#define PET_LINK_CODE_LENGTH 8
#define PET_LINK_CHECK_LENGTH 3
/* Offers in a row with no claim (codes nobody typed, answers without a code,
 * or "not now"): then linking stops until the owner asks again. */
#define PET_LINK_MAX_OFFERS 3

typedef enum { PET_LINK_OFFER, PET_LINK_STATUS, PET_LINK_CONFIRM, PET_LINK_DECLINE, PET_LINK_NONE } pet_link_call_t;
/* The cloud's routes, [moving][call]: the one place they are named. */
extern const char *const pet_link_paths[2][PET_LINK_NONE];

typedef enum {
    PET_LINK_OFF,       /* not linking; `error` says why it stopped, if it did */
    PET_LINK_ASKING,    /* asking for a code */
    PET_LINK_SHOWING,   /* the code is on screen */
    PET_LINK_CLAIMED,   /* an owner typed it: the question waits for a tap */
    PET_LINK_LINKING,   /* Link or Move was tapped */
    PET_LINK_DECLINING, /* Not me was tapped, or a moving device left */
    PET_LINK_LINKED,    /* linked, or moved */
    PET_LINK_PAUSED,    /* no code for now: asks again after the cloud's wait */
    PET_LINK_FAILED,    /* this device cannot link this way: `error` says why */
} pet_link_phase_t;

typedef enum {
    PET_LINK_NO_ANSWER,  /* none, or one not understood */
    PET_LINK_OFFERED,    /* code, expires_in, poll_seconds */
    PET_LINK_WAITING,    /* the code is not claimed yet */
    PET_LINK_CLAIMED_BY, /* account and check, either possibly empty */
    PET_LINK_EXPIRED,    /* the code ran out */
    PET_LINK_DONE,       /* setup complete, or moved */
    PET_LINK_DECLINED,
    PET_LINK_REFUSED,    /* the cloud's error code */
} pet_link_reply_t;

typedef struct {
    pet_link_reply_t reply;
    int http;                  /* the answer's HTTP status, 0 without one */
    unsigned retry_seconds;    /* its Retry-After, 0 without one */
    unsigned poll_seconds, expires_in;
    char code[PET_LINK_CODE_LENGTH + 1], account[81], error[33], check[PET_LINK_CHECK_LENGTH + 1];
} pet_link_answer_t;

typedef struct {
    pet_link_phase_t phase;
    bool moving;     /* a device with an account: the link/ routes */
    bool leaving;    /* the owner left: decline, then off */
    uint8_t offers;  /* offers since the last claim */
    uint32_t poll_ms;
    uint64_t next_ms, expires_ms;
    char code[PET_LINK_CODE_LENGTH + 1], account[81], error[33];
    char check[PET_LINK_CHECK_LENGTH + 1]; /* the claim's number: shown, then sent with Link */
    char device[7];  /* the device ID's last six characters, for the screen */
} pet_link_t;

/* Ask for a code. A question already waiting stays until it is answered. */
void pet_link_start(pet_link_t *link, bool moving, uint64_t now_ms);
/* The owner's answer: Link or Move (yes), or Not me. False without a question,
 * and for Move without the claim's three-digit number. */
bool pet_link_tap(pet_link_t *link, bool yes, uint64_t now_ms);
/* The owner left the link screen: a moving device declines its code. */
void pet_link_leave(pet_link_t *link, uint64_t now_ms);
/* The call due now, or PET_LINK_NONE. A code past its time is replaced. */
pet_link_call_t pet_link_next(pet_link_t *link, uint64_t now_ms);
/* When pet_link_next() has a call next, for the worker's wait. */
uint64_t pet_link_due(const pet_link_t *link);
void pet_link_answer(pet_link_t *link, pet_link_call_t call, const pet_link_answer_t *answer, uint64_t now_ms);
/* Calls are coming: the worker wakes for them. */
bool pet_link_active(const pet_link_t *link);
/* "2026-09-25T14:52:00.000Z" as seconds since 1970. UTC ("Z") only. */
bool pet_link_utc(const char *text, int64_t *seconds);
