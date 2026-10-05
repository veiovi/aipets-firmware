#pragma once
#include "pet_asset_store.h"
#include "pet_control_http.h"
#include "pet_pack_verify.h"

typedef enum { PET_SYNC_WAIT, PET_SYNC_LIBRARY, PET_SYNC_DOWNLOAD, PET_SYNC_VERIFY,
    PET_SYNC_ACTIVATE, PET_SYNC_READY, PET_SYNC_AUTH, PET_SYNC_STORAGE,
    PET_SYNC_UNTRUSTED, PET_SYNC_RECONCILE } pet_sync_status_t;
typedef struct {
    /* Stop microphone/playback and close the old socket. Keep the cached face.
     * Called before every uncertain admission or activation. Idempotent. */
    void (*freeze)(void *context);
    /* Full frame_player validation + arena initialization WITHOUT displaying.
     * The store retains this mapping until the next explicit release. */
    bool (*prepare)(void *context,unsigned slot,const void *pack,size_t bytes,
                     const pet_pack_verified_manifest_t *manifest);
    /* Apply/persist remote config, display prepared pack and admit ONLY this
     * exact cloud binding/config. Never use legacy fallback hydration. */
    bool (*activate)(void *context,unsigned slot,const pet_control_context_t *cloud,bool online);
    bool (*release)(void *context,unsigned slot); /* Detach inactive renderer references before unmap. */
    bool (*busy)(void *context); /* Voice or OTA owns shared resources. */
    void (*healthy)(void *context); /* Valid authenticated control context. */
    void *context;
} pet_sync_runtime_t;
typedef struct {
    pet_asset_store_t *store;
    pet_control_http_t http;
    const pet_pack_trust_key_t *keys;
    size_t key_count;
    pet_sync_runtime_t runtime;
    pet_control_context_t cloud;
    pet_control_operation_t operation;
    pet_control_download_t download; /* Its range connection, only while DOWNLOADING. */
    pet_sync_status_t status;
    unsigned retry_seconds;
    int http_status;
    char error_code[81];
    bool hello_sent,has_context,admitted;
    int prepared_slot;
    uint64_t inventory_generation;
    const char *firmware;
} pet_sync_t;

/* All calls on one worker. Struct must live on heap/static, not task stack.
 * Caller supplies a zero-initialized, opened store and real pinned trust keys. */
bool pet_sync_init(pet_sync_t *sync,pet_asset_store_t *store,const pet_control_http_t *http,
                   const pet_pack_trust_key_t *keys,size_t key_count,
                   const pet_sync_runtime_t *runtime,const char *firmware);
/* One bounded control/download checkpoint per call. Retry delay is explicit. */
void pet_sync_step(pet_sync_t *sync);
bool pet_sync_show_cached(pet_sync_t *sync); /* Verify durable pack; NEVER authorize a socket. */
bool pet_sync_library(pet_sync_t *sync,const char *cursor,pet_control_library_t *library);
bool pet_sync_select(pet_sync_t *sync,const pet_control_library_item_t *item,const char *request_uuid);
