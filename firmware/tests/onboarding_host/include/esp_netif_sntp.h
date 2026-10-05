#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef struct { const char *server; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(name) {.server=(name)}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_sync_wait(TickType_t timeout);
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
