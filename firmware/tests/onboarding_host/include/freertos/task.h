#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
TickType_t xTaskGetTickCount(void);
