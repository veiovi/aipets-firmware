#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
/* The test records critical-section depth to prove flash work stays outside. */
typedef struct { int depth; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void pet_test_critical_enter(portMUX_TYPE *mux);
void pet_test_critical_exit(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) pet_test_critical_enter(mux)
#define portEXIT_CRITICAL(mux) pet_test_critical_exit(mux)
