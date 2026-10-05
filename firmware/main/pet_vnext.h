#pragma once
#include "pet_config.h"
/* Generic firmware entry: built-in setup, download/control, existing face/audio. */
esp_err_t pet_vnext_start(const pet_config_t *wifi,const char *origin);
/* Select once before legacy OTA initialization. Single/unknown physical layout
 * uses independent v2 lifecycle; only recognized legacy layouts retain v1. */
bool pet_vnext_independent_control(void);
