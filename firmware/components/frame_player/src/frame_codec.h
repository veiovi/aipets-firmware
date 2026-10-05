#pragma once
#include "frame_player.h"
#include "../vendor/miniz/miniz_tinfl.h"

enum { FP_MAX_RESOURCE_BYTES = 4 + FP_MAX_PIXELS + (FP_MAX_PIXELS + 7) / 8 };
typedef struct {
    tinfl_decompressor inflater;
    uint8_t decoded[FP_MAX_RESOURCE_BYTES];
} fp_codec_workspace_t;

const uint8_t *fp_decode_resource(const uint8_t *data, uint32_t length,
    uint8_t codec, uint32_t canvas, fp_codec_workspace_t *workspace,
    uint32_t *decoded_length);

/* Render-time inflate of a whole FP_CODEC_INDEX8_FULL_ZLIB frame straight into
 * `out` (canvas * canvas palette indices). Returns 1 on success. */
int fp_inflate_full_frame(const uint8_t *data, uint32_t length, uint32_t canvas,
    fp_codec_workspace_t *workspace, uint8_t *out);
