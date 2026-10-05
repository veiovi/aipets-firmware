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
#define PET_BRAIN_CAPABILITY "brain-v1"
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
/* A gateway.binding.accepted naming exactly `expected`'s binding and
 * configuration version. */
bool pet_session_wire_accepted(const cJSON *message, const pet_control_context_t *expected);
