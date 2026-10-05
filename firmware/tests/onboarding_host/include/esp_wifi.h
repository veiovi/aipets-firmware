#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef struct { uint8_t ssid[33]; } wifi_ap_record_t;
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);
