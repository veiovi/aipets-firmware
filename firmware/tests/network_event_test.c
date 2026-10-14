/* Exercise the real callbacks with the pinned WebSocket event definitions.
 * No serial port, Wi-Fi connection, task or socket is created by this test. */
#include "network_host.h"
#ifndef PET_NETWORK_SOURCE
#define PET_NETWORK_SOURCE "../main/pet_network.c"
#endif
#include PET_NETWORK_SOURCE
#include <stdarg.h>
#include "nvs.h"

esp_event_base_t const WIFI_EVENT = "wifi", IP_EVENT = "ip", WEBSOCKET_EVENTS = "ws";
static unsigned disconnects, connects, starts, stops, destroys;
static unsigned radio_connects, radio_disconnects, sends, scans;
static bool station_connected;
static char configured_ssid[33];
/* One saved network for auto-join to recall; NULL keeps settings empty. */
static const char *saved_ssid, *saved_password;
/* The last check runs the real Wi-Fi settings code: NVS is then a small in-memory table. */
static bool settings_writable;
static struct {
    char key[16];
    char value[72];
} settings[24];
static unsigned settings_count;
static char *setting(const char *key, bool create)
{
    for (unsigned i = 0; i < settings_count; i++)
        if (!strcmp(settings[i].key, key)) return settings[i].value;
    if (!create) return NULL;
    assert(settings_count < sizeof(settings) / sizeof(settings[0]));
    strcpy(settings[settings_count].key, key);
    return settings[settings_count++].value;
}
static pet_wifi_admin_command_t wifi_received;
static bool wifi_command(const pet_wifi_admin_command_t *command)
{
    wifi_received = *command;
    return true;
}
static uint32_t wifi_bits;
static char output[8192];
static size_t output_bytes;
static uintptr_t next_client;

void network_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list args;
    va_start(args, format);
    int bytes = vsnprintf(output + output_bytes, sizeof(output) - output_bytes, format, args);
    va_end(args);
    assert(bytes >= 0 && (size_t)bytes < sizeof(output) - output_bytes);
    output_bytes += (size_t)bytes;
}
static int64_t now_us = 1234000;
int64_t esp_timer_get_time(void)
{
    return now_us;
}
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)name;
    *handle = 1;
    if (settings_writable) return ESP_OK;
    assert(mode == NVS_READONLY);
    return saved_ssid ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *value, size_t *bytes)
{
    (void)h;
    const char *stored = settings_writable ? setting(key, false) :
        !strcmp(key, "ssid") ? saved_ssid : !strcmp(key, "password") ? saved_password : NULL;
    if (!stored) return ESP_ERR_NVS_NOT_FOUND;
    if (strlen(stored) + 1 > *bytes) return ESP_ERR_NVS_INVALID_LENGTH;
    strcpy(value, stored);
    *bytes = strlen(stored) + 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *value)
{
    (void)h;
    const char *stored = settings_writable ? setting(key, false) : NULL;
    if (!stored) return ESP_ERR_NVS_NOT_FOUND;
    *value = (uint8_t)atoi(stored);
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    assert(settings_writable && "network events must not persist settings");
    assert(strlen(value) < sizeof(settings[0].value));
    strcpy(setting(key, true), value);
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    (void)h;
    assert(settings_writable && "network events must not persist settings");
    snprintf(setting(key, true), sizeof(settings[0].value), "%u", (unsigned)value);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    assert(settings_writable && "no settings writes");
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    assert(settings_writable && "no settings writes");
    for (unsigned i = 0; i < settings_count; i++) {
        if (strcmp(settings[i].key, key)) continue;
        settings[i] = settings[--settings_count];
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {.version = "fixture"};
    return &app;
}
esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    (void)type;
    memset(mac, 0, 6);
    return ESP_OK;
}
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int len, TickType_t ticks)
{
    (void)client;
    (void)data;
    (void)ticks;
    sends++;
    return len;
}
const char *pet_diagnostics_boot_id(void)
{
    return "fixture";
}
esp_err_t pet_diagnostics_acknowledge_crash(const char *sha, uint32_t pc)
{
    (void)sha;
    (void)pc;
    assert(!"crash acknowledgement is outside this scenario");
    return ESP_FAIL;
}
esp_err_t pet_face_pack_list(pet_face_catalog_item_t *items, size_t capacity, size_t *count)
{
    (void)items;
    (void)capacity;
    *count = 0;
    return ESP_OK;
}
static void connected(bool ready)
{
    if (ready)
        connects++;
    else
        disconnects++;
}
uint32_t xEventGroupGetBits(EventGroupHandle_t group)
{
    return *group;
}
uint32_t xEventGroupSetBits(EventGroupHandle_t group, uint32_t bits)
{
    return *group |= bits;
}
uint32_t xEventGroupClearBits(EventGroupHandle_t group, uint32_t bits)
{
    return *group &= ~bits;
}
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t ticks)
{
    (void)lock;
    (void)ticks;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t lock)
{
    (void)lock;
    return pdTRUE;
}
void xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
}
esp_err_t esp_wifi_connect(void)
{
    radio_connects++;
    return ESP_OK;
}
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config)
{
    assert(interface == WIFI_IF_STA);
    memcpy(configured_ssid, config->sta.ssid, sizeof(config->sta.ssid));
    configured_ssid[sizeof(config->sta.ssid)] = 0;
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{
    radio_disconnects++;
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    (void)ap;
    return station_connected ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    (void)config;
    assert(!block);
    scans++;
    return ESP_OK;
}
esp_err_t esp_crt_bundle_attach(void *config)
{
    (void)config;
    return ESP_OK;
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config)
{
    assert(config->disable_auto_reconnect && !config->enable_close_reconnect);
    return (esp_websocket_client_handle_t)++next_client;
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, esp_websocket_event_id_t event,
                                        esp_event_handler_t handler, void *arg)
{
    (void)client;
    (void)arg;
    assert(event == WEBSOCKET_EVENT_ANY && handler == websocket_event);
    return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client)
{
    (void)client;
    starts++;
    return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client)
{
    (void)client;
    destroys++;
    return ESP_OK;
}
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client)
{
    stops++;
    esp_websocket_event_data_t event = {.client = client};
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_CONNECTED, &event);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_CLOSED, &event);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_FINISH, &event);
    return ESP_OK;
}

static void reset_session(void)
{
    if (s_websocket)
        pet_network_freeze_bound();
    free(s_rx_frame);
    s_websocket = NULL;
    strcpy(s_config.gateway_host, "fixture.invalid");
    strcpy(s_config.pairing_token, "brain1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    s_config.gateway_port = 443;
    s_config.gateway_secure = true;
    create_websocket(true);
    assert(s_websocket);
    s_rx_frame = malloc(8);
    s_rx_expected = 8;
    s_bound_session = true;
    s_websocket_ready = true;
    s_gateway_rebind = s_gateway_story = s_gateway_sound_bank = true;
    s_rebind_pending = false;
    s_callbacks.connected = connected;
    atomic_store(&s_wifi_connected_callback, connected);
    s_events = &wifi_bits;
    wifi_bits = WIFI_CONNECTED_BIT;
    disconnects = connects = starts = stops = destroys = sends = 0;
}

int main(void)
{
    reset_session();
    assert(pet_network_is_ready());
    esp_websocket_event_data_t event = {.client = s_websocket, .close_status_code = 1000};
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_CLOSED, &event);
    assert(!pet_network_is_ready());
    assert(!s_gateway_rebind && !s_gateway_story && !s_gateway_sound_bank && !s_rebind_pending);
    assert(!s_rx_frame && !s_rx_expected);
    assert(disconnects == 1 && starts == 0 && stops == 0 && destroys == 0);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_FINISH, &event);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_DISCONNECTED, &event);
    assert(disconnects == 1);
    puts("ok clean close retires admission once without reconnecting in the callback");

    reset_session();
    event = (esp_websocket_event_data_t){.client = s_websocket,
                                         .data_ptr = "private-error-text",
                                         .error_handle = {.error_type = WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,
                                                          .esp_tls_last_esp_err = 32775,
                                                          .esp_transport_sock_errno = 119}};
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_ERROR, &event);
    assert(pet_network_is_ready() && disconnects == 0);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_DISCONNECTED, &event);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_FINISH, &event);
    assert(!pet_network_is_ready() && disconnects == 1 && !s_rx_frame);
    assert(starts == 0 && stops == 0 && destroys == 0);
    log_network_events();
    assert(!strstr(output, "private-error-text"));
    puts("ok transport errors record numeric causes and disconnect once");

    reset_session();
    event.client = s_websocket;
    s_websocket_ready = false;
    event.error_handle.error_type = WEBSOCKET_ERROR_TYPE_HANDSHAKE;
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_ERROR, &event);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_DISCONNECTED, &event);
    assert(disconnects == 1);
    puts("ok failure before hello still notifies the owner");

    reset_session();
    event.client = s_websocket;
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_FINISH, &event);
    assert(!pet_network_is_ready() && disconnects == 1);
    puts("ok an unexpected worker exit also retires admission");

    reset_session();
    event.client = s_websocket;
    s_rebind_pending = true;
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_CLOSED, &event);
    assert(!s_rebind_pending && !s_gateway_rebind && disconnects == 1);
    event.op_code = 1;
    event.payload_len = event.data_len = 2;
    event.data_ptr = "{}";
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_DATA, &event);
    assert(!s_rx_frame && !s_websocket_ready);
    puts("ok terminal state discards rebind and late payload fragments");

    esp_websocket_client_handle_t previous = s_websocket;
    reset_session();
    event.client = previous;
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_CLOSED, &event);
    assert(pet_network_is_ready() && disconnects == 0);
    unsigned generation = atomic_load(&s_socket_generation);
    pet_network_freeze_bound();
    assert(!s_websocket && !s_websocket_ready && !s_gateway_rebind && !s_rx_frame);
    assert(disconnects == 0 && sends == 0 && stops == 1 && destroys == 1);
    assert(atomic_load(&s_socket_generation) == generation + 1);
    puts("ok retired handles and owner stops cannot start reconnects or restore admission");

    reset_session();
    log_network_events();
    output_bytes = 0;
    output[0] = '\0';
    strcpy(s_config.wifi_ssid, "private-network-name");
    strcpy(s_config.wifi_password, "private-network-password");
    wifi_event_sta_disconnected_t down = {.reason = 200, .rssi = -67};
    s_wifi_retry_count = 2;
    s_scan_suspended_reconnect = true;
    radio_connects = 0;
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &down);
    assert(!pet_network_wifi_is_ready() && radio_connects == 0);
    network_event_t last = s_network_events[(s_network_event_next + NETWORK_EVENT_COUNT - 1) % NETWORK_EVENT_COUNT];
    assert(last.code == 200 && last.detail == -67 && last.extra == 2);
    assert(!strcmp(last.kind, "wifi-scan-down"));
    s_scan_suspended_reconnect = false;
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &down);
    assert(radio_connects == 1);
    ip_event_got_ip_t ip = {.ip_changed = true};
    wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(pet_network_wifi_is_ready());
    wifi_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(connect_wifi_profile(false) == ESP_OK);
    log_network_events();
    assert(!strstr(output, "private-network-name") && !strstr(output, "private-network-password"));
    puts("ok Wi-Fi reasons, scan intent and IP events contain no network identifiers");

    /* After four failed attempts the device scans, and the strongest saved
     * network in reach joins live, even one that is neither primary nor fallback. */
    scans = radio_connects = 0;
    s_wifi_retry_count = WIFI_RETRIES_PER_PROFILE - 1;
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &down);
    assert(scans == 1 && radio_connects == 0 && atomic_load(&s_autojoin_pending));
    atomic_store(&s_scan_running, false);
    atomic_store(&s_autojoin_pending, false);
    saved_ssid = "office";
    saved_password = "office-password";
    const pet_wifi_network_t seen[] = {{.ssid = "cafe", .rssi = -40}, {.ssid = "office", .rssi = -60, .saved = true},
                                       {.ssid = "home", .rssi = -75, .saved = true}};
    autojoin(seen, 3, ESP_OK);
    assert(radio_connects == 1 && !strcmp(configured_ssid, "office"));
    /* The office refuses four times (say its password changed): it is set aside
     * and the next strongest saved network joins, instead of the office forever. */
    s_wifi_retry_count = WIFI_RETRIES_PER_PROFILE - 1;
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &down);
    assert(scans == 2 && s_wifi_failed_count == 2 && wifi_failed("private-network-name") && wifi_failed("office"));
    atomic_store(&s_scan_running, false);
    atomic_store(&s_autojoin_pending, false);
    saved_ssid = "home";
    saved_password = "home-password";
    autojoin(seen, 3, ESP_OK);
    assert(radio_connects == 2 && !strcmp(configured_ssid, "home"));
    /* With none left in reach, the list starts over and the primary is tried. */
    autojoin(seen, 1, ESP_OK);
    assert(radio_connects == 3 && !strcmp(configured_ssid, "private-network-name") && s_wifi_failed_count == 0);

    /* A network stored over USB joins without a restart: an idle station (no
     * disconnect event would come) connects directly; a connected one drops its
     * network so the disconnect handler joins the new one. */
    saved_ssid = "office";
    saved_password = "office-password";
    radio_connects = radio_disconnects = 0;
    station_connected = false;
    s_send_lock = (SemaphoreHandle_t)1; /* the stubs never block */
    assert(pet_network_use_stored_wifi() == ESP_OK);
    assert(radio_connects == 1 && radio_disconnects == 0 && !strcmp(configured_ssid, "office") &&
           !strcmp(s_config.wifi_ssid, "office"));
    station_connected = true;
    assert(pet_network_use_stored_wifi() == ESP_OK);
    assert(radio_connects == 1 && radio_disconnects == 1);
    station_connected = false;
    saved_ssid = saved_password = NULL;
    log_network_events();
    assert(!strstr(output, "office"));
    puts("ok auto-join takes the strongest saved network in reach, then the primary again");

    /* A management reply carries a network sent from Studio: the device gets it,
     * and its answer leaves in the next request, which names the capability. */
    s_bound_session = true;
    s_send_lock = (SemaphoreHandle_t)1; /* the stubs never block */
    s_callbacks.wifi_admin_received = wifi_command;
    const char reply[] = "{\"version\":1,\"messages\":[{\"v\":1,\"type\":\"device.wifi.command\","
        "\"operationId\":\"0b7b3a40-3e9f-4f3a-9d43-5c0d6a1b2c3d\",\"action\":\"add\",\"ssid\":\"Office\","
        "\"password\":\"office-pass-1\",\"timeoutSeconds\":45}]}";
    assert(pet_network_management_response(reply, strlen(reply)));
    assert(wifi_received.action == PET_WIFI_ADMIN_ADD && !strcmp(wifi_received.ssid, "Office") &&
           !strcmp(wifi_received.password, "office-pass-1"));
    assert(pet_network_wifi_admin_result(wifi_received.operation_id, "Office", "saved", NULL, 0) == ESP_OK);
    char request[4096];
    assert(pet_network_management_request(request, sizeof(request)));
    assert(strstr(request, "\"capabilities\":[\"wifi-command-v1\"]") && strstr(request, "\"type\":\"device.wifi.result\"") &&
           strstr(request, "\"status\":\"saved\"") && !strstr(request, "office-pass-1"));
    s_bound_session = false;
    puts("ok a Wi-Fi network from a management reply is received and answered in the next request");

    for (unsigned i = 0; i < NETWORK_EVENT_COUNT + 3; i++)
        record_network_event("fixture", (int)i, 0, 0);
    assert(s_network_event_count == NETWORK_EVENT_COUNT && s_network_event_lost == 3);
    unsigned oldest = (s_network_event_next + NETWORK_EVENT_COUNT - s_network_event_count) % NETWORK_EVENT_COUNT;
    assert(s_network_events[oldest].code == 3);
    log_network_events();
    assert(!s_network_event_count && !s_network_event_lost);
    puts("ok bounded diagnostics retain newest events and count overwrites");

    s_network_counts = (network_counts_t){0};
    reset_session();
    s_websocket_ready = false;
    event = (esp_websocket_event_data_t){.client = s_websocket, .close_status_code = 1001};
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_CLOSED, &event);
    websocket_event(NULL, WEBSOCKET_EVENTS, WEBSOCKET_EVENT_FINISH, &event);
    assert(s_network_counts.socket_closes == 1 && s_network_counts.last_close_code == 1001);
    now_us += 4200000;
    const char hello[] = "{\"v\":1,\"type\":\"gateway.hello\",\"capabilities\":[\"" PET_BRAIN_CAPABILITY "\"]}";
    process_control((const uint8_t *)hello, strlen(hello), false);
    assert(s_websocket_ready && s_network_counts.last_reconnect_ms == 4200);
    pet_network_freeze_bound();
    assert(s_network_counts.socket_closes == 1);
    s_scan_suspended_reconnect = false;
    wifi_event_sta_disconnected_t lost = {.reason = 8, .rssi = -80};
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &lost);
    assert(s_network_counts.wifi_drops == 1 && s_network_counts.last_wifi_reason == 8);
    puts("ok telemetry counts unasked closes, the time voice took to come back, and Wi-Fi drops");
    pet_network_freeze_bound();

    /* A network from Studio is saved behind the primary and the fallback, so
     * forgetting another network over USB keeps them. */
    settings_writable = true;
    char primary[PET_SSID_MAX], primary_password[PET_PASSWORD_MAX];
    char fallback[PET_SSID_MAX], fallback_password[PET_PASSWORD_MAX];
    assert(pet_config_store_wifi_explicit("cafe", "cafe-password") == ESP_OK);
    assert(pet_config_store_wifi_explicit("office", "office-password") == ESP_OK);
    assert(pet_config_store_wifi_explicit("home", "home-password") == ESP_OK);
    assert(pet_config_save_wifi("studio", "studio-password") == ESP_OK);
    assert(pet_config_forget_wifi("cafe") == ESP_OK);
    assert(pet_config_load_wifi(primary, primary_password, fallback, fallback_password) == ESP_OK);
    assert(!strcmp(primary, "home") && !strcmp(fallback, "office"));
    /* Sending a network the device already knows replaces its password. */
    assert(pet_config_save_wifi("office", "office-password-2") == ESP_OK);
    assert(pet_config_load_wifi(primary, primary_password, fallback, fallback_password) == ESP_OK);
    assert(!strcmp(primary_password, "home-password") && !strcmp(fallback_password, "office-password-2"));
    puts("ok a network from Studio is saved behind the primary and the fallback");
    return 0;
}
