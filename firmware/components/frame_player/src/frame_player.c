#include "frame_internal.h"

#if defined(__wasm__)
#define FP_EXPORT(name) __attribute__((export_name(#name)))
#else
#define FP_EXPORT(name)
#endif

#define FP_PLAYER_MAGIC 0x46503150u /* "FP1P" */

static const uint8_t fp_magic[8] = { 0x41, 0x49, 0x50, 0x46, 0x52, 0x41, 0x4d, 0x45 }; /* "AIPFRAME" */

uint16_t fp_rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t fp_rd32(const uint8_t *p) { return (uint32_t)fp_rd16(p) | ((uint32_t)fp_rd16(p + 2) << 16); }

static int fp_range_ok(uint32_t total, uint32_t offset, uint32_t length)
{
    return offset >= FP_HEADER_BYTES && offset <= total && length <= total - offset;
}

static uint32_t fp_crc32_step(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) crc = (crc & 1u) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
    return crc;
}

static uint32_t fp_crc32_bytes(const uint8_t *bytes, uint32_t length)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < length; i++) crc = fp_crc32_step(crc, bytes[i]);
    return crc ^ 0xffffffffu;
}

static uint32_t fp_crc32_header(const uint8_t *bytes)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < FP_HEADER_BYTES; i++) {
        uint8_t byte = (i >= FP_HDR_HEADER_CRC32 && i < FP_HDR_HEADER_CRC32 + 4u) ? 0u : bytes[i];
        crc = fp_crc32_step(crc, byte);
    }
    return crc ^ 0xffffffffu;
}

static void fp_zero_bytes(void *target, uint32_t length)
{
    uint8_t *bytes = (uint8_t *)target;
    for (uint32_t i = 0; i < length; i++) bytes[i] = 0;
}

/* Called only after the frame and clip directories have been validated. */
static int fp_full_frame(const uint8_t *pack, uint32_t frame_dir, uint16_t frame)
{
    uint8_t codec = pack[frame_dir + (uint32_t)frame * FP_FRAME_DIR_ENTRY_BYTES + 8u];
    return fp_full_codec(codec);
}

static int fp_full_clip(const uint8_t *pack, uint32_t frame_dir, uint32_t clip_dir, uint16_t clip)
{
    const uint8_t *entry = pack + clip_dir + (uint32_t)clip * FP_CLIP_DIR_ENTRY_BYTES;
    uint32_t steps = fp_rd32(entry + 8u), count = fp_rd16(entry + 12u);
    for (uint32_t i = 0; i < count; i++)
        if (!fp_full_frame(pack, frame_dir, fp_rd16(pack + steps + i * FP_CLIP_STEP_BYTES))) return 0;
    return 1;
}

static uint32_t fp_clip_ticks(const uint8_t *pack, uint32_t clip_dir, uint32_t clip)
{
    const uint8_t *entry = pack + clip_dir + clip * FP_CLIP_DIR_ENTRY_BYTES;
    uint32_t steps = fp_rd32(entry + 8u), count = fp_rd16(entry + 12u), ticks = 0u;
    for (uint32_t i = 0; i < count; i++) ticks += fp_rd16(pack + steps + i * FP_CLIP_STEP_BYTES + 2u);
    return ticks;
}

/* String ref must lie fully inside the strings section. */
static int fp_string_ok(uint32_t strings_offset, uint32_t strings_length, uint32_t offset, uint32_t length)
{
    if (length == 0u) return 1;
    return offset >= strings_offset && offset - strings_offset <= strings_length &&
           length <= strings_length - (offset - strings_offset);
}

/*
 * Full structural validation, mirroring the frame-pack compiler's TypeScript
 * parseFramePack check for check (same order, same error
 * for the same defect). After success, rendering any frame of any clip
 * cannot fault.
 */
FP_EXPORT(fp_validate)
fp_error_t fp_validate(const void *pack_bytes_in, uint32_t pack_bytes, fp_pack_info_t *info)
{
    return fp_validate_with_workspace(pack_bytes_in, pack_bytes, info, 0, 0);
}

FP_EXPORT(fp_validation_workspace_size)
uint32_t fp_validation_workspace_size(void) { return ((uint32_t)sizeof(fp_codec_workspace_t) + 15u) & ~15u; }

/* With `prove`, every byte is checked: the payload CRC and each compressed
 * frame, inflated. Without it, the structure and the uncompressed frames are
 * checked (the renderer reads those directly from the pack) but compressed
 * frames are not inflated. Rendering checks each compressed frame as it
 * decodes it into the fixed-size workspace, so bytes that would fail the full
 * check stay memory-safe; they can only render wrong. */
static fp_error_t fp_check_pack(const void *pack_bytes_in, uint32_t pack_bytes,
    fp_pack_info_t *info, fp_codec_workspace_t *work, int prove)
{
    const uint8_t *pack = (const uint8_t *)pack_bytes_in;
    if (pack == 0) return FP_ERR_ARGUMENT;
    if (pack_bytes < FP_HEADER_BYTES) return FP_ERR_TRUNCATED;
    for (uint32_t i = 0; i < 8u; i++) {
        if (pack[i] != fp_magic[i]) return FP_ERR_BAD_MAGIC;
    }
    uint16_t format_version = fp_rd16(pack + FP_HDR_VERSION);
    if (format_version != FP_PACK_VERSION && format_version != FP_PACK_VERSION_240) return FP_ERR_UNSUPPORTED_VERSION;
    uint32_t canvas = format_version == FP_PACK_VERSION_240 ? 240u : 120u;
    if (fp_rd16(pack + FP_HDR_HEADER_BYTES) != FP_HEADER_BYTES) return FP_ERR_BAD_HEADER;
    if (fp_rd16(pack + FP_HDR_CANVAS_W) != canvas) return FP_ERR_BAD_HEADER;
    if (fp_rd16(pack + FP_HDR_CANVAS_H) != canvas) return FP_ERR_BAD_HEADER;
    uint32_t file_length = fp_rd32(pack + FP_HDR_FILE_LENGTH);
    if (file_length != pack_bytes) return FP_ERR_TRUNCATED;
    if (file_length > (format_version == FP_PACK_VERSION_240 ? FP_PACK_MAX_BYTES_240 : FP_PACK_MAX_BYTES)) return FP_ERR_BAD_HEADER;
    if (fp_rd16(pack + FP_HDR_TICK_MS) != FP_TICK_MS) return FP_ERR_BAD_HEADER;
    if (fp_crc32_header(pack) != fp_rd32(pack + FP_HDR_HEADER_CRC32)) return FP_ERR_BAD_CHECKSUM;
    uint32_t payload_crc = fp_rd32(pack + FP_HDR_PAYLOAD_CRC32);
    if (prove && fp_crc32_bytes(pack + FP_HEADER_BYTES, file_length - FP_HEADER_BYTES) != payload_crc) {
        return FP_ERR_BAD_CHECKSUM;
    }
    uint16_t features = fp_rd16(pack + FP_HDR_FLAGS);
    if ((features & ~(FP_FLAG_APPROVED | FP_FEATURE_OVERLAY_ACTIONS | FP_FEATURE_FACIAL_REGIONS | FP_FEATURE_ACTION_LIFECYCLE | FP_FEATURE_PERFORMANCE_ACTIONS | FP_FEATURE_SPEAKING_POSES | FP_FEATURE_MOVE)) != 0u) return FP_ERR_BAD_HEADER;
    /* A move pack is a 240px pack of at most 300,000 bytes whose only other
     * feature is its optional effect (binary.ts parseFramePack). */
    int move = (features & FP_FEATURE_MOVE) != 0u;
    if (move && (canvas != 240u || file_length > FP_MOVE_MAX_BYTES ||
                 (features & ~(FP_FLAG_APPROVED | FP_FEATURE_MOVE | FP_FEATURE_OVERLAY_ACTIONS)) != 0u)) return FP_ERR_BAD_MOVE;
    for (uint32_t i = FP_HDR_SPEAKING_LENGTH + 2u; i < FP_HEADER_BYTES; i++) {
        if (pack[i] != 0u) return FP_ERR_BAD_HEADER;
    }

    uint32_t palette_offset = fp_rd32(pack + FP_HDR_PALETTE_OFFSET);
    uint32_t palette_count = fp_rd16(pack + FP_HDR_PALETTE_COUNT);
    if (palette_count < 1u || palette_count > FP_MAX_PALETTE) return FP_ERR_BAD_HEADER;
    if (!fp_range_ok(file_length, palette_offset, palette_count * 2u)) return FP_ERR_BAD_BOUNDS;
    uint8_t bg_index = pack[FP_HDR_BG_PALETTE_INDEX];
    if (bg_index >= palette_count) return FP_ERR_BAD_HEADER;
    uint8_t gender = pack[FP_HDR_GENDER];
    if (gender > 3u) return FP_ERR_BAD_HEADER;

    uint32_t frame_count = fp_rd16(pack + FP_HDR_FRAME_COUNT);
    if (frame_count < 1u || frame_count > (format_version == FP_PACK_VERSION_240 ? FP_MAX_FRAMES : 640u)) return FP_ERR_BAD_HEADER;
    if (move && frame_count > FP_MOVE_MAX_FRAMES) return FP_ERR_BAD_MOVE;
    uint32_t frame_dir = fp_rd32(pack + FP_HDR_FRAME_DIR_OFFSET);
    if (!fp_range_ok(file_length, frame_dir, frame_count * FP_FRAME_DIR_ENTRY_BYTES)) return FP_ERR_BAD_BOUNDS;
    uint8_t codecs = 0;
    for (uint32_t i = 0; i < frame_count; i++) {
        const uint8_t *entry = pack + frame_dir + i * FP_FRAME_DIR_ENTRY_BYTES;
        uint32_t offset = fp_rd32(entry);
        uint32_t length = fp_rd32(entry + 4);
        uint8_t codec = entry[8];
        if (codec > FP_CODEC_INDEX8_DENSE_RECT_ZLIB || (format_version == FP_PACK_VERSION && fp_zlib_codec(codec))) return FP_ERR_BAD_FRAME;
        codecs |= (uint8_t)(1u << codec);
        if (codec == FP_CODEC_INDEX8_FULL && length != canvas * canvas) return FP_ERR_BAD_FRAME;
        if (!fp_range_ok(file_length, offset, length)) return FP_ERR_BAD_BOUNDS;
        const uint8_t *pixels = pack + offset;
        if (fp_zlib_codec(codec)) {
            if (!prove) continue;
            if (!work) return FP_ERR_ARENA;
            pixels = fp_decode_resource(pixels, length, codec, canvas, work, &length);
            if (!pixels) return FP_ERR_BAD_FRAME;
            codec = codec == FP_CODEC_INDEX8_FULL_ZLIB ? FP_CODEC_INDEX8_FULL :
                    codec == FP_CODEC_INDEX8_MASKED_RECT_ZLIB ? FP_CODEC_INDEX8_MASKED_RECT : FP_CODEC_INDEX8_DENSE_RECT;
        }
        if (codec == FP_CODEC_INDEX8_FULL) {
            for (uint32_t p = 0; p < canvas * canvas; p++) {
                if (pixels[p] >= palette_count) return FP_ERR_BAD_FRAME;
            }
        } else if (codec == FP_CODEC_INDEX8_FULL_RLE) {
            if (length < 2u || (length & 1u)) return FP_ERR_BAD_FRAME;
            uint32_t expanded = 0u;
            for (uint32_t p = 0; p < length; p += 2u) {
                if (pixels[p] == 0u || pixels[p + 1u] >= palette_count) return FP_ERR_BAD_FRAME;
                expanded += pixels[p];
            }
            if (expanded != canvas * canvas) return FP_ERR_BAD_FRAME;
        } else if (codec == FP_CODEC_INDEX8_MASKED_RECT || codec == FP_CODEC_INDEX8_MASKED_RECT_RLE) {
            if (length < 5u) return FP_ERR_BAD_FRAME;
            uint32_t x = pixels[0], y = pixels[1], w = pixels[2], h = pixels[3];
            if (w < 1u || h < 1u || x + w > canvas || y + h > canvas) return FP_ERR_BAD_FRAME;
            uint32_t mask_bytes = (w * h + 7u) / 8u;
            if (length < 4u + mask_bytes) return FP_ERR_BAD_FRAME;
            uint32_t opaque = 0u;
            for (uint32_t p = 0; p < w * h; p++) opaque += (pixels[4u + (p >> 3)] >> (p & 7u)) & 1u;
            uint32_t colors = 4u + mask_bytes;
            if (codec == FP_CODEC_INDEX8_MASKED_RECT) {
                if (length != colors + opaque) return FP_ERR_BAD_FRAME;
                for (uint32_t p = colors; p < length; p++) if (pixels[p] >= palette_count) return FP_ERR_BAD_FRAME;
            } else {
                if (length <= colors || ((length - colors) & 1u)) return FP_ERR_BAD_FRAME;
                uint32_t expanded = 0u;
                for (uint32_t p = colors; p < length; p += 2u) {
                    if (pixels[p] == 0u || pixels[p + 1u] >= palette_count) return FP_ERR_BAD_FRAME;
                    expanded += pixels[p];
                }
                if (expanded != opaque) return FP_ERR_BAD_FRAME;
            }
        } else { if(length<5u)return FP_ERR_BAD_FRAME;uint32_t x=pixels[0],y=pixels[1],w=pixels[2],h=pixels[3]; if(!w||!h||x+w>canvas||y+h>canvas||length!=4u+w*h)return FP_ERR_BAD_FRAME;for(uint32_t p=4;p<length;p++)if(pixels[p]>=palette_count)return FP_ERR_BAD_FRAME; }
    }

    uint32_t strings_offset = fp_rd32(pack + FP_HDR_STRINGS_OFFSET);
    uint32_t strings_length = fp_rd32(pack + FP_HDR_STRINGS_LENGTH);
    if (strings_length > 0u && !fp_range_ok(file_length, strings_offset, strings_length)) return FP_ERR_BAD_BOUNDS;

    uint32_t clip_count = fp_rd16(pack + FP_HDR_CLIP_COUNT);
    if (clip_count < 1u || clip_count > FP_MAX_CLIPS) return FP_ERR_BAD_HEADER;
    uint32_t clip_dir = fp_rd32(pack + FP_HDR_CLIP_DIR_OFFSET);
    if (!fp_range_ok(file_length, clip_dir, clip_count * FP_CLIP_DIR_ENTRY_BYTES)) return FP_ERR_BAD_BOUNDS;
    for (uint32_t i = 0; i < clip_count; i++) {
        const uint8_t *entry = pack + clip_dir + i * FP_CLIP_DIR_ENTRY_BYTES;
        if (!fp_string_ok(strings_offset, strings_length, fp_rd32(entry), fp_rd16(entry + 4))) {
            return FP_ERR_BAD_BOUNDS;
        }
        uint8_t loop_mode = entry[6];
        if (loop_mode > FP_LOOP_HOLD_LAST) return FP_ERR_BAD_CLIP;
        uint32_t steps_offset = fp_rd32(entry + 8);
        uint32_t step_count = fp_rd16(entry + 12);
        if (step_count < 1u || step_count > FP_MAX_CLIP_STEPS) return FP_ERR_BAD_CLIP;
        if (!fp_range_ok(file_length, steps_offset, step_count * FP_CLIP_STEP_BYTES)) return FP_ERR_BAD_BOUNDS;
        for (uint32_t s = 0; s < step_count; s++) {
            const uint8_t *step = pack + steps_offset + s * FP_CLIP_STEP_BYTES;
            if (fp_rd16(step) >= frame_count) return FP_ERR_BAD_CLIP;
            if (fp_rd16(step + 2) < 1u) return FP_ERR_BAD_CLIP;
        }
    }

    /* A move pack has no roles, emotions, talk table or idle section. */
    uint32_t emotion_count = 0u, talk_offset = 0u, stage_count = 0u, micro_count = 0u;
    if (move) {
        if (fp_rd32(pack + FP_HDR_ROLES_OFFSET) || fp_rd16(pack + FP_HDR_EMOTION_COUNT) || fp_rd32(pack + FP_HDR_EMOTIONS_OFFSET) ||
            fp_rd32(pack + FP_HDR_TALK_OFFSET) || fp_rd32(pack + FP_HDR_IDLE_OFFSET)) return FP_ERR_BAD_MOVE;
    } else {
        uint32_t roles_offset = fp_rd32(pack + FP_HDR_ROLES_OFFSET);
        if (!fp_range_ok(file_length, roles_offset, FP_ROLE_COUNT * 2u)) return FP_ERR_BAD_BOUNDS;
        for (uint32_t i = 0; i < FP_ROLE_COUNT; i++) {
            uint16_t role = fp_rd16(pack + roles_offset + i * 2u);
            if (role != FP_ROLE_ABSENT && role >= clip_count) return FP_ERR_BAD_ROLE;
            if (role != FP_ROLE_ABSENT && !fp_full_clip(pack, frame_dir, clip_dir, role)) return FP_ERR_BAD_ROLE;
        }
        if (fp_rd16(pack + roles_offset + FP_ROLE_IDLE_NEUTRAL * 2u) == FP_ROLE_ABSENT) return FP_ERR_BAD_ROLE;

        emotion_count = fp_rd16(pack + FP_HDR_EMOTION_COUNT);
        if (emotion_count > FP_MAX_EMOTIONS) return FP_ERR_BAD_EMOTION;
        uint32_t emotions_offset = fp_rd32(pack + FP_HDR_EMOTIONS_OFFSET);
        if (emotion_count > 0u && !fp_range_ok(file_length, emotions_offset, emotion_count * FP_EMOTION_ENTRY_BYTES)) {
            return FP_ERR_BAD_BOUNDS;
        }
        for (uint32_t i = 0; i < emotion_count; i++) {
            const uint8_t *entry = pack + emotions_offset + i * FP_EMOTION_ENTRY_BYTES;
            uint8_t id = entry[0];
            if (id == 0u || id > FP_MAX_EMOTION_ID) return FP_ERR_BAD_EMOTION;
            for (uint32_t j = 0; j < i; j++) {
                if (pack[emotions_offset + j * FP_EMOTION_ENTRY_BYTES] == id) return FP_ERR_BAD_EMOTION;
            }
            if (!fp_string_ok(strings_offset, strings_length, fp_rd32(entry + 4), fp_rd16(entry + 2))) {
                return FP_ERR_BAD_BOUNDS;
            }
            if (fp_rd16(entry + 8) >= clip_count) return FP_ERR_BAD_EMOTION;
            if (fp_rd16(entry + 10) >= clip_count) return FP_ERR_BAD_EMOTION;
            if (fp_rd16(entry + 12) >= clip_count) return FP_ERR_BAD_EMOTION;
            for (uint32_t at = 8; at <= 12; at += 2)
                if (!fp_full_clip(pack, frame_dir, clip_dir, fp_rd16(entry + at))) return FP_ERR_BAD_EMOTION;
            if (fp_rd16(entry + 14) > fp_rd16(entry + 16)) return FP_ERR_BAD_EMOTION;
            /* Non-terminating enter/exit clips would wedge the director. */
            if (pack[clip_dir + (uint32_t)fp_rd16(entry + 8) * FP_CLIP_DIR_ENTRY_BYTES + 6u] != FP_LOOP_ONCE) {
                return FP_ERR_BAD_EMOTION;
            }
            if (pack[clip_dir + (uint32_t)fp_rd16(entry + 12) * FP_CLIP_DIR_ENTRY_BYTES + 6u] != FP_LOOP_ONCE) {
                return FP_ERR_BAD_EMOTION;
            }
        }

        talk_offset = fp_rd32(pack + FP_HDR_TALK_OFFSET);
        if (!fp_range_ok(file_length, talk_offset, 4u)) return FP_ERR_BAD_BOUNDS;
        stage_count = pack[talk_offset];
        if (stage_count < 1u || stage_count > FP_MAX_TALK_STAGES) return FP_ERR_BAD_TALK;
        uint32_t talk_stage_bytes = (4u + stage_count * 2u + 3u) & ~3u;
        uint32_t talk_lut_bytes = (FP_TALK_LUT_BYTES + 3u) & ~3u;
        if (!fp_range_ok(file_length, talk_offset, talk_stage_bytes + talk_lut_bytes)) return FP_ERR_BAD_BOUNDS;
        for (uint32_t i = 0; i < stage_count; i++) {
            if (fp_rd16(pack + talk_offset + 4u + i * 2u) >= frame_count) return FP_ERR_BAD_TALK;
            if (!fp_full_frame(pack, frame_dir, fp_rd16(pack + talk_offset + 4u + i * 2u))) return FP_ERR_BAD_TALK;
        }
        for (uint32_t level = 0; level < FP_TALK_LUT_BYTES; level++) {
            if (pack[talk_offset + talk_stage_bytes + level] >= stage_count) return FP_ERR_BAD_TALK;
        }

        uint32_t idle_offset = fp_rd32(pack + FP_HDR_IDLE_OFFSET);
        if (!fp_range_ok(file_length, idle_offset, FP_IDLE_PARAMS_BYTES)) return FP_ERR_BAD_BOUNDS;
        uint16_t bob_amplitude = fp_rd16(pack + idle_offset);
        uint16_t bob_period = fp_rd16(pack + idle_offset + 2u);
        uint16_t micro_gap_min = fp_rd16(pack + idle_offset + 4u);
        uint16_t micro_gap_max = fp_rd16(pack + idle_offset + 6u);
        uint32_t micro_list_offset = fp_rd32(pack + idle_offset + 8u);
        micro_count = fp_rd16(pack + idle_offset + 12u);
        uint8_t motion_flags = pack[idle_offset + 14u];
        uint8_t max_parallax_px = pack[idle_offset + 15u];
        if ((motion_flags & ~3u) != 0u || max_parallax_px > 1u || (max_parallax_px > 0u && (motion_flags & 2u) == 0u)) {
            return FP_ERR_BAD_IDLE;
        }
        if (bob_amplitude > 2u) return FP_ERR_BAD_IDLE;
        if (bob_amplitude > 0u && bob_period < 2u) return FP_ERR_BAD_IDLE;
        if (micro_count > 0u && micro_gap_min >= micro_gap_max) return FP_ERR_BAD_IDLE;
        if (micro_count > FP_MAX_MICRO) return FP_ERR_BAD_IDLE;
        if (micro_count > 0u && !fp_range_ok(file_length, micro_list_offset, micro_count * FP_MICRO_ENTRY_BYTES)) {
            return FP_ERR_BAD_BOUNDS;
        }
        uint32_t micro_weight_total = 0;
        for (uint32_t i = 0; i < micro_count; i++) {
            const uint8_t *entry = pack + micro_list_offset + i * FP_MICRO_ENTRY_BYTES;
            if (fp_rd16(entry) >= clip_count) return FP_ERR_BAD_IDLE;
            if (!fp_full_clip(pack, frame_dir, clip_dir, fp_rd16(entry))) return FP_ERR_BAD_IDLE;
            uint16_t weight = fp_rd16(entry + 2);
            if (weight < 1u) return FP_ERR_BAD_IDLE;
            if (pack[clip_dir + (uint32_t)fp_rd16(entry) * FP_CLIP_DIR_ENTRY_BYTES + 6u] != FP_LOOP_ONCE) {
                return FP_ERR_BAD_IDLE;
            }
            micro_weight_total += weight;
        }
        if (micro_weight_total > 0xffffu) return FP_ERR_BAD_IDLE;
    }

    uint32_t action_count = fp_rd16(pack + FP_HDR_OVERLAY_COUNT);
    if (action_count > (format_version == FP_PACK_VERSION_240 ? FP_MAX_ACTIONS : 32u)) return FP_ERR_BAD_OVERLAY;
    if (action_count > 0u && (features & FP_FEATURE_OVERLAY_ACTIONS) == 0u) return FP_ERR_BAD_OVERLAY;
    if (action_count == 0u && (features & FP_FEATURE_OVERLAY_ACTIONS) != 0u) return FP_ERR_BAD_OVERLAY;
    uint32_t action_dir = fp_rd32(pack + FP_HDR_OVERLAY_DIR_OFFSET);
    if (action_count > 0u && !fp_range_ok(file_length, action_dir, action_count * FP_ACTION_ENTRY_BYTES)) {
        return FP_ERR_BAD_BOUNDS;
    }
    uint32_t base_actions = 0, effect_actions = 0;
    for (uint32_t i = 0; i < action_count; i++) {
        const uint8_t *a = pack + action_dir + i * FP_ACTION_ENTRY_BYTES;
        if (a[1] == FP_PLANE_BASE) base_actions++; else effect_actions++;
        if (format_version == FP_PACK_VERSION_240 && (base_actions > 64u || effect_actions > 8u)) return FP_ERR_BAD_OVERLAY;
        if (a[0] > FP_SEM_GESTURE_POP || a[1] >= FP_PLANE_COUNT || a[2] > (format_version==FP_PACK_VERSION_240?FP_ACTIVATION_MANUAL:FP_ACTIVATION_AMBIENT_EVENT)) return FP_ERR_BAD_OVERLAY;
        if(a[2]==FP_ACTIVATION_MANUAL&&a[1]!=FP_PLANE_BASE)return FP_ERR_BAD_OVERLAY;
        uint16_t clip = fp_rd16(a + 4u), exit_clip = fp_rd16(a + 6u), flags = fp_rd16(a + 8u);
        uint16_t enter_clip = (features & FP_FEATURE_ACTION_LIFECYCLE) != 0u ? fp_rd16(a + 20u) : FP_ACTION_NO_CLIP;
        uint16_t performance_mask = (features & FP_FEATURE_PERFORMANCE_ACTIONS) != 0u ? fp_rd16(a + 22u) : 0u;
        uint32_t valid_performance_mask = micro_count == 16u ? 0xffffu : ((1u << micro_count) - 1u);
        if (((uint32_t)performance_mask & ~valid_performance_mask) != 0u) return FP_ERR_BAD_OVERLAY;
        if (clip >= clip_count || (enter_clip != FP_ACTION_NO_CLIP && enter_clip >= clip_count) || (exit_clip != FP_ACTION_NO_CLIP && exit_clip >= clip_count)) return FP_ERR_BAD_OVERLAY;
        if ((flags & FP_ACTION_PROFILE_MASK) == 0u || (flags & ~(FP_ACTION_PROFILE_MASK | FP_ACTION_FLAG_ESSENTIAL | FP_ACTION_FLAG_CENTER_ONLY)) != 0u) {
            return FP_ERR_BAD_OVERLAY;
        }
        if (fp_rd16(a + 10u) == 0u || fp_rd16(a + 12u) == 0u ||
            fp_rd16(a + 14u) > fp_rd16(a + 16u)) return FP_ERR_BAD_OVERLAY;
        if (enter_clip != FP_ACTION_NO_CLIP && pack[clip_dir + (uint32_t)enter_clip * FP_CLIP_DIR_ENTRY_BYTES + 6u] != FP_LOOP_ONCE) return FP_ERR_BAD_OVERLAY;
        /* Nothing interrupts an exit clip, so one that never ends would hold
         * its plane forever. */
        if (exit_clip != FP_ACTION_NO_CLIP && pack[clip_dir + (uint32_t)exit_clip * FP_CLIP_DIR_ENTRY_BYTES + 6u] != FP_LOOP_ONCE) return FP_ERR_BAD_OVERLAY;
        uint32_t reserved_at = (features & FP_FEATURE_PERFORMANCE_ACTIONS) != 0u ? 24u :
            (features & FP_FEATURE_ACTION_LIFECYCLE) != 0u ? 22u : 20u;
        for (uint32_t r = reserved_at; r < FP_ACTION_ENTRY_BYTES; r++) if (a[r] != 0u) return FP_ERR_BAD_OVERLAY;
        uint16_t clips_to_check[3] = { clip, enter_clip, exit_clip };
        for (uint32_t c = 0; c < 3u; c++) {
            if (clips_to_check[c] == FP_ACTION_NO_CLIP) continue;
            uint16_t steps = fp_rd16(pack + clip_dir + (uint32_t)clips_to_check[c] * FP_CLIP_DIR_ENTRY_BYTES + 12u);
            uint32_t step_offset = fp_rd32(pack + clip_dir + (uint32_t)clips_to_check[c] * FP_CLIP_DIR_ENTRY_BYTES + 8u);
            for (uint32_t s = 0; s < steps; s++) {
                uint16_t frame = fp_rd16(pack + step_offset + s * FP_CLIP_STEP_BYTES);
                uint8_t codec = pack[frame_dir + (uint32_t)frame * FP_FRAME_DIR_ENTRY_BYTES + 8u];
                int valid = a[1] == FP_PLANE_BASE
                    ? fp_full_codec(codec)
                    : fp_masked_codec(codec);
                if (!valid) return FP_ERR_BAD_OVERLAY;
            }
        }
    }
    if (move) {
        /* Clip 0 plays once, in complete frames; an optional characterFx
         * effect plays clip 1 alongside it for exactly as long. */
        uint32_t ticks = fp_clip_ticks(pack, clip_dir, 0u);
        if (action_count > 1u || clip_count != 1u + action_count || pack[clip_dir + 6u] != FP_LOOP_ONCE ||
            !fp_full_clip(pack, frame_dir, clip_dir, 0u) || ticks < FP_MOVE_MIN_TICKS || ticks > FP_MOVE_MAX_TICKS) return FP_ERR_BAD_MOVE;
        if (action_count == 1u && (pack[action_dir + 1u] != FP_PLANE_CHARACTER_FX || fp_rd16(pack + action_dir + 4u) != 1u ||
            fp_rd16(pack + action_dir + 6u) != FP_ACTION_NO_CLIP || pack[clip_dir + FP_CLIP_DIR_ENTRY_BYTES + 6u] != FP_LOOP_ONCE ||
            fp_clip_ticks(pack, clip_dir, 1u) != ticks)) return FP_ERR_BAD_MOVE;
    }

    uint32_t facial_offset=fp_rd32(pack+FP_HDR_FACIAL_OFFSET),facial_length=fp_rd32(pack+FP_HDR_FACIAL_LENGTH);
    /* The talk stage selects one of the seven mouth replacements, and mouth
     * and eye patches must not overlap (binary.ts parseFramePack). */
    if((features&FP_FEATURE_FACIAL_REGIONS)!=0u){if(facial_length<FP_FACIAL_HEADER_BYTES)return FP_ERR_BAD_FACIAL;if(!fp_range_ok(file_length,facial_offset,facial_length))return FP_ERR_BAD_BOUNDS;if(fp_rd16(pack+facial_offset)!=1u&&(format_version!=FP_PACK_VERSION_240||fp_rd16(pack+facial_offset)!=2u))return FP_ERR_BAD_FACIAL;if(stage_count>FP_FACIAL_MOUTH_STAGES)return FP_ERR_BAD_FACIAL;uint32_t count=pack[facial_offset+2u],center=pack[facial_offset+3u],dir=fp_rd32(pack+facial_offset+52u);if(count!=5u||center>=count||facial_length!=FP_FACIAL_HEADER_BYTES+count*FP_FACIAL_GROUP_BYTES||dir!=facial_offset+FP_FACIAL_HEADER_BYTES)return FP_ERR_BAD_FACIAL;if(fp_rd16(pack+facial_offset)==2u)for(uint32_t j=0;j<5u;j++)if(!pack[facial_offset+20u+j]||pack[facial_offset+20u+j]>10u)return FP_ERR_BAD_FACIAL;uint8_t roles=0;for(uint32_t i=0;i<count;i++){const uint8_t*g=pack+dir+i*FP_FACIAL_GROUP_BYTES;uint8_t role=g[0];if(role>4u||(roles&(1u<<role)))return FP_ERR_BAD_FACIAL;roles|=1u<<role;uint16_t base=fp_rd16(g+2u);uint8_t base_codec=base<frame_count?pack[frame_dir+(uint32_t)base*FP_FRAME_DIR_ENTRY_BYTES+8u]:255u;if(base>=frame_count||!fp_full_codec(base_codec))return FP_ERR_BAD_FACIAL;const uint8_t*mouth=g+4u,*eyes=g+8u;if(mouth[0]<eyes[0]+eyes[2]&&mouth[0]+mouth[2]>eyes[0]&&mouth[1]<eyes[1]+eyes[3]&&mouth[1]+mouth[3]>eyes[1])return FP_ERR_BAD_FACIAL;for(uint32_t bank=0;bank<2;bank++){const uint8_t*r=g+4u+bank*4u;uint32_t n=bank?5u:7u,start=bank?26u:12u;if(!r[2]||!r[3]||r[0]+r[2]>canvas||r[1]+r[3]>canvas)return FP_ERR_BAD_FACIAL;for(uint32_t j=0;j<n;j++){uint16_t f=fp_rd16(g+start+j*2u);if(f>=frame_count||!fp_dense_codec(pack[frame_dir+(uint32_t)f*FP_FRAME_DIR_ENTRY_BYTES+8u]))return FP_ERR_BAD_FACIAL;uint32_t o=fp_rd32(pack+frame_dir+(uint32_t)f*FP_FRAME_DIR_ENTRY_BYTES);if(pack[o]!=r[0]||pack[o+1]!=r[1]||pack[o+2]!=r[2]||pack[o+3]!=r[3])return FP_ERR_BAD_FACIAL;}}}for(uint32_t i=0;i<6;i+=2)if(fp_rd16(pack+facial_offset+8u+i*2u)>=fp_rd16(pack+facial_offset+10u+i*2u))return FP_ERR_BAD_FACIAL;if(fp_rd16(pack+facial_offset)==2u){for(uint32_t i=0;i<8;i++){uint16_t t=fp_rd16(pack+facial_offset+36u+i*2u);if(i<5u?(!t||t>1818u):t!=0u)return FP_ERR_BAD_FACIAL;}}else for(uint32_t i=0;i<8;i+=2)if(fp_rd16(pack+facial_offset+36u+i*2u)>=fp_rd16(pack+facial_offset+38u+i*2u))return FP_ERR_BAD_FACIAL;}else if(facial_offset||facial_length)return FP_ERR_BAD_FACIAL;

    /* Speaking poses (binary.ts parseFramePack, same check order). */
    uint32_t speaking_offset = fp_rd32(pack + FP_HDR_SPEAKING_OFFSET), speaking_length = fp_rd16(pack + FP_HDR_SPEAKING_LENGTH);
    if ((features & FP_FEATURE_SPEAKING_POSES) != 0u) {
        if (canvas != 240u || (features & FP_FEATURE_FACIAL_REGIONS) != 0u) return FP_ERR_BAD_SPEAKING;
        if (speaking_length < FP_SPEAKING_HEADER_BYTES) return FP_ERR_BAD_SPEAKING;
        if (!fp_range_ok(file_length, speaking_offset, speaking_length)) return FP_ERR_BAD_BOUNDS;
        const uint8_t *section = pack + speaking_offset;
        uint32_t poses = section[1], stages = section[2], gestures = section[3], auto_mask = section[4];
        if (section[0] != FP_SPEAKING_SECTION_VERSION) return FP_ERR_BAD_SPEAKING;
        if (section[5] != 0u || fp_rd16(section + 10u) != 0u) return FP_ERR_BAD_SPEAKING;
        if (poses < FP_SPEAKING_MIN_POSES || poses > FP_SPEAKING_MAX_POSES) return FP_ERR_BAD_SPEAKING;
        if (stages != stage_count) return FP_ERR_BAD_SPEAKING;
        if (gestures < 1u || gestures > FP_SPEAKING_MAX_GESTURES) return FP_ERR_BAD_SPEAKING;
        if (auto_mask == 0u || (auto_mask >> gestures) != 0u) return FP_ERR_BAD_SPEAKING;
        uint32_t break_min = fp_rd16(section + 6u), break_max = fp_rd16(section + 8u);
        if (break_min < 1u || break_min > break_max || break_max > FP_SPEAKING_MAX_BREAK_TICKS) return FP_ERR_BAD_SPEAKING;
        uint32_t frame_bytes = (poses * stages * 2u + 3u) & ~3u;
        if (speaking_length != FP_SPEAKING_HEADER_BYTES + frame_bytes + gestures * FP_SPEAKING_GESTURE_BYTES) return FP_ERR_BAD_SPEAKING;
        for (uint32_t pose = 0; pose < poses; pose++) {
            for (uint32_t stage = 0; stage < stages; stage++) {
                uint16_t frame = fp_rd16(section + FP_SPEAKING_HEADER_BYTES + (pose * stages + stage) * 2u);
                if (frame >= frame_count || !fp_full_frame(pack, frame_dir, frame)) return FP_ERR_BAD_SPEAKING;
                if (pose == 0u && frame != fp_rd16(pack + talk_offset + 4u + stage * 2u)) return FP_ERR_BAD_SPEAKING;
            }
        }
        for (uint32_t i = FP_SPEAKING_HEADER_BYTES + poses * stages * 2u; i < FP_SPEAKING_HEADER_BYTES + frame_bytes; i++) {
            if (section[i] != 0u) return FP_ERR_BAD_SPEAKING;
        }
        for (uint32_t g = 0; g < gestures; g++) {
            const uint8_t *gesture = section + FP_SPEAKING_HEADER_BYTES + frame_bytes + g * FP_SPEAKING_GESTURE_BYTES;
            uint32_t steps = gesture[1];
            if (gesture[0] >= FP_SPEAKING_GESTURE_KINDS || steps < FP_SPEAKING_MIN_GESTURE_STEPS || steps > FP_SPEAKING_MAX_GESTURE_STEPS) {
                return FP_ERR_BAD_SPEAKING;
            }
            for (uint32_t j = 0; j < FP_SPEAKING_MAX_GESTURE_STEPS; j++) {
                uint8_t pose = gesture[2u + j * 2u], ticks = gesture[3u + j * 2u];
                if (j < steps ? (pose >= poses || ticks < 1u) : (pose != 0u || ticks != 0u)) return FP_ERR_BAD_SPEAKING;
            }
            if (gesture[2] != 0u || gesture[2u + (steps - 1u) * 2u] != 0u) return FP_ERR_BAD_SPEAKING;
            if (gesture[18] != 0u || gesture[19] != 0u) return FP_ERR_BAD_SPEAKING;
        }
    } else if (speaking_offset != 0u || speaking_length != 0u) {
        return FP_ERR_BAD_SPEAKING;
    }

    uint32_t meta_offset = fp_rd32(pack + FP_HDR_META_OFFSET);
    uint32_t meta_length = fp_rd32(pack + FP_HDR_META_LENGTH);
    if (meta_length > 0u && !fp_range_ok(file_length, meta_offset, meta_length)) return FP_ERR_BAD_BOUNDS;

    uint32_t id_offset = fp_rd32(pack + FP_HDR_ID_STR_OFFSET);
    uint16_t id_len = fp_rd16(pack + FP_HDR_ID_STR_LEN);
    uint32_t name_offset = fp_rd32(pack + FP_HDR_NAME_STR_OFFSET);
    uint16_t name_len = fp_rd16(pack + FP_HDR_NAME_STR_LEN);
    uint32_t version_offset = fp_rd32(pack + FP_HDR_VERSION_STR_OFFSET);
    uint16_t version_len = fp_rd16(pack + FP_HDR_VERSION_STR_LEN);
    if (!fp_string_ok(strings_offset, strings_length, id_offset, id_len)) return FP_ERR_BAD_BOUNDS;
    if (!fp_string_ok(strings_offset, strings_length, name_offset, name_len)) return FP_ERR_BAD_BOUNDS;
    if (!fp_string_ok(strings_offset, strings_length, version_offset, version_len)) return FP_ERR_BAD_BOUNDS;

    if (info != 0) {
        info->id = pack + id_offset;
        info->id_len = id_len;
        info->name = pack + name_offset;
        info->name_len = name_len;
        info->version = pack + version_offset;
        info->version_len = version_len;
        info->frame_count = (uint16_t)frame_count;
        info->clip_count = (uint16_t)clip_count;
        info->emotion_count = (uint8_t)emotion_count;
        info->gender = gender;
        info->approved = (uint8_t)(features & FP_FLAG_APPROVED);
        info->palette_count = (uint16_t)palette_count;
        info->payload_crc32 = payload_crc;
        info->pack_bytes = file_length; info->format_version = format_version; info->width = (uint16_t)canvas; info->height = (uint16_t)canvas;
        info->codecs = codecs;
        info->move = (uint8_t)move;
    }
    return FP_OK;
}

FP_EXPORT(fp_validate_with_workspace)
fp_error_t fp_validate_with_workspace(const void *pack_bytes_in, uint32_t pack_bytes,
    fp_pack_info_t *info, void *workspace, uint32_t workspace_bytes)
{
    if (workspace && (((uintptr_t)workspace & 7u) || workspace_bytes < sizeof(fp_codec_workspace_t))) return FP_ERR_ARENA;
    return fp_check_pack(pack_bytes_in, pack_bytes, info, (fp_codec_workspace_t *)workspace, 1);
}

FP_EXPORT(fp_inspect_prevalidated)
fp_error_t fp_inspect_prevalidated(const void *pack_bytes_in, uint32_t pack_bytes, fp_pack_info_t *info)
{
    return fp_check_pack(pack_bytes_in, pack_bytes, info, 0, 0);
}

FP_EXPORT(fp_arena_size)
uint32_t fp_arena_size(void)
{
    return ((uint32_t)sizeof(fp_player_t) + 15u) & ~15u;
}

static fp_player_t *fp_bind(void *arena, uint32_t arena_bytes, const void *pack_in,
                            uint32_t pack_bytes, uint32_t seed, fp_error_t *error, int prove)
{
    fp_error_t local = FP_OK;
    if (error == 0) error = &local;
    if (arena == 0 || (((uintptr_t)arena) & 7u) != 0u) {
        *error = FP_ERR_ARGUMENT;
        return 0;
    }
    if (arena_bytes < fp_arena_size()) {
        *error = FP_ERR_ARENA;
        return 0;
    }
    fp_player_t *player = (fp_player_t *)arena;
    fp_zero_bytes(player, (uint32_t)sizeof(fp_player_t));
    *error = fp_check_pack(pack_in, pack_bytes, &player->info, &player->codec_workspace, prove);
    player->decoded_base_frame = -1;
    /* A move pack has no roles to direct; it plays beside its core pack. */
    if (*error == FP_OK && player->info.move) *error = FP_ERR_ARGUMENT;
    if (*error != FP_OK) return 0;

    const uint8_t *pack = (const uint8_t *)pack_in;
    player->pack = pack;
    player->pack_bytes = pack_bytes;

    fp_pack_view_t *view = &player->view;
    uint32_t palette_offset = fp_rd32(pack + FP_HDR_PALETTE_OFFSET);
    view->palette_count = fp_rd16(pack + FP_HDR_PALETTE_COUNT);
    for (uint32_t i = 0; i < view->palette_count; i++) {
        view->palette[i] = fp_rd16(pack + palette_offset + i * 2u);
    }
    view->bg_index = pack[FP_HDR_BG_PALETTE_INDEX];
    view->frame_dir_offset = fp_rd32(pack + FP_HDR_FRAME_DIR_OFFSET);
    view->frame_count = fp_rd16(pack + FP_HDR_FRAME_COUNT);
    view->features = fp_rd16(pack + FP_HDR_FLAGS);
    view->clip_dir_offset = fp_rd32(pack + FP_HDR_CLIP_DIR_OFFSET);
    view->clip_count = fp_rd16(pack + FP_HDR_CLIP_COUNT);
    uint32_t roles_offset = fp_rd32(pack + FP_HDR_ROLES_OFFSET);
    for (uint32_t i = 0; i < FP_ROLE_COUNT; i++) view->roles[i] = fp_rd16(pack + roles_offset + i * 2u);
    view->emotion_count = (uint8_t)fp_rd16(pack + FP_HDR_EMOTION_COUNT);
    uint32_t emotions_offset = fp_rd32(pack + FP_HDR_EMOTIONS_OFFSET);
    for (uint32_t i = 0; i < view->emotion_count; i++) {
        const uint8_t *entry = pack + emotions_offset + i * FP_EMOTION_ENTRY_BYTES;
        view->emotions[i].id = entry[0];
        view->emotions[i].enter_clip = fp_rd16(entry + 8);
        view->emotions[i].hold_clip = fp_rd16(entry + 10);
        view->emotions[i].exit_clip = fp_rd16(entry + 12);
        view->emotions[i].min_hold_ms = fp_rd16(entry + 14);
        view->emotions[i].max_hold_ms = fp_rd16(entry + 16);
    }
    uint32_t talk_offset = fp_rd32(pack + FP_HDR_TALK_OFFSET);
    view->talk_stage_count = pack[talk_offset];
    for (uint32_t i = 0; i < view->talk_stage_count; i++) {
        view->talk_stage_frames[i] = fp_rd16(pack + talk_offset + 4u + i * 2u);
    }
    view->talk_lut_offset = talk_offset + ((4u + (uint32_t)view->talk_stage_count * 2u + 3u) & ~3u);
    uint32_t idle_offset = fp_rd32(pack + FP_HDR_IDLE_OFFSET);
    view->bob_amplitude_px = fp_rd16(pack + idle_offset);
    view->bob_period_ticks = fp_rd16(pack + idle_offset + 2u);
    view->micro_gap_min_ticks = fp_rd16(pack + idle_offset + 4u);
    view->micro_gap_max_ticks = fp_rd16(pack + idle_offset + 6u);
    uint32_t micro_list_offset = fp_rd32(pack + idle_offset + 8u);
    view->micro_count = (uint8_t)fp_rd16(pack + idle_offset + 12u);
    view->fallbacks_enabled = (uint8_t)(pack[idle_offset + 14u] & 1u);
    view->parallax_safe = (uint8_t)((pack[idle_offset + 14u] & 2u) != 0u);
    view->max_parallax_px = pack[idle_offset + 15u];
    for (uint32_t i = 0; i < view->micro_count; i++) {
        const uint8_t *entry = pack + micro_list_offset + i * FP_MICRO_ENTRY_BYTES;
        view->micro[i].clip = fp_rd16(entry);
        view->micro[i].weight = fp_rd16(entry + 2);
    }
    view->action_count = (uint8_t)fp_rd16(pack + FP_HDR_OVERLAY_COUNT);
    uint32_t action_dir = fp_rd32(pack + FP_HDR_OVERLAY_DIR_OFFSET);
    for (uint32_t i = 0; i < view->action_count; i++) {
        const uint8_t *entry = pack + action_dir + i * FP_ACTION_ENTRY_BYTES;
        fp_action_entry_t *action = &view->actions[i];
        action->semantic = entry[0]; action->plane = entry[1]; action->activation = entry[2]; action->priority = entry[3];
        action->clip = fp_rd16(entry + 4u); action->exit_clip = fp_rd16(entry + 6u); action->flags = fp_rd16(entry + 8u);
        action->state_mask = fp_rd16(entry + 10u); action->expression_mask = fp_rd16(entry + 12u);
        action->interval_min_ticks = fp_rd16(entry + 14u); action->interval_max_ticks = fp_rd16(entry + 16u);
        action->cooldown_ticks = fp_rd16(entry + 18u);
        action->enter_clip = (view->features & FP_FEATURE_ACTION_LIFECYCLE) != 0u ? fp_rd16(entry + 20u) : FP_ACTION_NO_CLIP;
        action->performance_mask = (view->features & FP_FEATURE_PERFORMANCE_ACTIONS) != 0u ? fp_rd16(entry + 22u) : 0u;
        player->action_event_countdown[i] = -1;
    }
    for (uint32_t plane = 0; plane < FP_PLANE_COUNT; plane++) player->action_cursors[plane].action = -1;
    player->requested_action=-1;
    if((view->features&FP_FEATURE_FACIAL_REGIONS)!=0u){uint32_t f=fp_rd32(pack+FP_HDR_FACIAL_OFFSET),dir=fp_rd32(pack+f+52u);view->facial_independent_blink=fp_rd16(pack+f)==2u;view->facial_group_count=pack[f+2u];view->facial_center=pack[f+3u];view->facial_blink_state_mask=fp_rd16(pack+f+4u);for(uint32_t i=0;i<6;i++)view->facial_blink_gaps[i]=fp_rd16(pack+f+8u+i*2u);for(uint32_t i=0;i<5;i++)view->facial_blink_ticks[i]=pack[f+20u+i];view->facial_pose_state_mask=fp_rd16(pack+f+26u);view->facial_bob_state_mask=fp_rd16(pack+f+30u);view->facial_bob_full=pack[f+32u];view->facial_bob_balanced=pack[f+33u];view->facial_bob_period=fp_rd16(pack+f+34u);for(uint32_t i=0;i<8;i++)view->facial_pose_timings[i]=fp_rd16(pack+f+36u+i*2u);for(uint32_t i=0;i<view->facial_group_count;i++){const uint8_t*s=pack+dir+i*FP_FACIAL_GROUP_BYTES;fp_facial_group_t*g=&view->facial_groups[i];g->role=s[0];g->base_frame=fp_rd16(s+2u);for(uint32_t j=0;j<4;j++){g->mouth[j]=s[4u+j];g->eyes[j]=s[8u+j];}for(uint32_t j=0;j<7;j++)g->mouth_frames[j]=fp_rd16(s+12u+j*2u);for(uint32_t j=0;j<5;j++)g->eye_frames[j]=fp_rd16(s+26u+j*2u);}player->facial_group=view->facial_center;player->facial_pose_side=1;player->facial_pose_ticks=view->facial_independent_blink?view->facial_pose_timings[0]:1;player->facial_blink_countdown=1;}

    if ((view->features & FP_FEATURE_SPEAKING_POSES) != 0u) {
        const uint8_t *section = pack + fp_rd32(pack + FP_HDR_SPEAKING_OFFSET);
        uint32_t cells = (uint32_t)section[1] * section[2];
        uint32_t frame_bytes = (cells * 2u + 3u) & ~3u;
        view->speaking_pose_count = section[1];
        view->speaking_gesture_count = section[3];
        view->speaking_auto_mask = section[4];
        view->speaking_break_min = fp_rd16(section + 6u);
        view->speaking_break_max = fp_rd16(section + 8u);
        for (uint32_t i = 0; i < cells; i++) view->speaking_frames[i] = fp_rd16(section + FP_SPEAKING_HEADER_BYTES + i * 2u);
        for (uint32_t g = 0; g < view->speaking_gesture_count; g++) {
            const uint8_t *entry = section + FP_SPEAKING_HEADER_BYTES + frame_bytes + g * FP_SPEAKING_GESTURE_BYTES;
            fp_speaking_gesture_t *gesture = &view->speaking_gestures[g];
            gesture->kind = entry[0];
            gesture->step_count = entry[1];
            for (uint32_t j = 0; j < gesture->step_count; j++) {
                gesture->pose[j] = entry[2u + j * 2u];
                gesture->ticks[j] = entry[3u + j * 2u];
            }
        }
    }
    player->speaking_gesture = -1;
    player->speaking_last_gesture = -1;
    player->drift_duration = 1;

    player->prng_state = seed != 0u ? seed : 0x9e3779b9u;
    player->sys_state = FP_SYS_BOOTING;
    player->requested_emotion = FP_EMOTION_REQUEST_NONE;
    player->audio_level = 0;
    player->tap_pending = 0;
    player->shake_pending = 0;
    player->animation_profile = FP_PROFILE_BALANCED;
    player->idle_showcase_enabled = FP_IDLE_SHOWCASE;
    player->calm_countdown = -1;
    player->facial_forced_bob = -1;
    player->magic = FP_PLAYER_MAGIC;
    fp_director_reset(player);
    return player;
}

FP_EXPORT(fp_player_init)
fp_player_t *fp_player_init(void *arena, uint32_t arena_bytes, const void *pack_in,
                            uint32_t pack_bytes, uint32_t seed, fp_error_t *error)
{
    return fp_bind(arena, arena_bytes, pack_in, pack_bytes, seed, error, 1);
}

FP_EXPORT(fp_player_init_prevalidated)
fp_player_t *fp_player_init_prevalidated(void *arena, uint32_t arena_bytes, const void *pack_in,
                                         uint32_t pack_bytes, uint32_t seed, fp_error_t *error)
{
    return fp_bind(arena, arena_bytes, pack_in, pack_bytes, seed, error, 0);
}

/* ---- Clip accessors (read from the mmap'd pack; validation proved bounds) */

uint8_t fp_clip_loop_mode(const fp_player_t *player, uint32_t clip)
{
    return player->pack[player->view.clip_dir_offset + clip * FP_CLIP_DIR_ENTRY_BYTES + 6u];
}

uint16_t fp_clip_step_count(const fp_player_t *player, uint32_t clip)
{
    return fp_rd16(player->pack + player->view.clip_dir_offset + clip * FP_CLIP_DIR_ENTRY_BYTES + 12u);
}

fp_clip_step_t fp_clip_step_at(const fp_player_t *player, uint32_t clip, uint32_t step)
{
    uint32_t steps_offset = fp_rd32(player->pack + player->view.clip_dir_offset + clip * FP_CLIP_DIR_ENTRY_BYTES + 8u);
    const uint8_t *entry = player->pack + steps_offset + step * FP_CLIP_STEP_BYTES;
    fp_clip_step_t out;
    out.frame = fp_rd16(entry);
    out.duration_ticks = fp_rd16(entry + 2);
    return out;
}

/* ---- Rendering — mirrors player.ts render() -------------------------- */
static int fp_facial_compatible(const fp_player_t*p){return p->view.facial_group_count&&p->action_cursors[0].action<0&&(p->phase==FP_PHASE_IDLE||p->phase==FP_PHASE_TALK||p->phase==FP_PHASE_ATTEND);}
static int32_t fp_range(fp_player_t*p,int32_t a,int32_t b){return a+(int32_t)(fp_next_random(p)%(uint32_t)(b-a+1));}
static int32_t fp_group_role(const fp_player_t*p,uint8_t role){for(uint32_t i=0;i<p->view.facial_group_count;i++)if(p->view.facial_groups[i].role==role)return(int32_t)i;return p->view.facial_center;}
static void fp_facial_tick(fp_player_t*p){fp_pack_view_t*v=&p->view;if(!v->facial_group_count)return;uint16_t bit=(uint16_t)(1u<<p->sys_state);if(!fp_facial_compatible(p)){p->facial_group=v->facial_center;p->facial_blink_stage=0;if(v->facial_independent_blink){p->facial_blink_ticks_left=0;p->facial_pose_ticks=v->facial_pose_timings[0];}p->facial_pose_phase=0;return;}if(v->facial_blink_state_mask&bit){if(p->facial_blink_ticks_left>0){if(--p->facial_blink_ticks_left==0){p->facial_blink_stage++;if(p->facial_blink_stage>=5){uint32_t o=(uint32_t)p->animation_profile*2u;p->facial_blink_stage=0;p->facial_blink_countdown=fp_range(p,v->facial_blink_gaps[o],v->facial_blink_gaps[o+1]);}else p->facial_blink_ticks_left=v->facial_blink_ticks[p->facial_blink_stage];}}else if(--p->facial_blink_countdown<=0){p->facial_blink_stage=v->facial_independent_blink?0:1;p->facial_blink_ticks_left=v->facial_blink_ticks[p->facial_blink_stage];}}else{p->facial_blink_stage=0;if(v->facial_independent_blink)p->facial_blink_ticks_left=0;}int center_only=p->action_cursors[1].action>=0&&(v->actions[p->action_cursors[1].action].flags&FP_ACTION_FLAG_CENTER_ONLY);if(center_only||p->animation_profile==FP_PROFILE_REDUCED||!(v->facial_pose_state_mask&bit)){p->facial_group=v->facial_center;p->facial_pose_phase=0;if(v->facial_independent_blink)p->facial_pose_ticks=v->facial_pose_timings[0];return;}if(v->facial_independent_blink){int ax=p->tilt_x<0?-p->tilt_x:p->tilt_x,ay=p->tilt_y<0?-p->tilt_y:p->tilt_y;if(ax>=50||ay>=50){uint8_t role=ax>=ay?(p->tilt_x<0?1u:3u):(p->tilt_y<0?2u:4u);p->facial_group=fp_group_role(p,role);p->facial_pose_phase=role;p->facial_pose_ticks=v->facial_pose_timings[role];return;}if(--p->facial_pose_ticks>0)return;p->facial_pose_phase=(p->facial_pose_phase+1u)%5u;p->facial_group=fp_group_role(p,p->facial_pose_phase);p->facial_pose_ticks=v->facial_pose_timings[p->facial_pose_phase];return;}if(p->tilt_x<=-50||p->tilt_x>=50){p->facial_group=fp_group_role(p,p->tilt_x<0?2u:4u);p->facial_pose_phase=0;return;}if(--p->facial_pose_ticks>0)return;uint32_t b=p->animation_profile==FP_PROFILE_FULL?0u:4u;if(p->facial_pose_phase==0){p->facial_pose_side=-p->facial_pose_side;p->facial_pose_phase=1;p->facial_group=fp_group_role(p,p->facial_pose_side<0?1u:3u);p->facial_pose_ticks=5;}else if(p->facial_pose_phase==1){p->facial_pose_phase=2;p->facial_group=fp_group_role(p,p->facial_pose_side<0?2u:4u);p->facial_pose_ticks=fp_range(p,v->facial_pose_timings[b+2],v->facial_pose_timings[b+3]);}else if(p->facial_pose_phase==2){p->facial_pose_phase=3;p->facial_group=fp_group_role(p,p->facial_pose_side<0?1u:3u);p->facial_pose_ticks=5;}else{p->facial_pose_phase=0;p->facial_group=v->facial_center;p->facial_pose_ticks=fp_range(p,v->facial_pose_timings[b],v->facial_pose_timings[b+1]);}}
static int32_t fp_facial_offset_y(const fp_player_t*p){const fp_pack_view_t*v=&p->view;if(p->facial_forced_bob>=0)return p->facial_forced_bob;if(!fp_facial_compatible(p)||p->animation_profile==FP_PROFILE_REDUCED||!(v->facial_bob_state_mask&(1u<<p->sys_state)))return 0;int32_t a=p->animation_profile==FP_PROFILE_FULL?v->facial_bob_full:v->facial_bob_balanced;if(!a||!v->facial_bob_period)return 0;int32_t q=p->bob_tick%v->facial_bob_period;return(q*4>=v->facial_bob_period&&q*4<v->facial_bob_period*3)?a:0;}
static void fp_dense(fp_player_t*p,uint16_t*target,uint16_t frame,int32_t ox,int32_t oy){uint32_t o=fp_rd32(p->pack+p->view.frame_dir_offset+(uint32_t)frame*FP_FRAME_DIR_ENTRY_BYTES);uint32_t n=fp_rd32(p->pack+p->view.frame_dir_offset+(uint32_t)frame*FP_FRAME_DIR_ENTRY_BYTES+4u);uint8_t c=p->pack[p->view.frame_dir_offset+(uint32_t)frame*FP_FRAME_DIR_ENTRY_BYTES+8u];const uint8_t*d=fp_decode_resource(p->pack+o,n,c,p->info.width,&p->codec_workspace,&n);if(!d)return;uint32_t w=d[2],h=d[3];for(uint32_t q=0;q<w*h;q++){int32_t x=d[0]+(int32_t)(q%w)+ox,y=d[1]+(int32_t)(q/w)+oy;if(x>=0&&x<p->info.width&&y>=0&&y<p->info.height)target[y*p->info.width+x]=p->view.palette[d[4+q]];}}

/* Draw a complete base frame at a whole-pixel offset; off-canvas pixels are dropped. */
static void fp_draw_base(fp_player_t *player, uint16_t *target, const uint8_t *frame, uint8_t frame_codec,
                         uint32_t frame_index, int32_t offset_x, int32_t offset_y)
{
    const uint16_t *palette = player->view.palette;
    const int32_t width = player->info.width, height = player->info.height;
    if (frame_codec == FP_CODEC_INDEX8_FULL_RLE) {
        uint32_t frame_length = fp_rd32(player->pack + player->view.frame_dir_offset +
                                        frame_index * FP_FRAME_DIR_ENTRY_BYTES + 4u);
        int32_t source_x = 0, source_y = 0;
        for (uint32_t at = 0; at < frame_length; at += 2u) {
            uint32_t run = frame[at];
            uint16_t color = palette[frame[at + 1u]];
            while (run-- > 0u) {
                int32_t x = source_x + offset_x, y = source_y + offset_y;
                if (x >= 0 && x < width && y >= 0 && y < height) target[y * width + x] = color;
                if (++source_x == width) { source_x = 0; source_y++; }
            }
        }
        return;
    }
    /* Palette indices: each covered canvas row is one clipped source row. */
    int32_t x0 = offset_x > 0 ? offset_x : 0, x1 = offset_x < 0 ? width + offset_x : width;
    int32_t y0 = offset_y > 0 ? offset_y : 0, y1 = offset_y < 0 ? height + offset_y : height;
    for (int32_t y = y0; y < y1 && x0 < x1; y++) {
        const uint8_t *source = frame + (y - offset_y) * width + (x0 - offset_x);
        uint16_t *out = target + y * width + x0;
        for (int32_t i = 0; i < x1 - x0; i++) out[i] = palette[source[i]];
    }
}

/* One out-of-line copy of the hot loop serves every call site. */
#if defined(__GNUC__)
#define FP_NOINLINE __attribute__((noinline))
#else
#define FP_NOINLINE
#endif

typedef struct { int32_t lo, hi; } fp_span_t;

/* Write the pixels of row[x, end) whose new color differs and widen `span`
 * around them; the runs of a row are visited left to right. Pixel x's new
 * color is palette[*indices], and `indices` advances by `step` per pixel: 1
 * walks a row of palette indices, 0 repeats one index (the background). */
static FP_NOINLINE void fp_sync_run(uint16_t *row, int32_t x, int32_t end, const uint8_t *indices, int32_t step,
                                    const uint16_t *palette, fp_span_t *span)
{
    int32_t lo = span->lo, hi = span->hi;
    for (; x < end; x++, indices += step) {
        uint16_t next = palette[*indices];
        if (next != row[x]) {
            row[x] = next;
            lo = hi < 0 ? x : lo;
            hi = x;
        }
    }
    span->lo = lo;
    span->hi = hi;
}

static int fp_dirty_from_rows(fp_dirty_rect_t *dirty, int32_t min_x, int32_t max_x, int32_t min_y, int32_t max_y)
{
    if (max_x < min_x) return 0;
    dirty->x = (int16_t)min_x; dirty->y = (int16_t)min_y;
    dirty->width = (uint8_t)(max_x - min_x + 1); dirty->height = (uint8_t)(max_y - min_y + 1);
    return 1;
}

/* The framebuffer becomes `frame` (palette indices) at an offset with nothing
 * drawn over it. It is compared in place, without composing a scratch canvas,
 * and only differing pixels are written. The edges an offset exposes show
 * `under` (the unshifted frame) or else the background, as fp_render draws
 * them. Returns 0 when no pixel changed, else the changed bounding box. */
static int fp_sync_plain(fp_player_t *player, const uint8_t *frame, int32_t offset_x, int32_t offset_y,
                         const uint8_t *under, fp_dirty_rect_t *dirty)
{
    const uint16_t *palette = player->view.palette;
    const int32_t width = player->info.width, height = player->info.height;
    const int32_t cx0 = offset_x < 0 ? 0 : offset_x > width ? width : offset_x;
    const int32_t cx1 = offset_x > 0 ? width : offset_x < -width ? 0 : width + offset_x;
    const int32_t cy0 = offset_y > 0 ? offset_y : 0, cy1 = offset_y < 0 ? height + offset_y : height;
    int32_t min_x = width, max_x = -1, min_y = height, max_y = -1;
    for (int32_t y = 0; y < height; y++) {
        uint16_t *row = player->framebuffer + y * width;
        /* Exposed pixels: the unshifted frame's own row, or the background. */
        const uint8_t *exposed = under ? under + y * width : &player->view.bg_index;
        int32_t step = under ? 1 : 0;
        int covered = y >= cy0 && y < cy1 && cx0 < cx1;
        int32_t left = covered ? cx0 : width, right = covered ? cx1 : width;
        fp_span_t span = { width, -1 };
        fp_sync_run(row, 0, left, exposed, step, palette, &span);
        if (covered) fp_sync_run(row, cx0, cx1, frame + (y - offset_y) * width + (cx0 - offset_x), 1, palette, &span);
        fp_sync_run(row, right, width, exposed + step * right, step, palette, &span);
        if (span.hi < 0) continue;
        if (span.lo < min_x) min_x = span.lo;
        if (span.hi > max_x) max_x = span.hi;
        if (y < min_y) min_y = y;
        max_y = y;
    }
    return fp_dirty_from_rows(dirty, min_x, max_x, min_y, max_y);
}

/* Copy a composed canvas into the framebuffer where it differs. */
static int fp_sync_canvas(fp_player_t *player, const uint16_t *target, fp_dirty_rect_t *dirty)
{
    const int32_t width = player->info.width, height = player->info.height;
    int32_t min_x = width, max_x = -1, min_y = height, max_y = -1;
    for (int32_t y = 0; y < height; y++) {
        uint16_t *row = player->framebuffer + y * width;
        const uint16_t *next = target + y * width;
        int32_t lo = width, hi = -1;
        for (int32_t x = 0; x < width; x++) {
            if (next[x] != row[x]) {
                row[x] = next[x];
                lo = hi < 0 ? x : lo;
                hi = x;
            }
        }
        if (hi < 0) continue;
        if (lo < min_x) min_x = lo;
        if (hi > max_x) max_x = hi;
        if (y < min_y) min_y = y;
        max_y = y;
    }
    return fp_dirty_from_rows(dirty, min_x, max_x, min_y, max_y);
}

static int fp_effects_idle(const fp_player_t *player)
{
    for (uint32_t plane = FP_PLANE_CHARACTER_FX; plane < FP_PLANE_COUNT; plane++)
        if (player->action_cursors[plane].action >= 0) return 0;
    return 1;
}

void fp_render(fp_player_t *player)
{
    uint32_t frame_index = (uint32_t)fp_actions_frame(player);
    int32_t offset_x = fp_actions_offset_x(player);
    int32_t offset_y = fp_actions_offset_y(player);
    int facial=fp_facial_compatible(player);fp_facial_group_t*group=facial?&player->view.facial_groups[player->facial_group]:0;if(group){frame_index=group->base_frame;offset_y=fp_facial_offset_y(player);}
    /* Leaving a facial pose may reuse the same base frame number. Its prior
     * mouth/eye patches still have to be erased; frame identity is insufficient. */
    int feature_pack = player->view.action_count > 0u || player->view.fallbacks_enabled || player->view.parallax_safe ||
                       player->view.facial_group_count > 0u || player->view.speaking_pose_count > 0u;
    if (!feature_pack && player->render_initialized &&
        player->rendered_frame == (int32_t)frame_index &&
        player->rendered_offset_y == offset_y) {
        player->dirty_rect.width = 0u;
        player->dirty_rect.height = 0u;
        return;
    }
    /* Drifting speech frames sit on the unshifted frame, so the exposed edge
     * shows the character's own background instead of an empty strip. */
    int under = fp_speaking_motion(player) && (offset_x != 0 || offset_y != 0);
    /* A base frame with nothing over it is the common tick. When the
     * framebuffer already holds exactly that, no pixel can change. */
    int plain = feature_pack && !group && !player->view.fallbacks_enabled && fp_effects_idle(player);
    if (plain && player->plain_valid && player->plain_frame == (int32_t)frame_index &&
        player->plain_offset_x == offset_x && player->plain_offset_y == offset_y && player->plain_under == under) {
        player->dirty_rect.width = 0u;
        player->dirty_rect.height = 0u;
        return;
    }
    const uint8_t *entry = player->pack + player->view.frame_dir_offset + frame_index * FP_FRAME_DIR_ENTRY_BYTES;
    uint8_t frame_codec = entry[8];
    const uint8_t *frame = player->pack + fp_rd32(entry);
    if (frame_codec == FP_CODEC_INDEX8_FULL_ZLIB) {
        if (player->decoded_base_frame != (int32_t)frame_index) {
            /* Validated immutable pack; corruption must never overrun. */
            player->decoded_base_frame = -1;
            if (!fp_inflate_full_frame(frame, fp_rd32(entry + 4u), player->info.width,
                                       &player->codec_workspace, player->decoded_base)) return;
            player->decoded_base_frame = (int32_t)frame_index;
            player->base_decode_count++;
        }
        frame = player->decoded_base;
    }
    fp_dirty_rect_t dirty = player->dirty_rect;
    int changed = 1;
    if (plain && frame_codec != FP_CODEC_INDEX8_FULL_RLE) {
        changed = fp_sync_plain(player, frame, offset_x, offset_y, under ? frame : 0, &dirty);
    } else {
        uint16_t *target = feature_pack ? player->scratch : player->framebuffer;
        /* A full frame covers the canvas; only an offset exposes an edge. */
        if (under) {
            fp_draw_base(player, target, frame, frame_codec, frame_index, 0, 0);
        } else if (offset_x != 0 || offset_y != 0) {
            uint16_t bg = player->view.palette[player->view.bg_index];
            uint32_t pixels = (uint32_t)player->info.width * player->info.height;
            for (uint32_t pixel = 0; pixel < pixels; pixel++) target[pixel] = bg;
        }
        fp_draw_base(player, target, frame, frame_codec, frame_index, offset_x, offset_y);
        /* The validator bounds facial talk stages; the clamp is a memory-safety backstop. */
        if(group){uint32_t mouth=player->phase==FP_PHASE_TALK?(uint32_t)player->talk_stage:0u;if(mouth>=FP_FACIAL_MOUTH_STAGES)mouth=FP_FACIAL_MOUTH_STAGES-1u;fp_dense(player,target,group->mouth_frames[mouth],offset_x,offset_y);if(!player->view.facial_independent_blink||player->facial_blink_ticks_left>0||player->facial_forced_bob>=0)fp_dense(player,target,group->eye_frames[player->facial_blink_stage],offset_x,offset_y);}
        if (feature_pack) {
            fp_actions_composite(player, target);
            changed = fp_sync_canvas(player, target, &dirty);
        } else {
            dirty.x = 0; dirty.y = 0;
            dirty.width = player->info.width; dirty.height = player->info.height;
        }
    }
    player->plain_valid = plain;
    player->plain_frame = (int32_t)frame_index;
    player->plain_offset_x = offset_x;
    player->plain_offset_y = offset_y;
    player->plain_under = under;
    if (!changed) {
        player->dirty_rect.width = 0u; player->dirty_rect.height = 0u;
        return;
    }
    player->dirty_rect = dirty;
    player->rendered_frame = (int32_t)frame_index;
    player->rendered_offset_x = offset_x;
    player->rendered_offset_y = offset_y;
    player->render_initialized = 1;
    player->visual_revision++;
}

/* ---- Public API ------------------------------------------------------- */

FP_EXPORT(fp_request_action)
uint8_t fp_request_action(fp_player_t *player,int16_t action)
{
    if(!player||player->magic!=FP_PLAYER_MAGIC)return 0u;
    return fp_actions_request(player,action);
}

FP_EXPORT(fp_debug_base_decode_count)
uint32_t fp_debug_base_decode_count(const fp_player_t *player) { return player ? player->base_decode_count : 0u; }

FP_EXPORT(fp_set_sys_state)
void fp_set_sys_state(fp_player_t *player, fp_sys_state_t state)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    /* State masks are 16-bit; an unknown state would shift out of range. */
    if ((int32_t)state < FP_SYS_BOOTING || (int32_t)state > FP_SYS_ERROR) return;
    if(player->sys_state!=(int32_t)state)player->requested_action=-1;
    player->sys_state = (int32_t)state;
}

FP_EXPORT(fp_set_emotion)
void fp_set_emotion(fp_player_t *player, fp_emotion_t emotion)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->requested_emotion = (int32_t)emotion;
}

FP_EXPORT(fp_set_audio_level)
void fp_set_audio_level(fp_player_t *player, uint8_t level_0_100)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->audio_level = level_0_100 > 100u ? 100 : (int32_t)level_0_100;
}

FP_EXPORT(fp_notify_tap)
void fp_notify_tap(fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->tap_pending = 1;
}

FP_EXPORT(fp_notify_touch)
void fp_notify_touch(fp_player_t *player)
{
    fp_notify_tap(player);
}

FP_EXPORT(fp_notify_shake)
void fp_notify_shake(fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->shake_pending = 1;
}

FP_EXPORT(fp_notify_gesture)
void fp_notify_gesture(fp_player_t *player, fp_gesture_t gesture)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->gesture_pending = gesture >= FP_GESTURE_NOD && gesture <= FP_GESTURE_POP ? (int32_t)gesture : FP_GESTURE_NONE;
}

FP_EXPORT(fp_set_tilt)
void fp_set_tilt(fp_player_t *player, int8_t screen_x, int8_t screen_y)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->tilt_x = screen_x < -100 ? -100 : screen_x > 100 ? 100 : screen_x;
    player->tilt_y = screen_y < -100 ? -100 : screen_y > 100 ? 100 : screen_y;
}

FP_EXPORT(fp_set_speaking_drift)
void fp_set_speaking_drift(fp_player_t *player, uint8_t enabled)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->speaking_drift_enabled = enabled ? 1 : 0;
}

FP_EXPORT(fp_set_animation_profile)
void fp_set_animation_profile(fp_player_t *player, fp_animation_profile_t profile)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    player->animation_profile = profile < FP_PROFILE_FULL || profile > FP_PROFILE_REDUCED ? FP_PROFILE_BALANCED : profile;
}

FP_EXPORT(fp_set_idle_showcase_enabled)
void fp_set_idle_showcase_enabled(fp_player_t *player, uint8_t enabled)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    fp_set_idle_mode(player, enabled ? FP_IDLE_SHOWCASE : FP_IDLE_HOLD);
    if (!enabled) {
        player->requested_emotion = FP_EMO_NEUTRAL;
        player->pending_emotion = FP_EMO_NEUTRAL;
    }
}

FP_EXPORT(fp_set_idle_mode)
void fp_set_idle_mode(fp_player_t *player, fp_idle_mode_t mode)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    if (mode != FP_IDLE_HOLD && mode != FP_IDLE_NATURAL) mode = FP_IDLE_SHOWCASE;
    if (player->idle_showcase_enabled == (int32_t)mode) return;
    player->idle_showcase_enabled = (int32_t)mode;
    player->calm_countdown = -1;
    /* Interrupted loops resume only within one stretch of natural idle. */
    for (uint32_t i = 0; i < FP_MAX_ACTIONS; i++) player->loop_resume_ticks[i] = 0;
    if (mode == FP_IDLE_HOLD) {
        player->requested_emotion = FP_EMO_NEUTRAL;
        player->pending_emotion = FP_EMO_NEUTRAL;
    }
}

FP_EXPORT(fp_report_health)
void fp_report_health(fp_player_t *player, uint8_t audio_healthy, uint16_t presentation_ms)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    if (!audio_healthy || presentation_ms > 20u) player->optional_suppression_ticks = (2000 + FP_TICK_MS - 1) / FP_TICK_MS;
}

FP_EXPORT(fp_tick)
void fp_tick(fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return;
    fp_facial_tick(player);
    fp_director_tick(player);
}

FP_EXPORT(fp_framebuffer)
const uint16_t *fp_framebuffer(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0;
    return player->framebuffer;
}

FP_EXPORT(fp_frame_crc32)
uint32_t fp_frame_crc32(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0;
    return fp_crc32_bytes((const uint8_t *)player->framebuffer, ((uint32_t)player->info.width * player->info.height) * 2u);
}

FP_EXPORT(fp_canvas_width)
uint16_t fp_canvas_width(const fp_player_t *player)
{
    return player && player->magic == FP_PLAYER_MAGIC ? player->info.width : 0;
}

FP_EXPORT(fp_canvas_height)
uint16_t fp_canvas_height(const fp_player_t *player)
{
    return player && player->magic == FP_PLAYER_MAGIC ? player->info.height : 0;
}

FP_EXPORT(fp_visual_revision)
uint32_t fp_visual_revision(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0;
    return player->visual_revision;
}

FP_EXPORT(fp_dirty_rect)
fp_dirty_rect_t fp_dirty_rect(const fp_player_t *player)
{
    fp_dirty_rect_t empty = { 0, 0, 0, 0 };
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return empty;
    return player->dirty_rect;
}

#define FP_DIRTY_ACCESSOR(name, field, type) \
    FP_EXPORT(name) type name(const fp_player_t *player) { \
        return player == 0 || player->magic != FP_PLAYER_MAGIC ? 0 : (type)player->dirty_rect.field; \
    }
FP_DIRTY_ACCESSOR(fp_debug_dirty_x, x, int16_t)
FP_DIRTY_ACCESSOR(fp_debug_dirty_y, y, int16_t)
FP_DIRTY_ACCESSOR(fp_debug_dirty_width, width, uint16_t)
FP_DIRTY_ACCESSOR(fp_debug_dirty_height, height, uint16_t)

FP_EXPORT(fp_active_action)
int16_t fp_active_action(const fp_player_t *player, fp_plane_t plane)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC || plane < FP_PLANE_BASE || plane >= FP_PLANE_COUNT) return -1;
    return player->action_cursors[plane].action;
}

FP_EXPORT(fp_motion_capabilities)
fp_motion_capabilities_t fp_motion_capabilities(const fp_player_t *player)
{
    fp_motion_capabilities_t out = { 0 };
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return out;
    out.overlay_actions = (uint8_t)((player->view.features & FP_FEATURE_OVERLAY_ACTIONS) != 0u);
    out.parallax_safe = player->view.parallax_safe;
    out.max_parallax_px = player->view.max_parallax_px;
    out.action_count = player->view.action_count;
    out.facial_regions = player->view.facial_group_count > 0u;
    out.facial_group_count = player->view.facial_group_count;
    out.speaking_poses = player->view.speaking_pose_count > 0u;
    out.speaking_gesture_count = player->view.speaking_gesture_count;
    for (uint32_t i = 0; i < player->view.action_count; i++) out.semantic_mask |= 1u << player->view.actions[i].semantic;
    return out;
}

FP_EXPORT(fp_pack_info)
const fp_pack_info_t *fp_pack_info(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0;
    return &player->info;
}

FP_EXPORT(fp_emotion_id_at)
uint8_t fp_emotion_id_at(const fp_player_t *player, uint8_t index)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0;
    if (index >= player->view.emotion_count) return 0;
    return player->view.emotions[index].id;
}

FP_EXPORT(fp_debug_phase)
uint8_t fp_debug_phase(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0xffu;
    return (uint8_t)player->phase;
}

FP_EXPORT(fp_debug_frame)
uint16_t fp_debug_frame(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0xffffu;
    return (uint16_t)fp_actions_frame(player);
}

FP_EXPORT(fp_debug_offset_y)
int8_t fp_debug_offset_y(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0;
    return (int8_t)(fp_facial_compatible(player) ? fp_facial_offset_y(player) : fp_actions_offset_y(player));
}

FP_EXPORT(fp_debug_offset_x)
int8_t fp_debug_offset_x(const fp_player_t *player)
{
    if (player == 0 || player->magic != FP_PLAYER_MAGIC) return 0;
    return (int8_t)fp_actions_offset_x(player);
}
FP_EXPORT(fp_debug_facial_group) int8_t fp_debug_facial_group(const fp_player_t*p){return p&&p->magic==FP_PLAYER_MAGIC?(int8_t)p->facial_group:-1;}
FP_EXPORT(fp_debug_mouth_stage) uint8_t fp_debug_mouth_stage(const fp_player_t*p){return p&&p->magic==FP_PLAYER_MAGIC?(uint8_t)(p->phase==FP_PHASE_TALK?p->talk_stage:0):0;}
FP_EXPORT(fp_debug_blink_stage) uint8_t fp_debug_blink_stage(const fp_player_t*p){return p&&p->magic==FP_PLAYER_MAGIC?(uint8_t)p->facial_blink_stage:0;}
FP_EXPORT(fp_debug_speaking_pose)
uint8_t fp_debug_speaking_pose(const fp_player_t *player)
{
    return player && player->magic == FP_PLAYER_MAGIC && player->phase == FP_PHASE_TALK ? (uint8_t)player->speaking_pose : 0u;
}
FP_EXPORT(fp_debug_speaking_gesture)
int8_t fp_debug_speaking_gesture(const fp_player_t *player)
{
    return player && player->magic == FP_PLAYER_MAGIC && player->phase == FP_PHASE_TALK ? (int8_t)player->speaking_gesture : -1;
}
FP_EXPORT(fp_debug_compose_facial) void fp_debug_compose_facial(fp_player_t*p,uint8_t group,uint8_t mouth,uint8_t blink,uint8_t bob){if(!p||p->magic!=FP_PLAYER_MAGIC||group>=p->view.facial_group_count||mouth>=7u||blink>=5u||bob>2u)return;p->phase=FP_PHASE_TALK;p->sys_state=FP_SYS_SPEAKING;p->facial_group=group;p->talk_stage=mouth;p->facial_blink_stage=blink;p->facial_forced_bob=bob;fp_render(p);}

const char *fp_error_string(fp_error_t error)
{
    switch (error) {
    case FP_OK: return "ok";
    case FP_ERR_ARGUMENT: return "bad argument";
    case FP_ERR_BAD_MAGIC: return "bad magic";
    case FP_ERR_UNSUPPORTED_VERSION: return "unsupported version";
    case FP_ERR_BAD_HEADER: return "bad header";
    case FP_ERR_BAD_CHECKSUM: return "bad checksum";
    case FP_ERR_BAD_BOUNDS: return "section out of bounds";
    case FP_ERR_BAD_FRAME: return "bad frame";
    case FP_ERR_BAD_CLIP: return "bad clip";
    case FP_ERR_BAD_ROLE: return "bad role";
    case FP_ERR_BAD_EMOTION: return "bad emotion";
    case FP_ERR_BAD_TALK: return "bad talk table";
    case FP_ERR_BAD_IDLE: return "bad idle params";
    case FP_ERR_BAD_OVERLAY: return "bad overlay action";
    case FP_ERR_BAD_FACIAL: return "bad facial regions";
    case FP_ERR_TRUNCATED: return "truncated";
    case FP_ERR_ARENA: return "arena too small";
    case FP_ERR_BAD_SPEAKING: return "bad speaking poses";
    case FP_ERR_BAD_MOVE: return "bad move";
    default: return "unknown";
    }
}
