#pragma once
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);
