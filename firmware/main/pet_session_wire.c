#include "pet_session_wire.h"
#include "pet_ima_adpcm.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

bool pet_session_wire_origin(const char *origin, char *host, size_t capacity, uint16_t *port)
{
    /* 127 characters fit the setup, control and gateway buffers. */
    if (!origin || strncmp(origin, "https://", 8) || strlen(origin) > 127)
        return false;
    const char *name = origin + 8;
    size_t length = strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789.-");
    unsigned long number = 443;
    if (!length || (name[length] && name[length] != ':'))
        return false;
    if (name[length] == ':')
    {
        /* As a URL writes it: no leading zero, and no ":443". */
        const char *digits = name + length + 1;
        char *end = NULL;
        if (*digits >= '1' && *digits <= '9')
            number = strtoul(digits, &end, 10);
        if (!end || *end || number > 65535 || number == 443)
            return false;
    }
    if (host)
    {
        if (length >= capacity)
            return false;
        memcpy(host, name, length);
        host[length] = 0;
    }
    if (port)
        *port = (uint16_t)number;
    return true;
}

static bool listed(const char *type, const char *const *names, size_t count)
{
    if (!type) return false;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(type, names[i])) return true;
    return false;
}

bool pet_session_wire_brain_message(const char *type, bool incoming)
{
    static const char *const input[] = {"gateway.hello", "gateway.binding.accepted", "pet.state",
        "turn.text", "pet.expression", "pet.gesture", "output.audio.start", "output.audio.end",
        "story.log", "story.state", "story.list", "story.atmosphere", "story.events", "story.event", "session.error",
        "sound.bank", "story.sound", "ping", "pong"};
    static const char *const output[] = {"device.hello", "device.binding.changed",
        "input.audio.start", "input.audio.end", "input.cancel", "input.story", "input.blessing", "story.pause",
        "story.load", "story.restart", "output.pause", "output.resume", "story.event.ended",
        "ping", "pong"};
    return incoming ? listed(type, input, sizeof(input) / sizeof(input[0])) :
                      listed(type, output, sizeof(output) / sizeof(output[0]));
}

bool pet_session_wire_management_message(const char *type, bool incoming)
{
    static const char *const input[] = {"pet.config.desired", "pet.speech.profile", "pet.crash.acknowledged",
        "device.wifi.command"};
    static const char *const output[] = {"device.telemetry", "device.config.applied",
        "device.config.rejected", "device.speech.profile.confirmed", "device.wifi.result"};
    return incoming ? listed(type, input, sizeof(input) / sizeof(input[0])) :
                      listed(type, output, sizeof(output) / sizeof(output[0]));
}

bool pet_session_wire_brain_token(const char *token)
{
    if (!token || strlen(token) != PET_BRAIN_TOKEN_MAX - 1 || strncmp(token, "brain1_", 7)) return false;
    for (size_t i = 7; token[i]; ++i)
    {
        char c = token[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '_' && c != '-') return false;
    }
    return true;
}

bool pet_session_wire_brain_request(const pet_control_context_t *context, char *out, size_t capacity)
{
    char rebind[PET_SESSION_WIRE_REBIND_MAX];
    if (!pet_session_wire_encode_rebind(context, rebind, sizeof(rebind))) return false;
    cJSON *source = cJSON_Parse(rebind), *request = cJSON_CreateObject();
    cJSON *binding = source ? cJSON_DetachItemFromObjectCaseSensitive(source, "binding") : NULL;
    bool ok = request && binding && cJSON_AddItemToObject(request, "binding", binding);
    if (!ok) cJSON_Delete(binding);
    ok = ok && out && capacity <= 0x7fffffff && cJSON_PrintPreallocated(request, out, (int)capacity, false);
    cJSON_Delete(source);
    cJSON_Delete(request);
    return ok;
}

bool pet_session_wire_brain_rebind(const pet_control_context_t *context, const char *token,
                                  char *out, size_t capacity)
{
    if (!pet_session_wire_brain_token(token) || !pet_session_wire_encode_rebind(context, out, capacity)) return false;
    cJSON *root = cJSON_Parse(out);
    bool ok = root && cJSON_AddStringToObject(root, "token", token) &&
        cJSON_PrintPreallocated(root, out, (int)capacity, false);
    cJSON_Delete(root);
    return ok;
}

bool pet_session_wire_encode_rebind(const pet_control_context_t *c, char *out, size_t capacity)
{
    if (!c || !out || !capacity || capacity > 0x7fffffff || !c->binding.assigned || !c->config.face_id[0] ||
        !c->config.version[0]) return false;
    cJSON *root = cJSON_CreateObject(), *binding = NULL, *installed = NULL, *face = NULL;
    bool ok = root && cJSON_AddNumberToObject(root, "v", 1) &&
        cJSON_AddStringToObject(root, "type", "device.binding.changed") &&
        (binding = cJSON_AddObjectToObject(root, "binding")) &&
        cJSON_AddStringToObject(binding, "revision", c->binding.revision) &&
        cJSON_AddStringToObject(binding, "relationshipId", c->binding.relationship_id) &&
        cJSON_AddStringToObject(binding, "buildId", c->binding.build_id) &&
        cJSON_AddStringToObject(binding, "sha256", c->binding.sha256) &&
        cJSON_AddStringToObject(binding, "configVersion", c->config.version) &&
        cJSON_AddStringToObject(root, "activeFaceId", c->config.face_id) &&
        (installed = cJSON_AddArrayToObject(root, "installedFaceIds")) &&
        (face = cJSON_CreateString(c->config.face_id)) != NULL;
    if (ok && !cJSON_AddItemToArray(installed, face)) { cJSON_Delete(face); ok = false; }
    else if (!ok) cJSON_Delete(face);
    ok = ok && cJSON_PrintPreallocated(root, out, (int)capacity, false);
    cJSON_Delete(root);
    return ok;
}

bool pet_session_wire_gateway_offers(const cJSON *hello, const char *capability)
{
    const cJSON *list = hello ? cJSON_GetObjectItemCaseSensitive(hello, "capabilities") : NULL, *item = NULL;
    if (!capability || !cJSON_IsArray(list)) return false;
    cJSON_ArrayForEach(item, list)
        if (cJSON_IsString(item) && !strcmp(item->valuestring, capability)) return true;
    return false;
}

bool pet_session_wire_hello_capabilities(cJSON *hello, bool pocket, bool story_mode)
{
    static const char *const common[] = {"audio-in", "audio-out", "touch", "gestures", "session-binding-v1",
        PET_BRAIN_CAPABILITY, "text-v1", PET_IMA_ADPCM_CAPABILITY, PET_WIFI_COMMAND_CAPABILITY};
    static const char *const stories[] = {PET_SESSION_REBIND_CAPABILITY, PET_STORY_CAPABILITY,
        PET_STORY_LOG_CAPABILITY, PET_STORY_MODE_CAPABILITY, PET_STORY_GAME_CAPABILITY, PET_STORY_EVENT_CAPABILITY,
        PET_STORY_EVENTS_ALL_CAPABILITY, PET_STORY_REACTION_CAPABILITY, PET_STORY_PACING_CAPABILITY,
        PET_STORY_HURRY_CAPABILITY, PET_SPEECH_PAUSE_CAPABILITY, PET_SPEECH_PAUSE_LISTEN_CAPABILITY,
        PET_SOUND_BANK_CAPABILITY, PET_STORY_BLESSINGS_CAPABILITY};
    cJSON *list = hello ? cJSON_AddArrayToObject(hello, "capabilities") : NULL;
    bool ok = list != NULL;
    for (size_t i = 0; ok && i < sizeof(common) / sizeof(common[0]); ++i)
        ok = cJSON_AddItemToArray(list, cJSON_CreateString(common[i]));
    for (size_t i = 0; ok && pocket && i < sizeof(stories) / sizeof(stories[0]); ++i)
        ok = cJSON_AddItemToArray(list, cJSON_CreateString(stories[i]));
    if (ok && pocket)
        ok = cJSON_AddBoolToObject(hello, "storyMode", story_mode) != NULL;
    return ok;
}

const char *pet_session_wire_story_log(const cJSON *message, const char *lines[PET_STORY_LOG_LINES],
                                       size_t *count)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(message, "v");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(message, "lines");
    const cJSON *line = NULL;
    if (!cJSON_IsNumber(version) || version->valuedouble != 1)
        return "STORY_LOG_VERSION";
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > PET_STORY_LOG_LINES)
        return "STORY_LOG_LINES";
    size_t n = 0;
    cJSON_ArrayForEach(line, list)
    {
        size_t bytes = cJSON_IsString(line) ? strlen(line->valuestring) : 0;
        if (!bytes || bytes > PET_STORY_LOG_LINE_BYTES)
            return "STORY_LOG_LINE";
        lines[n++] = line->valuestring;
    }
    *count = n;
    return NULL;
}

static bool version_one(const cJSON *message)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(message, "v");
    return cJSON_IsNumber(version) && version->valuedouble == 1;
}

/* A string of 1 to `bytes` UTF-8 bytes, copied into `out`. */
static bool text_field(const cJSON *value, size_t bytes, char *out)
{
    const size_t length = cJSON_IsString(value) ? strlen(value->valuestring) : 0;
    if (!length || length > bytes)
        return false;
    memcpy(out, value->valuestring, length + 1);
    return true;
}

/* A whole number from `low` to `high`. */
static bool whole(const cJSON *value, int low, int high, uint8_t *out)
{
    if (!cJSON_IsNumber(value) || value->valuedouble < low || value->valuedouble > high ||
        value->valuedouble != (double)(int)value->valuedouble)
        return false;
    *out = (uint8_t)value->valuedouble;
    return true;
}

/* A UUID as the cloud's schema takes it: 8-4-4-4-12 hex digits. */
static bool uuid(const char *s)
{
    if (!s || strlen(s) != PET_STORY_ID_BYTES)
        return false;
    for (size_t i = 0; i < PET_STORY_ID_BYTES; ++i)
    {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !isxdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

/* An object of exactly `keys` members, as the cloud's strict schemas have it. */
static bool exactly(const cJSON *object, int keys)
{
    return cJSON_IsObject(object) && cJSON_GetArraySize(object) == keys;
}

/* A blessing's or an event's id: 1 to `bytes` of a-z, 0-9 and '-'. */
static bool slug(const char *id, size_t bytes)
{
    const size_t length = id ? strlen(id) : 0;
    return length && length <= bytes && strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") == length;
}

/* The hero's sheet: exactly its five numbers, its health at most its maximum. */
static bool read_hero(const cJSON *hero, pet_story_state_t *state)
{
    if (!exactly(hero, 5) ||
        !whole(cJSON_GetObjectItemCaseSensitive(hero, "health"), 0, 100, &state->hero.health) ||
        !whole(cJSON_GetObjectItemCaseSensitive(hero, "maxHealth"), 1, 100, &state->hero.max_health) ||
        !whole(cJSON_GetObjectItemCaseSensitive(hero, "attack"), 0, 20, &state->hero.attack) ||
        !whole(cJSON_GetObjectItemCaseSensitive(hero, "defense"), 0, 20, &state->hero.defense) ||
        !whole(cJSON_GetObjectItemCaseSensitive(hero, "willpower"), 0, 20, &state->hero.willpower))
        return false;
    state->has_hero = true;
    return state->hero.health <= state->hero.max_health;
}

static bool read_conditions(const cJSON *conditions, pet_story_state_t *state)
{
    const cJSON *condition = NULL;
    if (!cJSON_IsArray(conditions) || cJSON_GetArraySize(conditions) > PET_STORY_CONDITIONS)
        return false;
    cJSON_ArrayForEach(condition, conditions)
    {
        char *name = state->conditions[state->condition_count];
        if (!exactly(condition, 1) ||
            !text_field(cJSON_GetObjectItemCaseSensitive(condition, "name"), PET_STORY_NAME_BYTES, name))
            return false;
        ++state->condition_count;
    }
    return true;
}

/* A story.state's blessings. With `words` (story-blessings-v2) a blessing may
 * have any icon, and `speak`, which is only ever true; without it, the first
 * twelve icons and exactly four members. */
static bool read_blessings(const cJSON *blessings, bool words, pet_story_state_t *state)
{
    static const char *const icons[PET_BLESSING_ICONS] = {"heart", "storm", "shield", "star", "flame", "leaf",
        "moon", "eye", "key", "feather", "potion", "sword", "mask", "hand", "bubble", "skull", "crown", "coin", "ring",
        "scroll", "bell", "hourglass", "sun", "cloud", "drop", "pick", "bow", "lantern", "gem", "ghost", "paw", "gear"};
    const uint8_t known = words ? PET_BLESSING_ICONS : PET_BLESSING_MASK;
    const cJSON *item = NULL;
    if (!cJSON_IsArray(blessings) || cJSON_GetArraySize(blessings) > PET_STORY_BLESSINGS)
        return false;
    cJSON_ArrayForEach(item, blessings)
    {
        pet_story_blessing_t *blessing = &state->blessings[state->blessing_count];
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *icon = cJSON_GetObjectItemCaseSensitive(item, "icon");
        const cJSON *speak = cJSON_GetObjectItemCaseSensitive(item, "speak");
        blessing->speak = words && cJSON_IsTrue(speak);
        if (!exactly(item, blessing->speak ? 5 : 4) || !cJSON_IsString(id) ||
            !slug(id->valuestring, PET_STORY_BLESSING_ID_BYTES) || !cJSON_IsString(icon) ||
            !text_field(cJSON_GetObjectItemCaseSensitive(item, "name"), PET_STORY_NAME_BYTES, blessing->name) ||
            !whole(cJSON_GetObjectItemCaseSensitive(item, "uses"), 0, 9, &blessing->uses))
            return false;
        blessing->icon = PET_BLESSING_ICONS;
        for (uint8_t i = 0; i < known; ++i)
            if (!strcmp(icon->valuestring, icons[i]))
                blessing->icon = i;
        if (blessing->icon == PET_BLESSING_ICONS)
            return false;
        strcpy(blessing->id, id->valuestring);
        ++state->blessing_count;
    }
    state->has_blessings = true;
    return true;
}

const char *pet_session_wire_story_state(const cJSON *message, bool game, bool words, bool pacing, bool hurry,
                                         pet_story_state_t *out)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(message, "mode");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(message, "title");
    const cJSON *goal = cJSON_GetObjectItemCaseSensitive(message, "goal");
    pet_story_state_t state = {0};
    if (!version_one(message))
        return "STORY_STATE_VERSION";
    if (!cJSON_IsString(mode) || (strcmp(mode->valuestring, "story") && strcmp(mode->valuestring, "paused")))
        return "STORY_STATE_MODE";
    state.story = !strcmp(mode->valuestring, "story");
    /* Each is a string or null, never left out. */
    if (!cJSON_IsNull(title) && !text_field(title, PET_STORY_TITLE_BYTES, state.title))
        return "STORY_STATE_TITLE";
    if (!cJSON_IsNull(goal) && !text_field(goal, PET_STORY_GOAL_BYTES, state.goal))
        return "STORY_STATE_GOAL";
    if (!whole(cJSON_GetObjectItemCaseSensitive(message, "reached"), 0, PET_STORY_MILESTONES, &state.reached) ||
        !whole(cJSON_GetObjectItemCaseSensitive(message, "of"), 0, PET_STORY_MILESTONES, &state.of))
        return "STORY_STATE_MILESTONES";
    /* A game's parts are each left out unless the story plays as one. */
    const cJSON *hero = game ? cJSON_GetObjectItemCaseSensitive(message, "hero") : NULL;
    const cJSON *conditions = game ? cJSON_GetObjectItemCaseSensitive(message, "conditions") : NULL;
    const cJSON *blessings = game ? cJSON_GetObjectItemCaseSensitive(message, "blessings") : NULL;
    if (hero && !read_hero(hero, &state))
        return "STORY_STATE_HERO";
    if (conditions && !read_conditions(conditions, &state))
        return "STORY_STATE_CONDITIONS";
    if (blessings && !read_blessings(blessings, words, &state))
        return "STORY_STATE_BLESSINGS";
    /* With pacing, the path: "story" when left out, as when the story has ended. */
    const cJSON *path = pacing ? cJSON_GetObjectItemCaseSensitive(message, "path") : NULL;
    const bool known = cJSON_IsString(path) && (!strcmp(path->valuestring, "story") ||
                                                !strcmp(path->valuestring, "detour") ||
                                                (hurry && !strcmp(path->valuestring, "hurry")));
    if (path && !known)
        return "STORY_STATE_PATH";
    state.detour = path && !strcmp(path->valuestring, "detour");
    state.hurry = path && !strcmp(path->valuestring, "hurry");
    *out = state;
    return NULL;
}

const char *pet_session_wire_story_list(const cJSON *message, pet_story_list_t *out)
{
    const cJSON *stories = cJSON_GetObjectItemCaseSensitive(message, "stories");
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(message, "current");
    const cJSON *story = NULL;
    pet_story_list_t list = {0};
    if (!version_one(message))
        return "STORY_LIST_VERSION";
    if (!cJSON_IsArray(stories) || cJSON_GetArraySize(stories) > PET_STORY_LIST_LENGTH)
        return "STORY_LIST_STORIES";
    cJSON_ArrayForEach(story, stories)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(story, "id");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(story, "title");
        /* Exactly an id and a title, as the cloud's strict schema has it. */
        if (!cJSON_IsObject(story) || cJSON_GetArraySize(story) != 2 || !cJSON_IsString(id) ||
            !uuid(id->valuestring) || !text_field(title, PET_STORY_LIST_TITLE_BYTES, list.stories[list.count].title))
            return "STORY_LIST_STORY";
        strcpy(list.stories[list.count++].id, id->valuestring);
    }
    if (cJSON_IsString(current) && uuid(current->valuestring))
        strcpy(list.current, current->valuestring);
    else if (!cJSON_IsNull(current))
        return "STORY_LIST_CURRENT";
    *out = list;
    return NULL;
}

bool pet_session_wire_story_request(const char *type, const char *story_id, char *out, size_t capacity)
{
    const bool load = type && !strcmp(type, "story.load");
    const bool other = type && (!strcmp(type, "story.pause") || !strcmp(type, "story.restart"));
    if (!out || !(other || (load && uuid(story_id))))
        return false;
    int written;
    if (load)
        written = snprintf(out, capacity, "{\"v\":1,\"type\":\"story.load\",\"storyId\":\"%s\"}", story_id);
    else
        written = snprintf(out, capacity, "{\"v\":1,\"type\":\"%s\"}", type);
    return written > 0 && (size_t)written < capacity;
}

bool pet_session_wire_output_pause(uint32_t stream, bool paused, char *out, size_t capacity)
{
    if (!out)
        return false;
    const int written = snprintf(out, capacity, "{\"v\":1,\"type\":\"output.%s\",\"streamId\":%lu}",
                                 paused ? "pause" : "resume", (unsigned long)stream);
    return written > 0 && (size_t)written < capacity;
}

bool pet_session_wire_story_event_ended(const char *id, char *out, size_t capacity)
{
    if (!out || !slug(id, PET_STORY_EVENT_ID_BYTES))
        return false;
    const int written = snprintf(out, capacity, "{\"v\":1,\"type\":\"story.event.ended\",\"id\":\"%s\"}", id);
    return written > 0 && (size_t)written < capacity;
}

bool pet_session_wire_story_mode_kept(bool story_mode, const pet_control_context_t *from,
                                      const pet_control_context_t *to)
{
    return story_mode && from && to && !strcmp(from->config.face_id, to->config.face_id);
}

/* One of the story's sound loops, by name, or NULL. */
static const char *atmosphere_name(const cJSON *atmosphere)
{
    static const char *const loops[] = {"calm", "forest", "water", "night", "cave", "storm", "danger", "battle",
        "magic", "victory"};
    for (size_t i = 0; cJSON_IsString(atmosphere) && i < sizeof(loops) / sizeof(loops[0]); ++i)
        if (!strcmp(atmosphere->valuestring, loops[i]))
            return loops[i];
    return NULL;
}

/* A sound id (sound-bank-v1). */
static bool sound_id(const cJSON *value)
{
    return cJSON_IsString(value) && slug(value->valuestring, PET_SOUND_ID_BYTES);
}

const char *pet_session_wire_story_atmosphere(const cJSON *message, const char **name)
{
    const cJSON *atmosphere = cJSON_GetObjectItemCaseSensitive(message, "atmosphere");
    if (!version_one(message))
        return "STORY_ATMOSPHERE_VERSION";
    if (!cJSON_IsNull(atmosphere) && !sound_id(atmosphere))
        return "STORY_ATMOSPHERE_NAME";
    *name = cJSON_IsNull(atmosphere) ? NULL : atmosphere->valuestring;
    return NULL;
}

const char *pet_session_wire_sound_bank(const cJSON *message, char sha256[65], uint32_t *bytes)
{
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(message, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(message, "bytes");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(message, "url");
    if (!version_one(message))
        return "SOUND_BANK_VERSION";
    if (!cJSON_IsString(sha) || strlen(sha->valuestring) != 64 || strspn(sha->valuestring, "0123456789abcdef") != 64)
        return "SOUND_BANK_SHA256";
    if (!cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > PET_SOUND_BANK_MAX_BYTES ||
        size->valuedouble != (double)(uint32_t)size->valuedouble)
        return "SOUND_BANK_BYTES";
    if (!cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) || strlen(url->valuestring) > 320)
        return "SOUND_BANK_URL";
    strcpy(sha256, sha->valuestring);
    *bytes = (uint32_t)size->valuedouble;
    return NULL;
}

/* An event's animation: exactly an https url of at most 320 characters, the
 * pack's sha256 in lowercase hex and its size, 1 to 250 KiB. */
static bool read_animation(const cJSON *animation, pet_story_event_t *event)
{
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(animation, "url");
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(animation, "sha256");
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(animation, "bytes");
    if (!exactly(animation, 3) || !cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) ||
        strlen(url->valuestring) > 320 || !cJSON_IsString(sha256) || strlen(sha256->valuestring) != 64 ||
        strspn(sha256->valuestring, "0123456789abcdef") != 64 || !cJSON_IsNumber(bytes) ||
        bytes->valuedouble < 1 || bytes->valuedouble > PET_STORY_EVENT_BYTES ||
        bytes->valuedouble != (double)(uint32_t)bytes->valuedouble)
        return false;
    strcpy(event->sha256, sha256->valuestring);
    event->bytes = (uint32_t)bytes->valuedouble;
    return true;
}

/* One event, exactly as the cloud's strict schema has it: an id, a name of
 * 1 to 40 bytes, its animation and, if there, one of the sound loops and its
 * stinger's sound id. */
static bool read_event(const cJSON *item, pet_story_event_t *event)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
    const cJSON *atmosphere = cJSON_GetObjectItemCaseSensitive(item, "atmosphere");
    const cJSON *sound = cJSON_GetObjectItemCaseSensitive(item, "sound");
    char name[PET_STORY_TITLE_BYTES + 1];
    if (!exactly(item, 3 + (atmosphere != NULL) + (sound != NULL)) || !cJSON_IsString(id) ||
        !slug(id->valuestring, PET_STORY_EVENT_ID_BYTES) ||
        !text_field(cJSON_GetObjectItemCaseSensitive(item, "name"), PET_STORY_TITLE_BYTES, name) ||
        !read_animation(cJSON_GetObjectItemCaseSensitive(item, "animation"), event) ||
        (atmosphere && !atmosphere_name(atmosphere)) || (sound && !sound_id(sound)))
        return false;
    strcpy(event->id, id->valuestring);
    strcpy(event->sound, sound ? sound->valuestring : "");
    return true;
}

const char *pet_session_wire_story_events(const cJSON *message, pet_story_events_t *out)
{
    const cJSON *events = cJSON_GetObjectItemCaseSensitive(message, "events");
    const cJSON *item = NULL;
    uint32_t total = 0;
    if (!version_one(message))
        return "STORY_EVENTS_VERSION";
    if (!cJSON_IsArray(events) || cJSON_GetArraySize(events) > PET_STORY_EVENTS)
        return "STORY_EVENTS_LIST";
    /* Checked whole before `out` is written, with no copy of the list (1.8 KB)
     * on the network task's stack. */
    cJSON_ArrayForEach(item, events)
    {
        pet_story_event_t event;
        if (!read_event(item, &event))
            return "STORY_EVENTS_EVENT";
        for (const cJSON *before = events->child; before != item; before = before->next)
            if (!strcmp(cJSON_GetObjectItemCaseSensitive(before, "id")->valuestring, event.id))
                return "STORY_EVENTS_TWICE";
        total += event.bytes;
    }
    if (total > PET_STORY_EVENTS_BYTES)
        return "STORY_EVENTS_BYTES";
    out->count = 0;
    cJSON_ArrayForEach(item, events)
        read_event(item, &out->events[out->count++]);
    return NULL;
}

const char *pet_session_wire_story_sound(const cJSON *message, char id[PET_SOUND_ID_BYTES + 1])
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(message, "id");
    if (!version_one(message))
        return "STORY_SOUND_VERSION";
    if (!sound_id(value))
        return "STORY_SOUND_ID";
    strcpy(id, value->valuestring);
    return NULL;
}

const char *pet_session_wire_story_event(const cJSON *message, char id[PET_STORY_EVENT_ID_BYTES + 1])
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(message, "id");
    if (!version_one(message))
        return "STORY_EVENT_VERSION";
    if (!cJSON_IsString(value) || !slug(value->valuestring, PET_STORY_EVENT_ID_BYTES))
        return "STORY_EVENT_ID";
    strcpy(id, value->valuestring);
    return NULL;
}

bool pet_session_wire_story_turn(uint32_t stream, const char *blessing, const char *ai_pet_id,
                                 const char *speech_profile, char *out, size_t capacity)
{
    if (!out || !capacity || capacity > 0x7fffffff || (blessing && !slug(blessing, PET_STORY_BLESSING_ID_BYTES)))
        return false;
    cJSON *root = cJSON_CreateObject();
    bool ok = root && cJSON_AddNumberToObject(root, "v", 1) &&
        cJSON_AddStringToObject(root, "type", blessing ? "input.blessing" : "input.story") &&
        cJSON_AddNumberToObject(root, "streamId", stream) &&
        (!blessing || cJSON_AddStringToObject(root, "blessing", blessing)) &&
        (!ai_pet_id || !ai_pet_id[0] || cJSON_AddStringToObject(root, "aiPetId", ai_pet_id)) &&
        (!speech_profile || cJSON_AddStringToObject(root, "speechProfile", speech_profile));
    ok = ok && cJSON_PrintPreallocated(root, out, (int)capacity, false);
    cJSON_Delete(root);
    return ok;
}

bool pet_session_wire_input_start(uint32_t stream, int64_t started_ms, const char *ai_pet_id,
                                  const char *speech_profile, const char *blessing, char *out, size_t capacity)
{
    const bool blessed = blessing && blessing[0];
    if (!out || !capacity || capacity > 0x7fffffff || (blessed && !slug(blessing, PET_STORY_BLESSING_ID_BYTES)))
        return false;
    cJSON *root = cJSON_CreateObject();
    cJSON *format = root ? cJSON_CreateObject() : NULL;
    bool ok = format && cJSON_AddNumberToObject(root, "v", 1) &&
        cJSON_AddStringToObject(root, "type", "input.audio.start") &&
        cJSON_AddNumberToObject(root, "streamId", stream) && cJSON_AddItemToObject(root, "format", format);
    if (!ok)
        cJSON_Delete(format);
    ok = ok && cJSON_AddStringToObject(format, "encoding", "pcm_s16le") &&
        cJSON_AddNumberToObject(format, "sampleRate", 16000) && cJSON_AddNumberToObject(format, "channels", 1) &&
        cJSON_AddNumberToObject(root, "startedAtMs", started_ms) &&
        (!ai_pet_id || !ai_pet_id[0] || cJSON_AddStringToObject(root, "aiPetId", ai_pet_id)) &&
        (!speech_profile || cJSON_AddStringToObject(root, "speechProfile", speech_profile)) &&
        (!blessed || cJSON_AddStringToObject(root, "blessing", blessing));
    ok = ok && cJSON_PrintPreallocated(root, out, (int)capacity, false);
    cJSON_Delete(root);
    return ok;
}

static bool same(const cJSON *object, const char *key, const char *expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && !strcmp(value->valuestring, expected);
}

bool pet_session_wire_audio_start(const cJSON *message, uint32_t *stream, uint32_t *rate, bool *adpcm)
{
    if (!message || !stream || !rate || !adpcm) return false;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "streamId");
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(message, "format");
    const cJSON *sample = cJSON_GetObjectItemCaseSensitive(format, "sampleRate");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(format, "channels");
    if (!cJSON_IsNumber(id) || id->valuedouble < 0 || id->valuedouble > UINT32_MAX ||
        id->valuedouble != (double)(uint32_t)id->valuedouble ||
        !cJSON_IsNumber(channels) || channels->valuedouble != 1 ||
        !cJSON_IsNumber(sample) || (sample->valuedouble != 16000 && sample->valuedouble != 24000)) return false;
    bool coded = same(format, "encoding", "ima_adpcm");
    if (!coded && !same(format, "encoding", "pcm_s16le")) return false;
    if (coded && sample->valuedouble != 16000) return false;
    *stream = (uint32_t)id->valuedouble;
    *rate = (uint32_t)sample->valuedouble;
    *adpcm = coded;
    return true;
}

bool pet_session_wire_accepted(const cJSON *m, const pet_control_context_t *e)
{
    if (!cJSON_IsObject(m) || !e || !e->binding.assigned) return false;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(m, "v"), *b = cJSON_GetObjectItemCaseSensitive(m, "binding");
    return cJSON_IsNumber(v) && v->valuedouble == 1 && same(m, "type", "gateway.binding.accepted") && cJSON_IsObject(b) &&
        same(b, "revision", e->binding.revision) && same(b, "relationshipId", e->binding.relationship_id) &&
        same(b, "buildId", e->binding.build_id) && same(b, "sha256", e->binding.sha256) &&
        same(b, "configVersion", e->config.version);
}

bool pet_session_wire_brain_grant(const cJSON *root, const pet_control_context_t *expected,
                                 const char *origin, char token[PET_BRAIN_TOKEN_MAX])
{
    if (!root || !expected || !token || !expected->binding.assigned ||
        !pet_session_wire_origin(origin, NULL, 0, NULL))
        return false;
    char endpoint[160];
    snprintf(endpoint, sizeof(endpoint), "wss://%s/v1/device", origin + 8);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(root, "expiresInSeconds");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "token");
    const cJSON *binding = cJSON_GetObjectItemCaseSensitive(root, "binding");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !cJSON_IsNumber(ttl) || ttl->valuedouble != 900 ||
        !same(root, "endpoint", endpoint) || !cJSON_IsString(value) ||
        !pet_session_wire_brain_token(value->valuestring) || !cJSON_IsObject(binding) ||
        !same(binding, "revision", expected->binding.revision) ||
        !same(binding, "relationshipId", expected->binding.relationship_id) ||
        !same(binding, "buildId", expected->binding.build_id) ||
        !same(binding, "sha256", expected->binding.sha256) ||
        !same(binding, "configVersion", expected->config.version)) return false;
    memcpy(token, value->valuestring, PET_BRAIN_TOKEN_MAX);
    return true;
}
