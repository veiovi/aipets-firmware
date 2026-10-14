/* Narrow Wi-Fi/RTOS boundary for the real pet_network.c. WebSocket types and
 * event values come from the pinned component, not a duplicated client model. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#define BIT0 1
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_WIFI_STATE 0x3006
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
esp_err_t nvs_flash_init(void);
void usb_serial_jtag_vfs_use_driver(void);
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
#define ESP_RETURN_ON_ERROR(call, tag, text)                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        (void)(tag);                                                                                                   \
        esp_err_t e = (call);                                                                                          \
        if (e != ESP_OK)                                                                                               \
            return e;                                                                                                  \
    } while (0)
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
#define ESP_EVENT_DECLARE_BASE(name) extern esp_event_base_t const name
#define ESP_EVENT_ANY_ID -1
ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
ESP_EVENT_DECLARE_BASE(IP_EVENT);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *);
esp_err_t esp_event_loop_create_default(void);
typedef void *TaskHandle_t;
typedef uint32_t *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
uint32_t xEventGroupGetBits(EventGroupHandle_t);
uint32_t xEventGroupSetBits(EventGroupHandle_t, uint32_t);
uint32_t xEventGroupClearBits(EventGroupHandle_t, uint32_t);
QueueHandle_t xQueueCreateWithCaps(unsigned, unsigned, unsigned);
int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *, unsigned, void *, int);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(int, TickType_t);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
enum
{
    WIFI_EVENT_STA_START,
    WIFI_EVENT_STA_CONNECTED,
    WIFI_EVENT_STA_DISCONNECTED,
    WIFI_EVENT_SCAN_DONE
};
enum
{
    IP_EVENT_STA_GOT_IP,
    IP_EVENT_STA_LOST_IP
};
enum
{
    WIFI_IF_STA,
    WIFI_MODE_STA,
    WIFI_STORAGE_RAM,
    WIFI_SCAN_TYPE_ACTIVE,
    WIFI_AUTH_OPEN,
    WIFI_AUTH_WPA2_PSK,
    WPA3_SAE_PWE_BOTH
};
typedef enum
{
    WIFI_PS_NONE,
    WIFI_PS_MIN_MODEM
} wifi_ps_type_t;
typedef struct
{
    uint8_t ssid[33];
    int8_t rssi;
    int authmode;
} wifi_ap_record_t;
typedef struct
{
    int channel, authmode;
} wifi_event_sta_connected_t;
typedef struct
{
    uint16_t reason;
    int8_t rssi;
} wifi_event_sta_disconnected_t;
typedef struct
{
    bool ip_changed;
} ip_event_got_ip_t;
typedef struct
{
    int unused;
} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct
{
    struct
    {
        uint8_t ssid[32], password[64];
        struct
        {
            int authmode;
        } threshold;
        int sae_pwe_h2e;
    } sta;
} wifi_config_t;
typedef struct
{
    bool show_hidden;
    int scan_type;
} wifi_scan_config_t;
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, wifi_config_t *);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *, bool);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *, wifi_ap_record_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_netif_init(void);
void *esp_netif_create_default_wifi_sta(void);
size_t esp_get_free_heap_size(void);
int esp_reset_reason(void);
size_t heap_caps_get_free_size(unsigned);
void network_test_log(const char *, const char *, ...);
#define ESP_LOGI network_test_log
#define ESP_LOGW network_test_log
#define ESP_LOGE network_test_log
