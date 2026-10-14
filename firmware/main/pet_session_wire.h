#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "pet_control_wire.h"

/* In-session rebind (cloud device-protocol `session-rebind-v1`): a
 * three-pet swipe moves the open, bound
 * conversation to the newly selected pet instead of opening a new socket. */
#define PET_SESSION_REBIND_CAPABILITY "session-rebind-v1"
/* session.error code of a refused rebind; the gateway then closes the socket. */
#define PET_SESSION_REBIND_REFUSED "REBIND_REFUSED"
#define PET_SESSION_WIRE_REBIND_MAX 640
/* Long-press stories (cloud device-protocol STORY_CAPABILITY): with a gateway
 * that lists it, a long press sends `input.story` and the pet tells what is
 * happening in its own world, a turn without the microphone. */
#define PET_STORY_CAPABILITY "story-v1"
/* The quest log (cloud device-protocol STORY_LOG_CAPABILITY), offered only
 * beside story-v1: a gateway that lists it sends `story.log`, the pet's whole
 * log, oldest line first; an empty list clears it. Each line is 1 to 90
 * UTF-8 bytes. */
#define PET_STORY_LOG_CAPABILITY "story-log-v1"
#define PET_STORY_LOG_LINES 20
#define PET_STORY_LOG_LINE_BYTES 90
/* Story mode (cloud device-protocol STORY_MODE_CAPABILITY), offered beside
 * story-v1. With a gateway that lists it, a long press enters or resumes story
 * mode (input.story) and, in story mode, pauses it (story.pause); the deck
 * loads another story (story.load) or starts this one over (story.restart).
 * The gateway sends story.state and story.list, each a whole replacement. */
#define PET_STORY_MODE_CAPABILITY "story-mode-v1"
#define PET_STORY_TITLE_BYTES 40
#define PET_STORY_GOAL_BYTES 60
#define PET_STORY_MILESTONES 8
#define PET_STORY_LIST_LENGTH 8
#define PET_STORY_LIST_TITLE_BYTES 32
#define PET_STORY_ID_BYTES 36
/* session.error code: the cloud could not load or restart a story. */
#define PET_STORY_UNAVAILABLE "STORY_UNAVAILABLE"
/* Story games (cloud device-protocol STORY_GAME_CAPABILITY), offered beside
 * story-mode-v1. With a gateway that lists it, story.state may carry the
 * hero, its conditions and its blessings, story.atmosphere names the sound
 * loop under the next story turn, and the owner uses a blessing with
 * input.blessing, a turn without the microphone like input.story. */
#define PET_STORY_GAME_CAPABILITY "story-game-v1"
#define PET_STORY_CONDITIONS 4
#define PET_STORY_BLESSINGS 9
#define PET_STORY_NAME_BYTES 24
#define PET_STORY_BLESSING_ID_BYTES 24
/* session.error code: the blessing can't be used now (spent, or no game). */
#define PET_BLESSING_UNAVAILABLE "BLESSING_UNAVAILABLE"
/* Blessings that take the owner's words, and every blessing icon (cloud
 * device-protocol STORY_BLESSINGS_CAPABILITY), offered beside story-game-v1.
 * With a gateway that lists it, a story.state blessing may have any of the
 * icons and `speak`, and the owner uses such a blessing by recording what
 * they say: input.audio.start with the blessing's id. */
#define PET_STORY_BLESSINGS_CAPABILITY "story-blessings-v2"
/* Whether the pet is on its path (cloud device-protocol STORY_PACING_CAPABILITY), offered beside story-mode-v1.
 * With a gateway that lists it, story.state may carry path "story" or "detour", which colours the story frame. */
#define PET_STORY_PACING_CAPABILITY "story-pacing-v1"
/* Hurrying back to the story (cloud story-hurry-v1), offered beside
 * story-pacing-v1. With a gateway that lists it, story.state's path may also
 * be "hurry". */
#define PET_STORY_HURRY_CAPABILITY "story-hurry-v1"
/* The blessing icons, in the protocol's order (STORY_BLESSING_ICONS). */
typedef enum {
    PET_BLESSING_HEART,
    PET_BLESSING_STORM,
    PET_BLESSING_SHIELD,
    PET_BLESSING_STAR,
    PET_BLESSING_FLAME,
    PET_BLESSING_LEAF,
    PET_BLESSING_MOON,
    PET_BLESSING_EYE,
    PET_BLESSING_KEY,
    PET_BLESSING_FEATHER,
    PET_BLESSING_POTION,
    PET_BLESSING_SWORD,
    /* story-blessings-v2 */
    PET_BLESSING_MASK,
    PET_BLESSING_HAND,
    PET_BLESSING_BUBBLE,
    PET_BLESSING_SKULL,
    PET_BLESSING_CROWN,
    PET_BLESSING_COIN,
    PET_BLESSING_RING,
    PET_BLESSING_SCROLL,
    PET_BLESSING_BELL,
    PET_BLESSING_HOURGLASS,
    PET_BLESSING_SUN,
    PET_BLESSING_CLOUD,
    PET_BLESSING_DROP,
    PET_BLESSING_PICK,
    PET_BLESSING_BOW,
    PET_BLESSING_LANTERN,
    PET_BLESSING_GEM,
    PET_BLESSING_GHOST,
    PET_BLESSING_PAW,
    PET_BLESSING_GEAR,
    PET_BLESSING_ICONS,
} pet_blessing_icon_t;
typedef struct {
    char id[PET_STORY_BLESSING_ID_BYTES + 1];
    char name[PET_STORY_NAME_BYTES + 1];
    uint8_t icon; /* pet_blessing_icon_t */
    uint8_t uses; /* 0: spent */
    bool speak;   /* takes the owner's words (story-blessings-v2) */
} pet_story_blessing_t;
/* story.state: the mode, and the story's title, the next milestone's place
 * (goal) and the milestones reached of all of them. The title and goal are
 * empty without a story. A story played as a game (story-game-v1) adds its
 * hero and blessings, each only when sent, and up to four conditions. */
typedef struct {
    bool story;  /* "story"; false while "paused" */
    bool detour; /* path "detour" (story-pacing-v1); false for "story" or none */
    bool hurry;  /* path "hurry" (story-hurry-v1) */
    char title[PET_STORY_TITLE_BYTES + 1];
    char goal[PET_STORY_GOAL_BYTES + 1];
    uint8_t reached, of;
    bool has_hero;
    struct {
        uint8_t health, max_health, attack, defense, willpower;
    } hero;
    uint8_t condition_count;
    char conditions[PET_STORY_CONDITIONS][PET_STORY_NAME_BYTES + 1];
    bool has_blessings;
    uint8_t blessing_count;
    pet_story_blessing_t blessings[PET_STORY_BLESSINGS];
} pet_story_state_t;
/* story.list: the owner's stories, newest first, and the pet's current one
 * (empty for none). */
typedef struct {
    uint8_t count;
    struct {
        char id[PET_STORY_ID_BYTES + 1];
        char title[PET_STORY_LIST_TITLE_BYTES + 1];
    } stories[PET_STORY_LIST_LENGTH];
    char current[PET_STORY_ID_BYTES + 1];
} pet_story_list_t;
/* Story events (cloud device-protocol STORY_EVENT_CAPABILITY), offered beside
 * story-mode-v1. A gateway that lists it sends story.events, the events the
 * story can reach next, whole each time, and story.event once a milestone's
 * speech has ended: the device then shows that event's animation, a frame
 * pack of at most 250 KiB, and the pet again (pet_story_events.h). Beside it
 * the Pocket offers story-events-all-v1, which needs no echo: it takes lists
 * of up to 16 events and 4,096,000 bytes and keeps as many packs as fit beside
 * the pet on screen, in the list's order. */
#define PET_STORY_EVENT_CAPABILITY "story-event-v1"
#define PET_STORY_EVENTS_ALL_CAPABILITY "story-events-all-v1"
#define PET_STORY_EVENTS 16
#define PET_STORY_EVENT_ID_BYTES 40
#define PET_STORY_EVENT_BYTES 256000u
#define PET_STORY_EVENTS_BYTES 4096000u
/* The pet's reaction to a story event (cloud device-protocol
 * STORY_REACTION_CAPABILITY), offered beside story-event-v1. A gateway that
 * lists it holds the reaction until the device sends story.event.ended for the
 * cue (the animation played to its end, was tapped away, or never played),
 * then speaks it as another reply. */
#define PET_STORY_REACTION_CAPABILITY "story-reaction-v1"
/* Holding a reply's speech (cloud device-protocol SPEECH_PAUSE_CAPABILITY),
 * offered by the Pocket. With a gateway that lists it, output.pause with the
 * reply's streamId holds its frames until output.resume or input.cancel; the
 * gateway's pacing goes on from where it stopped. */
#define PET_SPEECH_PAUSE_CAPABILITY "speech-pause-v1"
/* Talking over a held reply (cloud speech-pause-listen-v1), offered beside
 * speech-pause-v1. A gateway that lists it keeps a held reply when the next
 * input.audio.start comes: the reply stays held through a cancel of that
 * input or its NO_SPEECH, for output.resume, and is dropped once that input
 * has speech. input.cancel then ends only its own stream. */
#define PET_SPEECH_PAUSE_LISTEN_CAPABILITY "speech-pause-listen-v1"
/* The sound bank (cloud device-protocol sound-bank-v1), offered by the
 * Pocket. A gateway that lists it sends sound.bank, the bank to keep
 * (pet_sound_bank.h), after its hello and after a rebind; story.sound, a game
 * cue the next reply's speech waits for; an event's stinger in story.events
 * (`sound`); and a bank loop's id in story.atmosphere. Sound ids are 1 to 23
 * of a-z, 0-9 and '-'. */
#define PET_SOUND_BANK_CAPABILITY "sound-bank-v1"
#define PET_SOUND_ID_BYTES 23
#define PET_SOUND_BANK_MAX_BYTES 2097152u
/* An event's animation: the pack's sha256 (lowercase hex) and size. Its url
 * is checked but not kept: the device fetches by sha256 from its own origin.
 * `sound`: the bank one-shot that starts with it (sound-bank-v1), or empty. */
typedef struct {
    char id[PET_STORY_EVENT_ID_BYTES + 1];
    char sha256[65];
    uint32_t bytes;
    char sound[PET_SOUND_ID_BYTES + 1];
} pet_story_event_t;
typedef struct {
    uint8_t count;
    pet_story_event_t events[PET_STORY_EVENTS];
} pet_story_events_t;
#define PET_BRAIN_CAPABILITY "brain-v1"
/* Saves a Wi-Fi network sent in a management reply (device.wifi.command). */
#define PET_WIFI_COMMAND_CAPABILITY "wifi-command-v1"
/* The backend's origin, the build setting CONFIG_PET_VNEXT_CONTROL_ORIGIN:
 * "https://", a lowercase DNS name, then ":port" unless the port is 443, at
 * most 127 characters. Gives its name and port; either may be NULL. Voice
 * connects to wss://<name>[:port]/v1/device. */
bool pet_session_wire_origin(const char *origin, char *host, size_t capacity, uint16_t *port);
#define PET_BRAIN_TOKEN_MAX 51
bool pet_session_wire_brain_token(const char *token);
/* output.audio.start: mono PCM at 16 or 24 kHz, or IMA ADPCM at 16 kHz
 * (PET_IMA_ADPCM_CAPABILITY), which sets `adpcm`. */
bool pet_session_wire_audio_start(const cJSON *message, uint32_t *stream, uint32_t *rate, bool *adpcm);

bool pet_session_wire_brain_message(const char *type, bool incoming);
bool pet_session_wire_management_message(const char *type, bool incoming);
bool pet_session_wire_brain_request(const pet_control_context_t *context, char *out, size_t capacity);
/* A grant for `expected` whose endpoint is the voice address of `origin`. */
bool pet_session_wire_brain_grant(const cJSON *root, const pet_control_context_t *expected,
                                 const char *origin, char token[PET_BRAIN_TOKEN_MAX]);
bool pet_session_wire_brain_rebind(const pet_control_context_t *context, const char *token,
                                  char *out, size_t capacity);

/* device.binding.changed for `context`: its binding exactly as the bound hello
 * carries it (revision, relationshipId, buildId, sha256, configVersion), and
 * the pet's face as activeFaceId and installedFaceIds. */
bool pet_session_wire_encode_rebind(const pet_control_context_t *context, char *out, size_t capacity);
/* A gateway.hello whose `capabilities` list `capability`. */
bool pet_session_wire_gateway_offers(const cJSON *hello, const char *capability);
/* The bound device.hello's `capabilities`. A Pocket Terminal adds rebind,
 * stories, their log, story mode, games, events (all of them, and the pet's
 * reactions), its path and hurry, speech pause and talking over it, and the
 * sound bank, and says whether its pet is in story mode (`storyMode`), so a
 * reconnect resumes it. */
bool pet_session_wire_hello_capabilities(cJSON *hello, bool pocket, bool story_mode);
/* A story.log: NULL, with `lines` pointing into `message`, or why it is
 * refused, for the log. */
const char *pet_session_wire_story_log(const cJSON *message, const char *lines[PET_STORY_LOG_LINES],
                                       size_t *count);
/* A story.state or story.list, read whole into `out`: NULL, or why it is
 * refused, for the log. A refused message leaves `out` as it was. With
 * `game` (story-game-v1) a state's hero, conditions and blessings are read
 * too, and with `pacing` (story-pacing-v1) its path; without them they are
 * ignored. The path "hurry" is one only with `hurry` (story-hurry-v1). With
 * `words` (story-blessings-v2) a blessing may have any icon and `speak`. */
const char *pet_session_wire_story_state(const cJSON *message, bool game, bool words, bool pacing, bool hurry,
                                         pet_story_state_t *out);
const char *pet_session_wire_story_list(const cJSON *message, pet_story_list_t *out);
/* A story.atmosphere: NULL with `name` the loop's name, a built-in's or a
 * bank loop's id, or NULL for none, or why it is refused. */
const char *pet_session_wire_story_atmosphere(const cJSON *message, const char **name);
/* A sound.bank: NULL with the bank's sha256 (lowercase hex) and size, at
 * most PET_SOUND_BANK_MAX_BYTES, or why it is refused. Its url is checked, never followed: the
 * device fetches /v2/device/sound-bank/<sha256> from its own origin. */
const char *pet_session_wire_sound_bank(const cJSON *message, char sha256[65], uint32_t *bytes);
/* A story.sound: NULL with `id` the cue's sound id, or why it is refused. */
const char *pet_session_wire_story_sound(const cJSON *message, char id[PET_SOUND_ID_BYTES + 1]);
/* A story.events, read whole and as strictly as the cloud's schema: NULL, or
 * why it is refused, which leaves `out` as it was. A story.event: NULL with
 * `id` its event's id, or why it is refused. */
const char *pet_session_wire_story_events(const cJSON *message, pet_story_events_t *out);
const char *pet_session_wire_story_event(const cJSON *message, char id[PET_STORY_EVENT_ID_BYTES + 1]);
/* A turn without the microphone on `stream`: input.story, or input.blessing
 * with `blessing`'s id; with `ai_pet_id` when not empty, and the speech
 * profile. False for a malformed id or too small an `out`. */
bool pet_session_wire_story_turn(uint32_t stream, const char *blessing, const char *ai_pet_id,
                                 const char *speech_profile, char *out, size_t capacity);
/* input.audio.start on `stream`, a turn on the microphone: with `ai_pet_id`
 * when not empty, the speech profile when given, and `blessing`'s id when the
 * recording is the owner's words for it (story-blessings-v2). False for a
 * malformed id or too small an `out`. */
bool pet_session_wire_input_start(uint32_t stream, int64_t started_ms, const char *ai_pet_id,
                                  const char *speech_profile, const char *blessing, char *out, size_t capacity);
/* "story.pause", "story.restart", or "story.load" with `story_id` (a UUID). */
bool pet_session_wire_story_request(const char *type, const char *story_id, char *out, size_t capacity);
/* output.pause, or output.resume when not `paused`, for the reply on `stream`
 * (speech-pause-v1). False for too small an `out`. */
bool pet_session_wire_output_pause(uint32_t stream, bool paused, char *out, size_t capacity);
/* story.event.ended for the cue of event `id` (story-reaction-v1). False for
 * a malformed id or too small an `out`. */
bool pet_session_wire_story_event_ended(const char *id, char *out, size_t capacity);
/* Whether story mode carries over from the session's binding `from` to `to`.
 * It belongs to its pet (its face): a new binding of the same pet keeps it, as
 * a renewed grant's rebind about every 14 minutes or a reconnect, and another
 * pet ends it, as the gateway does. */
bool pet_session_wire_story_mode_kept(bool story_mode, const pet_control_context_t *from,
                                      const pet_control_context_t *to);
/* A gateway.binding.accepted naming exactly `expected`'s binding and
 * configuration version. */
bool pet_session_wire_accepted(const cJSON *message, const pet_control_context_t *expected);
