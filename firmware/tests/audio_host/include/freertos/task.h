#pragma once
#include "freertos/FreeRTOS.h"
typedef struct fake_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *arg);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack_depth,
                                   void *arg, UBaseType_t priority, TaskHandle_t *handle,
                                   BaseType_t core);
void vTaskDelay(TickType_t ticks);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
