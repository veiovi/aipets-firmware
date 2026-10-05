#include "pet_session_wire.h"
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
        "session.error", "ping", "pong"};
    static const char *const output[] = {"device.hello", "device.binding.changed",
        "input.audio.start", "input.audio.end", "input.cancel", "input.story", "ping", "pong"};
    return incoming ? listed(type, input, sizeof(input) / sizeof(input[0])) :
                      listed(type, output, sizeof(output) / sizeof(output[0]));
}

bool pet_session_wire_management_message(const char *type, bool incoming)
{
    static const char *const input[] = {"pet.config.desired", "pet.speech.profile", "pet.crash.acknowledged"};
    static const char *const output[] = {"device.telemetry", "device.config.applied",
        "device.config.rejected", "device.speech.profile.confirmed"};
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
