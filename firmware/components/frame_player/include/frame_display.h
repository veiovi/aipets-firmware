#pragma once
#include "frame_player.h"

/* Nearest-neighbour scaling to a square LCD. display_size is its side in pixels;
 * the caller owns display_size squared RGB565 pixels. Bounds are clipped before
 * arithmetic; empty/unsupported input returns empty. */
fp_dirty_rect_t fp_display_dirty(uint16_t width, uint16_t height, uint16_t display_size,
                                 fp_dirty_rect_t dirty);
void fp_display_expand(const uint16_t *source, uint16_t width, uint16_t height,
                       uint16_t *display, uint16_t display_size, fp_dirty_rect_t dirty);

/* Physical touch position to the player's -256..256 gaze range. */
int16_t fp_display_touch(int32_t pixel, uint16_t display_size);
