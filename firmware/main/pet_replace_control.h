#pragma once
#include "pet_control_http.h"
#include "pet_replace_receipt.h"
#include "pet_replace_writer.h"

typedef struct {
    pet_control_http_t http;
    const pet_pack_trust_key_t *keys;
    size_t key_count;
    char boot_id[37];
    /* Independently observed qualified, VALID/healthy firmware and physical
     * layout must satisfy this signed release before any new pet flash work. */
    bool (*compatible_healthy)(void *context,const pet_release_v2_t *release);
    /* Map immutable completed bytes and validate full hash + every frame using
     * bounded off-stack workspace. Never display or start conversation here. */
    bool (*verify)(void *context,const pet_release_v2_t *release);
    /* Revalidate signed persisted metadata/full pack for the ACTIVE journal,
     * prepare/render it and admit only the current authenticated binding. */
    bool (*activate)(void *context,const pet_control_context_t *cloud);
    void (*freeze)(void *context);
    /* Optional, for devices that keep their conversation across a selection
     * (three-pet devices, session-rebind-v1). pause: a new binding without an
     * operation change (a selection) revokes voice but keeps the conversation
     * open. rebind: mount admits the current binding over that open
     * conversation; false freezes and activates as before. Without them every
     * binding change freezes and every mount reconnects. */
    void (*pause)(void *context);
    bool (*rebind)(void *context,const pet_control_context_t *cloud);
    /* Optional three-slot deletion. True only after exact-target metadata is
     * durable and no renderer/voice retains it, or when already absent.
     * Called under the same serialized runtime gate as installations. */
    bool (*remove)(void *context, const pet_replace_pack_t *pack);
    uint64_t (*now_ms)(void *context);
    void *context;
} pet_replace_control_config_t;
typedef enum {PET_REPLACE_CONTROL_IDLE,PET_REPLACE_CONTROL_WORKING,PET_REPLACE_CONTROL_WAITING,
    PET_REPLACE_CONTROL_RECOVERY,PET_REPLACE_CONTROL_READY} pet_replace_control_status_t;
typedef struct {
    pet_replace_control_config_t config;
    pet_replace_journal_t *store;
    pet_replace_writer_t *writer;
    pet_replace_poll_t cloud,incoming;
    bool has_context,authenticated,admitted,manifest_ready,verified;
    pet_replace_pack_t verified_pack;
    uint64_t next_poll_ms,retry_at_ms;
    unsigned removal_ack_count;
    char removal_ack[PET_REMOVAL_MAX][37];
    pet_replace_control_status_t status;
    pet_control_download_t download; /* Its range connection, only while DOWNLOADING. */
    char error_code[81],request[2048],response[PET_CONTROL_RESPONSE_MAX];
    uint8_t chunk[PET_REPLACE_CHECKPOINT_BYTES];
} pet_replace_control_t;

/* Allocate this bounded worker in PSRAM, never on a task stack. Polling stays
 * operational with unavailable/corrupt journal or writer. No I/O during init. */
bool pet_replace_control_init(pet_replace_control_t *worker,const pet_replace_control_config_t *config,
                               pet_replace_journal_t *store,pet_replace_writer_t *writer);
/* Same serialized task/runtime gate as firmware OTA. At most one bounded HTTP
 * call per step. allow_flash=false still polls/replays existing durable reports,
 * but cannot change the journal, write pet bytes or admit a conversation. */
pet_replace_control_status_t pet_replace_control_step(pet_replace_control_t *worker,uint64_t now_ms,bool allow_flash);
/* Call on loss of control connectivity or runtime admission; cached pixels may
 * remain but microphone/socket authority is revoked until fresh reconciliation. */
void pet_replace_control_disconnect(pet_replace_control_t *worker);

typedef enum {PET_REPLACE_SELECT_CHOSEN,PET_REPLACE_SELECT_REFUSED,PET_REPLACE_SELECT_UNAVAILABLE} pet_replace_select_t;
/* Three-pet devices: ask the cloud to talk as `pack`, an installed pet
 * (POST /v2/device/pets/select). One bounded HTTP call on the control task,
 * between steps, only while authenticated with a context and outside the
 * worker's backoff. CHOSEN adopts the reply, a v2 poll result binding exactly
 * `pack` with no operation, as the worker's cloud state; the caller then
 * rebinds its journal to match before the next step mounts. REFUSED (a final
 * answer, or a reply binding anything else) changes nothing. UNAVAILABLE
 * changes nothing either: with no answer, or a 429 or 5xx, it starts the
 * worker's backoff (Retry-After when given), after which the caller asks again.
 * `http`, if not NULL, receives the answer's HTTP status, or 0 without one. */
pet_replace_select_t pet_replace_control_select(pet_replace_control_t *worker,uint64_t now_ms,
                                                const pet_replace_pack_t *pack,int *http);
