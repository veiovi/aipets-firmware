#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "pet_face.h"

esp_err_t pet_face_ai_create(lv_obj_t *parent, lv_obj_t **root_out);
void pet_face_ai_render(pet_face_state_t state, pet_expression_t expression,
                        uint8_t audio_level, uint32_t tick_ms, bool pressed);

