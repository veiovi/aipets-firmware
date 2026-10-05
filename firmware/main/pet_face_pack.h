#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "frame_player.h"
#include "lvgl.h"
#include "pet_animation_profile.h"
#include "pet_face.h"
#include "pet_face_catalog.h"
#include "pet_face_profile.h"

esp_err_t pet_face_pack_create(lv_obj_t *parent, const char *preferred_id,
                               lv_obj_t **root_out);
esp_err_t pet_face_pack_select(const char *id);
esp_err_t pet_face_pack_list(pet_face_catalog_item_t *items, size_t capacity,
                             size_t *count_out);
esp_err_t pet_face_pack_identity(const char *id, pet_face_gender_t *gender_out);
esp_err_t pet_face_pack_current_id(char *id, size_t capacity);
/* Validation scratch is temporary PSRAM, never the caller's task stack. */
fp_error_t pet_face_pack_validate(const void *bytes, uint32_t length, fp_pack_info_t *info);
/* Verified remote pack only: caller owns/pins the mapping for the entire player
 * lifetime. Call under the display lock; replaces the active binding if live.
 * No network/download/signature authority is inferred by this renderer API. */
/* Register an installed pack for display. The caller must already have
 * validated these exact bytes in full (fp_validate_with_workspace); this only
 * inspects the structure, identity and approval, so it is fast enough to run
 * under the display lock. */
esp_err_t pet_face_pack_use_external(const char *id,const void *pack,size_t bytes);
/* Under the display lock, after draining external catalog/network readers:
 * remove every renderer/catalog reference before the owner unmaps or overwrites
 * external bytes. Idempotent; retains the independent canvas allocation. A new
 * verified external pack can subsequently be bound without recreating the UI. */
void pet_face_pack_release_external(void);
/* As release, while the owner replaces the bytes with another pet's: the
 * canvas keeps the last frame and updates succeed without drawing until the
 * next pet_face_pack_use_external, so no emergency face shows in between.
 * A later release ends the hold. */
void pet_face_pack_hold_external(void);

/* Evaluation is always called at 30 Hz. visible controls only the expensive
 * display expansion; clocks, input edges, and clip ownership keep moving. */
esp_err_t pet_face_pack_update(pet_face_state_t state,
                               pet_expression_t expression,
                               uint8_t audio_level,
                               uint8_t viseme,
                               bool pressed,
                               int16_t touch_x_q88,
                               int16_t touch_y_q88,
                               int16_t imu_x_q88,
                               int16_t imu_y_q88,
                               pet_animation_profile_t profile,
                               uint32_t now_ms,
                               bool visible,
                               bool *rendered_out);

uint8_t pet_face_pack_trigger_gesture(uint8_t gesture);
/* The pack's touch reaction, exactly as when the screen is pressed. */
bool pet_face_pack_touch(void);
bool pet_face_pack_gesture_active(void);
/* Embedded frame packs have no staged candidate state. */
bool pet_face_pack_candidate_pending(void);
uint32_t pet_face_pack_frame_crc(void);
