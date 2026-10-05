#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
int xTaskCreate(void (*task)(void *), const char *name, uint32_t stack, void *arg,
                unsigned priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
