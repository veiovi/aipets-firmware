#pragma once
/* Host stand-in for the FreeRTOS subset pet_audio.c uses, on POSIX threads
 * (tests/audio_host/fake_rtos.c). One tick is one millisecond, as on the
 * device (CONFIG_FREERTOS_HZ=1000). Priorities are not modelled: every task
 * runs in parallel, which admits more interleavings than the device. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
