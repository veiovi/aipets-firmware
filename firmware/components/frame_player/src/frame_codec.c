#include "frame_internal.h"

static int inflate_exact(fp_codec_workspace_t *work, const uint8_t *data,
                         uint32_t length, uint8_t *out, uint32_t expected)
{
    if (!work || length < 6u || length > FP_MAX_RESOURCE_BYTES + 1024u || !expected || expected > FP_MAX_RESOURCE_BYTES - 4u ||
        (data[0] & 15u) != 8u || (data[0] >> 4u) > 7u || (data[1] & 32u) ||
        (((uint32_t)data[0] * 256u + data[1]) % 31u)) return 0;
    size_t input = length, output = expected;
    tinfl_init(&work->inflater);
    tinfl_status status = tinfl_decompress(&work->inflater, data, &input, out, out,
        &output, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return status == TINFL_STATUS_DONE && input == length && output == expected;
}

const uint8_t *fp_decode_resource(const uint8_t *data, uint32_t length,
    uint8_t codec, uint32_t canvas, fp_codec_workspace_t *work, uint32_t *decoded_length)
{
    *decoded_length = length;
    if (!fp_zlib_codec(codec)) return data;
    if (!work || (canvas != 120u && canvas != 240u)) return 0;
    uint32_t header = 0u, input_at = 0u, expected = canvas * canvas;
    if (codec != FP_CODEC_INDEX8_FULL_ZLIB) {
        if (length < 10u) return 0;
        uint32_t w = data[2], h = data[3];
        if (!w || !h || data[0] + w > canvas || data[1] + h > canvas) return 0;
        header = input_at = 4u; expected = w * h;
        if (codec == FP_CODEC_INDEX8_MASKED_RECT_ZLIB) {
            expected = fp_rd32(data + 4u); input_at = 8u;
            uint32_t mask_bytes = (w * h + 7u) / 8u;
            if (expected < mask_bytes || expected > mask_bytes + w * h) return 0;
        }
        for (uint32_t i = 0; i < 4u; i++) work->decoded[i] = data[i];
    }
    if (!inflate_exact(work, data + input_at, length - input_at, work->decoded + header, expected)) return 0;
    *decoded_length = header + expected;
    return work->decoded;
}

/* The player renders only proven packs: fully validated, or bytes whose
 * SHA-256 still equals bytes that were. Validation already checked each
 * frame's zlib header and Adler-32, so rendering inflates the raw DEFLATE body
 * straight into the caller's buffer: no Adler-32 pass over the output (about a
 * quarter of the inflate time) and no copy out of the workspace. The stream
 * must still end exactly at the trailer and fill exactly canvas * canvas
 * bytes, and the non-wrapping output bound keeps any input memory-safe. */
int fp_inflate_full_frame(const uint8_t *data, uint32_t length, uint32_t canvas,
    fp_codec_workspace_t *work, uint8_t *out)
{
    if (!work || (canvas != 120u && canvas != 240u) || length < 6u || length > FP_MAX_RESOURCE_BYTES + 1024u ||
        (data[0] & 15u) != 8u || (data[0] >> 4u) > 7u || (data[1] & 32u) ||
        (((uint32_t)data[0] * 256u + data[1]) % 31u)) return 0;
    size_t input = length - 6u, output = canvas * canvas;
    tinfl_init(&work->inflater);
    tinfl_status status = tinfl_decompress(&work->inflater, data + 2u, &input, out, out, &output,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return status == TINFL_STATUS_DONE && input == length - 6u && output == canvas * canvas;
}
