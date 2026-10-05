#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef void *esp_http_client_handle_t;
typedef int esp_http_client_method_t;
typedef struct { int event_id; char *header_key; char *header_value; void *user_data; } esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 3
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_GET 0
typedef struct {
    const char *url;
    int method;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *);
    bool disable_auto_redirect;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    void *transport;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length);
int esp_http_client_write(esp_http_client_handle_t client, const char *bytes, int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *bytes, int capacity);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
