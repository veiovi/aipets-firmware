#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_control_http.h"
#include "pet_session_wire.h"

/* Story events (story-event-v1) on the Pocket Terminal. story.events names
 * the events the story can reach next, whole each time. Between turns the
 * control task downloads each one's frame pack from the device's own origin
 * and checks it whole, in the region the runtime gives it: the unused tail of
 * the pet's resident PSRAM buffer, never the heap, a pet slot, the journal
 * or NVS. story.event cues one, and the runtime shows it once the speech has
 * drained (pet_face_pack_play_event). Each cue taken ends once, with `ended`
 * (story-reaction-v1): shown to its end, ended early or dropped unshown; the
 * runtime reports a cue it ignores itself. One lock guards the cache; it is
 * never held over the network or while `ended` runs. Without
 * pet_story_events_init (no PSRAM for the cache) nothing is kept: every event
 * is narration. */
bool pet_story_events_init(esp_err_t (*ended)(const char *id));
/* Control task: the region packs and their check's workspace go into, or
 * NULL while the copy changes. Each call forgets every pack, so the events
 * still listed download again into the next region, and drops a cue, which
 * ends. Only while no event shows: pet_face_pack_hold_external ends one. */
void pet_story_events_region(uint8_t *base, size_t bytes);
/* Network task: the events story.events listed. The packs of events no longer
 * listed are forgotten on the next step, unless cued or showing; failed
 * downloads of the events listed again are tried again. */
void pet_story_events_want(const pet_story_events_t *events);
/* Network task: story.event. True when `id` is listed and its pack is ready;
 * it is then the cued event until taken, dropped or replaced by the next cue,
 * which ends it. Never while an event shows. */
bool pet_story_events_cue(const char *id);
/* The cued event's pack, which then shows until pet_story_events_done, and
 * the stinger that starts with it (sound-bank-v1), or an empty `sound`. */
bool pet_story_events_take(const void **pack, size_t *bytes, char sound[PET_SOUND_ID_BYTES + 1]);
/* An event shows (taken, not done yet). */
bool pet_story_events_showing(void);
/* Drops a cue, which ends. True while an event shows, which only its done
 * ends. */
bool pet_story_events_uncue(void);
/* The event that showed has ended: nothing reads its pack any more. */
void pet_story_events_done(void);
/* Control task, between turns: forgets what is no longer listed, then
 * downloads the first listed pack still missing that fits in the region past
 * the check's workspace (fp_validation_workspace_size), in 32 KiB Range chunks, into the first gap that holds it.
 * Partial packs reserve their space but cannot be cued. One that doesn't fit waits as
 * narration until the region or the list makes room, said once. The pack must
 * be exactly its size and sha256, valid in full (fp_validate_with_workspace),
 * format 2, 240x240, not a move pack, with an id starting "event-". Approval
 * is not asked, and only here: an event pack is a compiler candidate whose
 * sha256 the authenticated session gave. A pack that fails the check is
 * refused for good. A failed download is tried again after the server's retry
 * delay, three times, and again whenever story.events lists the event anew.
 * True when it downloaded a pack, or tried to. */
bool pet_story_events_step(const pet_control_http_t *http, uint64_t now_ms);
