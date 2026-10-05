#pragma once
#include "pet_enrollment.h"
#include "pet_control_wire.h"

typedef struct {
    const char *origin; /* Pinned HTTPS DNS origin; never a response-provided URL. */
    const char *device_id;
    const char *credential;
} pet_control_http_t;
typedef struct { int status; unsigned retry_seconds; size_t bytes; } pet_control_http_result_t;
/* The last request's timing on the device's clock: connect_ms opening a new
 * connection (DNS, TCP and TLS), 0 on a kept one; total_ms the whole request. */
typedef struct { uint32_t connect_ms,total_ms; bool reused; } pet_control_http_timing_t;
/* One operation's range downloads. Owned by the worker running the operation,
 * zero-initialized with it and never shared across tasks. Consecutive ranges
 * with the same origin, path, SHA-256 and size reuse one keep-alive HTTPS
 * connection; each range still has its own absolute 30 s deadline and its own
 * Authorization header. An error, redirect, Connection: close, unread or
 * unrequested byte discards the connection, and the next range connects anew.
 * After two consecutive connection failures (connect, TLS, or no response
 * byte before the deadline) every remaining range of the operation gets its
 * own new connection. HTTP status, range, ETag and verification failures are
 * not connection failures and keep their callers' recovery. */
typedef struct {
    void *client,*transport; /* The kept connection, or NULL. */
    char url[200],sha256[65];
    uint32_t total;
    bool independent;
    uint8_t failures;
    uint32_t bytes,ranges,connections,reused,failed;
    int64_t started_us,connect_us,transfer_us,write_us,returned_us;
} pet_control_download_t;

/* Single worker only. No logging of request bodies, headers or responses.
 * JSON result requires transport completion (including bounded 4xx/5xx error
 * bodies); caller MUST validate status/schema before accepting it as success.
 * JSON requests share one kept connection to their origin (the control task
 * is their only caller) while it has been idle less than 25 s; each keeps its
 * own 30 s deadline and Authorization header. It is kept only after a fully
 * consumed exchange below 500 without Connection: close. A kept connection
 * that fails before any response byte is replaced once by a new connection
 * and the request sent again: every control request is safe to repeat.
 * Range result additionally requires exact 206, ETag, Content-Range and length.
 * Output must never be written to flash unless the return value is true.
 * download may be NULL: that range then uses a connection of its own. */
bool pet_control_http_json(const pet_control_http_t *http,const char *path,
                            const char *body,char *response,size_t capacity,
                            pet_control_http_result_t *result);
bool pet_control_http_range(const pet_control_http_t *http,pet_control_download_t *download,
                             const pet_control_operation_t *operation,uint32_t offset,
                             void *response,size_t bytes,pet_control_http_result_t *result);
/* Download transport only: caller must separately verify the signed release,
 * actual layout/headroom and protected packs before starting application OTA. */
bool pet_control_http_firmware_range(const pet_control_http_t *http,pet_control_download_t *download,
                                      const char *operation_id,const char *sha256,uint32_t image_bytes,
                                      uint32_t offset,void *response,size_t bytes,pet_control_http_result_t *result);
/* Single-slot transport only. A successful range is not permission to erase:
 * the caller must hold the exact confirmed fence and durable invalidation. */
bool pet_control_http_pet_range(const pet_control_http_t *http,pet_control_download_t *download,
                                 const char *operation_id,const char *sha256,uint32_t pack_bytes,
                                 uint32_t offset,void *response,size_t bytes,pet_control_http_result_t *result);
/* Right after a successful range is durably written: adds the time since
 * that range returned to the operation's flash-write total. */
void pet_control_download_wrote(pet_control_download_t *download);
/* Connectivity or authority lost: close the connection, keep the operation. */
void pet_control_download_close(pet_control_download_t *download);
/* The operation completed, failed or was cancelled: close its connection, log
 * one summary line (bytes, time, connections, reuse, fallback, connect,
 * transfer and write time) and reset. Does nothing when nothing was fetched. */
void pet_control_download_end(pet_control_download_t *download,bool complete);
bool pet_control_http_library_path(const char *cursor,char *path,size_t capacity);
/* Connectivity lost: close the kept JSON connection. */
void pet_control_http_close_kept(void);
pet_control_http_timing_t pet_control_http_last_timing(void);
