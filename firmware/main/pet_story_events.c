#include "pet_story_events.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "frame_player.h"
#include "mbedtls/sha256.h"

#define TRIES 3u
/* The listed events, and one cued or showing that the list has dropped. */
#define ENTRIES (PET_STORY_EVENTS + 1)
/* The check's workspace and each pack start on a 16-byte boundary. */
#define ALIGN 16u
#define RANGE_BYTES 32768u

static const char *TAG = "pet_story_events";

typedef struct
{
    pet_story_event_t event; /* An empty id: a free entry. */
    uint8_t *pack;           /* Reserved in the region until released, including partial downloads. */
    uint32_t downloaded;
    bool ready;
    uint8_t failures;        /* Downloads failed since the list last named it. */
    bool refused;            /* Its pack failed the check, which it always will. */
    bool roomless;           /* Said once that its pack doesn't fit the region. */
    uint64_t retry_at_ms;
} entry_t;

typedef struct
{
    pet_story_events_t wanted;
    entry_t entries[ENTRIES];
    int held; /* The cued or showing entry, or -1. */
    bool showing;
    /* The check's workspace, then the packs (pet_story_events_region). */
    uint8_t *region;
    size_t region_bytes;
} cache_t;

/* In PSRAM, under s_lock. */
static cache_t *s_cache;
static SemaphoreHandle_t s_lock;
static esp_err_t (*s_ended)(const char *id);

typedef enum
{
    CHECK_OK,
    CHECK_RETRY,
    CHECK_REFUSED
} check_t;

bool pet_story_events_init(esp_err_t (*ended)(const char *id))
{
    s_ended = ended;
    s_lock = xSemaphoreCreateMutex();
    s_cache = s_lock ? heap_caps_calloc(1, sizeof(*s_cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (s_cache)
        s_cache->held = -1;
    return s_cache != NULL;
}

static size_t aligned(size_t bytes)
{
    return (bytes + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

/* Lets the cued or showing entry go: `id` is its event's id, for `ended`, or
 * empty when there is none. Under s_lock. */
static void release(char id[PET_STORY_EVENT_ID_BYTES + 1])
{
    id[0] = 0;
    if (s_cache->held >= 0)
        strcpy(id, s_cache->entries[s_cache->held].event.id);
    s_cache->held = -1;
    s_cache->showing = false;
}

/* Without s_lock: the cue `id` let go has ended. */
static void report(const char *id)
{
    if (id[0] && s_ended)
        s_ended(id);
}

static bool same(const pet_story_event_t *a, const pet_story_event_t *b)
{
    return !strcmp(a->id, b->id) && !strcmp(a->sha256, b->sha256) && a->bytes == b->bytes;
}

/* The entry kept for `event`, or -1. Under s_lock. */
static int find(const pet_story_event_t *event)
{
    for (int i = 0; i < ENTRIES; ++i)
        if (s_cache->entries[i].event.id[0] && same(&s_cache->entries[i].event, event))
            return i;
    return -1;
}

/* The event the list names with these bytes, or NULL. */
static const pet_story_event_t *listed(const pet_story_event_t *event)
{
    for (uint8_t i = 0; i < s_cache->wanted.count; ++i)
        if (same(&s_cache->wanted.events[i], event))
            return &s_cache->wanted.events[i];
    return NULL;
}

void pet_story_events_region(uint8_t *base, size_t bytes)
{
    char dropped[PET_STORY_EVENT_ID_BYTES + 1];
    const size_t skip = base ? aligned((uintptr_t)base) - (uintptr_t)base : 0;
    if (!s_cache)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    release(dropped);
    for (int i = 0; i < ENTRIES; ++i)
    {
        s_cache->entries[i].pack = NULL;
        s_cache->entries[i].downloaded = 0;
        s_cache->entries[i].ready = false;
        s_cache->entries[i].roomless = false;
    }
    s_cache->region = base && bytes > skip ? base + skip : NULL;
    s_cache->region_bytes = s_cache->region ? bytes - skip : 0;
    xSemaphoreGive(s_lock);
    report(dropped);
}

void pet_story_events_want(const pet_story_events_t *events)
{
    if (!s_cache)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cache->wanted = *events;
    /* Named again: a download that failed is tried again, with the stinger
     * named now. */
    for (int i = 0; i < ENTRIES; ++i)
    {
        const pet_story_event_t *named = listed(&s_cache->entries[i].event);
        if (named)
        {
            s_cache->entries[i].failures = 0;
            strcpy(s_cache->entries[i].event.sound, named->sound);
        }
    }
    xSemaphoreGive(s_lock);
}

bool pet_story_events_cue(const char *id)
{
    char dropped[PET_STORY_EVENT_ID_BYTES + 1] = "";
    bool cued = false;
    if (!s_cache)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_cache->showing)
    {
        /* A cue still waiting for the speech is replaced, and ends. */
        release(dropped);
        for (uint8_t i = 0; i < s_cache->wanted.count; ++i)
        {
            int entry = find(&s_cache->wanted.events[i]);
            if (!strcmp(s_cache->wanted.events[i].id, id) && entry >= 0 && s_cache->entries[entry].ready)
                s_cache->held = entry;
        }
        cued = s_cache->held >= 0;
    }
    xSemaphoreGive(s_lock);
    report(dropped);
    return cued;
}

bool pet_story_events_take(const void **pack, size_t *bytes, char sound[PET_SOUND_ID_BYTES + 1])
{
    bool taken = false;
    if (!s_cache)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cache->held >= 0 && !s_cache->showing)
    {
        *pack = s_cache->entries[s_cache->held].pack;
        *bytes = s_cache->entries[s_cache->held].event.bytes;
        strcpy(sound, s_cache->entries[s_cache->held].event.sound);
        s_cache->showing = taken = true;
    }
    xSemaphoreGive(s_lock);
    return taken;
}

bool pet_story_events_showing(void)
{
    bool showing = false;
    if (!s_cache)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    showing = s_cache->showing;
    xSemaphoreGive(s_lock);
    return showing;
}

bool pet_story_events_uncue(void)
{
    char dropped[PET_STORY_EVENT_ID_BYTES + 1] = "";
    bool showing = false;
    if (!s_cache)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    showing = s_cache->showing;
    if (!showing)
        release(dropped);
    xSemaphoreGive(s_lock);
    report(dropped);
    return showing;
}

void pet_story_events_done(void)
{
    char ended[PET_STORY_EVENT_ID_BYTES + 1];
    if (!s_cache)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    release(ended);
    xSemaphoreGive(s_lock);
    report(ended);
}

/* Forgets what the list no longer names, but the held entry, and gives each
 * newly named event an empty entry. Under s_lock. */
static void reconcile(void)
{
    for (int i = 0; i < ENTRIES; ++i)
    {
        entry_t *entry = &s_cache->entries[i];
        if (entry->event.id[0] && i != s_cache->held && !listed(&entry->event))
            memset(entry, 0, sizeof(*entry));
    }
    for (uint8_t w = 0; w < s_cache->wanted.count; ++w)
    {
        int empty = 0;
        while (empty < ENTRIES && s_cache->entries[empty].event.id[0])
            ++empty;
        if (empty < ENTRIES && find(&s_cache->wanted.events[w]) < 0)
            s_cache->entries[empty].event = s_cache->wanted.events[w];
    }
}

/* Whether `bytes` at `at` in the region overlap a pack kept. Under s_lock. */
static bool taken(size_t at, size_t bytes)
{
    for (int i = 0; i < ENTRIES; ++i)
    {
        const entry_t *entry = &s_cache->entries[i];
        const size_t start = entry->pack ? (size_t)(entry->pack - s_cache->region) : 0;
        if (entry->pack && at < start + aligned(entry->event.bytes) && start < at + bytes)
            return true;
    }
    return false;
}

/* Where a pack of `bytes` goes: the first gap in the region, past the check's
 * workspace, that holds it beside the packs kept; NULL when none does. A first
 * gap starts after the workspace or after a pack. Under s_lock. */
static uint8_t *place(uint32_t bytes)
{
    const size_t need = aligned(bytes);
    size_t first = SIZE_MAX;
    for (int i = -1; i < ENTRIES; ++i)
    {
        const entry_t *before = i >= 0 ? &s_cache->entries[i] : NULL;
        if (before && !before->pack)
            continue;
        const size_t at = before ? (size_t)(before->pack - s_cache->region) + aligned(before->event.bytes)
                                 : fp_validation_workspace_size();
        if (at < first && at + need <= s_cache->region_bytes && !taken(at, need))
            first = at;
    }
    return first == SIZE_MAX ? NULL : s_cache->region + first;
}

/* The entry to download now, in the list's order (the event the story heads
 * for comes first), and where its pack goes; or NULL: neither ready, refused
 * nor given up, its retry time come, and room for it in the region. One with
 * no room waits, and that is said once. Under s_lock. */
static entry_t *next_due(uint64_t now_ms, uint8_t **at)
{
    for (uint8_t w = 0; w < s_cache->wanted.count; ++w)
    {
        int i = find(&s_cache->wanted.events[w]);
        entry_t *entry = i >= 0 ? &s_cache->entries[i] : NULL;
        if (!entry || entry->ready || entry->refused || entry->failures >= TRIES || now_ms < entry->retry_at_ms)
            continue;
        *at = entry->pack ? entry->pack : place(entry->event.bytes);
        if (*at)
        {
            entry->pack = *at;
            return entry;
        }
        if (s_cache->region && !entry->roomless)
        {
            entry->roomless = true;
            ESP_LOGW(TAG, "story event %s waits: no room for %lu bytes beside the pet", entry->event.id,
                     (unsigned long)entry->event.bytes);
        }
    }
    return NULL;
}

/* The whole pack is exactly the one the session named, and an event's,
 * checked in the region's workspace. */
static check_t check(const uint8_t *pack, const pet_story_event_t *event, uint8_t *workspace)
{
    uint8_t digest[32];
    char hex[65];
    fp_pack_info_t info = {0};
    if (mbedtls_sha256(pack, event->bytes, digest, 0))
        return CHECK_RETRY;
    for (int i = 0; i < 32; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (strcmp(hex, event->sha256))
        return CHECK_REFUSED;
    const uint32_t room = fp_validation_workspace_size();
    fp_error_t result = fp_validate_with_workspace(pack, event->bytes, &info, workspace, room);
    return result == FP_OK && info.format_version == 2 && info.width == 240 && info.height == 240 && !info.move &&
                   info.id_len > 6 && !memcmp(info.id, "event-", 6)
               ? CHECK_OK
               : CHECK_REFUSED;
}

bool pet_story_events_step(const pet_control_http_t *http, uint64_t now_ms)
{
    uint8_t *pack = NULL;
    if (!s_cache)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reconcile();
    entry_t *entry = next_due(now_ms, &pack);
    uint8_t *workspace = s_cache->region;
    xSemaphoreGive(s_lock);
    if (!entry)
        return false;
    /* One bounded Range per control pass. Partial bytes cannot be cued and
     * remain reserved until completion or the next list/region drops them. */
    const pet_story_event_t *event = &entry->event;
    uint32_t offset = entry->downloaded;
    uint32_t bytes = event->bytes - offset;
    if (bytes > RANGE_BYTES) bytes = RANGE_BYTES;
    pet_control_http_result_t result = {.retry_seconds = 30};
    bool received =
        pet_control_http_event_range(http, event->sha256, event->bytes, offset, pack + offset, bytes, &result);
    bool complete = received && offset + bytes == event->bytes;
    check_t checked = complete ? check(pack, event, workspace) : CHECK_RETRY;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (received && !complete) entry->downloaded += bytes;
    else if (checked == CHECK_OK)
    {
        entry->downloaded = event->bytes;
        entry->ready = true;
    }
    else if (checked == CHECK_REFUSED)
    {
        entry->refused = true;
        entry->pack = NULL;
        entry->downloaded = 0;
    }
    else
    {
        if (complete) entry->downloaded = 0;
        ++entry->failures;
        if (entry->failures >= TRIES)
        {
            entry->pack = NULL;
            entry->downloaded = 0;
        }
        entry->retry_at_ms = now_ms + result.retry_seconds * 1000ull;
    }
    xSemaphoreGive(s_lock);
    if (checked == CHECK_OK)
        ESP_LOGI(TAG, "story event %s ready: %lu bytes", event->id, (unsigned long)event->bytes);
    else if (!received || complete)
        ESP_LOGW(TAG, "story event %s %s", event->id, checked == CHECK_REFUSED ? "refused" : "not downloaded");
    return true;
}
