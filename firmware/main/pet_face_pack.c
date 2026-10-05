#include "pet_face_pack.h"
#include "pet_face_id.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "face_core.h"
#include "frame_player.h"

static const char *TAG = "pet_face_frames";

fp_error_t pet_face_pack_validate(const void *bytes, uint32_t length, fp_pack_info_t *info)
{
    uint32_t size = fp_validation_workspace_size();
    void *workspace = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!workspace) return FP_ERR_ARENA;
    fp_error_t result = fp_validate_with_workspace(bytes, length, info, workspace, size);
    heap_caps_free(workspace);
    return result;
}

#include "frame_display.h"

/* PCM energy is refreshed in 20 ms slices.  The display timer runs at 33 ms,
 * so this ceiling presents every available speaking tick without inventing
 * extra animation latency. */
#define SPEAKING_PRESENT_INTERVAL_US 20000LL
static uint8_t audio_level_to_percent(uint8_t level_0_255)
{
    return (uint8_t)(((uint32_t)level_0_255 * 100u + 127u) / 255u);
}

typedef struct {
    const char *id;
    const uint8_t *start;
    const uint8_t *end;
    uint8_t slot;
} embedded_pack_t;

static const embedded_pack_t s_embedded[] = {
    {NULL,NULL,NULL,0}, /* Zero real embedded packs in the generic firmware. */
};
static embedded_pack_t s_external;
static char s_external_id[PET_FACE_ID_MAX];

/* What each pack proved, once per boot. An embedded pack is validated in full
 * on first use; the external pack was validated by its installer and is only
 * inspected when registered. Nothing is validated twice, so binding a pack for
 * display (under the display lock) never inflates its frames again. */
typedef struct {
    bool checked, valid;
    fp_pack_info_t info;
    char sha256[65];
} pack_check_t;
static pack_check_t s_embedded_checks[sizeof(s_embedded) / sizeof(s_embedded[0])];
static pack_check_t s_external_check;

typedef struct {
    fp_player_t *player;
    void *arena;
    uint16_t *physical;
    lv_obj_t *root;
    lv_obj_t *canvas;
    uint16_t display_size;
    char active_id[PET_FACE_ID_MAX];
    pet_expression_t shown_expression;
    pet_face_state_t shown_state;
    uint32_t presented_revision;
    uint32_t last_now_ms;
    uint32_t gesture_until_ms;
    uint32_t ticks;
    uint32_t presentations;
    uint32_t coalesced_presentations;
    int64_t last_stats_us;
    int64_t last_present_us;
    int64_t max_tick_us;
    int64_t max_present_us;
    fp_dirty_rect_t pending_dirty;
    bool pending_dirty_valid;
    bool shown_state_valid;
    bool touch_was_pressed;
    bool bound_external;
    bool holding; /* pet_face_pack_hold_external: last frame stays, no player. */
} frame_scene_t;

static frame_scene_t s_scene;

_Static_assert((int)PET_FACE_BOOTING == (int)FP_SYS_BOOTING &&
               (int)PET_FACE_PROVISIONING == (int)FP_SYS_PROVISIONING &&
               (int)PET_FACE_CONNECTING == (int)FP_SYS_CONNECTING &&
               (int)PET_FACE_IDLE == (int)FP_SYS_IDLE &&
               (int)PET_FACE_LISTENING == (int)FP_SYS_LISTENING &&
               (int)PET_FACE_THINKING == (int)FP_SYS_THINKING &&
               (int)PET_FACE_SPEAKING == (int)FP_SYS_SPEAKING &&
               (int)PET_FACE_OFFLINE == (int)FP_SYS_OFFLINE &&
               (int)PET_FACE_ERROR == (int)FP_SYS_ERROR,
               "pet face and full-frame state ordinals must match");
_Static_assert((int)PET_ANIMATION_FULL == (int)FP_PROFILE_FULL &&
               (int)PET_ANIMATION_BALANCED == (int)FP_PROFILE_BALANCED &&
               (int)PET_ANIMATION_REDUCED == (int)FP_PROFILE_REDUCED,
               "animation profile ordinals must match");

static void *frame_alloc(size_t bytes)
{
    void *memory = heap_caps_aligned_alloc(
        16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        memory = heap_caps_aligned_alloc(
            16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return memory;
}

static const embedded_pack_t *find_pack(const char *id)
{
    if (!id) return NULL;
    if(s_external.id&&!strcmp(id,s_external.id))return &s_external;
    for (size_t index = 0; index < sizeof(s_embedded) / sizeof(s_embedded[0]);
         ++index) {
        if (s_embedded[index].id&&!strcmp(id, s_embedded[index].id)) return &s_embedded[index];
    }
    return NULL;
}

static uint32_t pack_bytes(const embedded_pack_t *pack)
{
    return pack ? (uint32_t)(pack->end - pack->start) : 0;
}

static pack_check_t *pack_check(const embedded_pack_t *pack)
{
    if (!pack || !pack->start) return NULL;
    pack_check_t *check = pack == &s_external ? &s_external_check : &s_embedded_checks[pack - s_embedded];
    if (!check->checked) {
        check->checked = true;
        fp_error_t result = pet_face_pack_validate(pack->start, pack_bytes(pack), &check->info);
        check->valid = result == FP_OK && check->info.approved;
        if (!check->valid) ESP_LOGE(TAG, "pack %s rejected: %s", pack->id, result == FP_OK ? "unapproved" : fp_error_string(result));
    }
    return check->valid ? check : NULL;
}

static fp_emotion_t expression_to_emotion(pet_expression_t expression)
{
    switch (expression) {
        case PET_EXPRESSION_HAPPY: return FP_EMO_JOY;
        case PET_EXPRESSION_CURIOUS: return FP_EMO_CURIOUS;
        case PET_EXPRESSION_SURPRISED: return FP_EMO_SURPRISED;
        case PET_EXPRESSION_SLEEPY: return FP_EMO_SLEEPY;
        case PET_EXPRESSION_CONCERNED: return FP_EMO_CONCERNED;
        case PET_EXPRESSION_EXCITED: return FP_EMO_EXCITED;
        case PET_EXPRESSION_SHY: return FP_EMO_SHY;
        case PET_EXPRESSION_ANGRY: return FP_EMO_ANGRY;
        case PET_EXPRESSION_CONFUSED: return FP_EMO_CONFUSED;
        case PET_EXPRESSION_DETERMINED: return FP_EMO_DETERMINED;
        case PET_EXPRESSION_DISGUST: return FP_EMO_DISGUST;
        case PET_EXPRESSION_EMBARRASSED: return FP_EMO_EMBARRASSED;
        case PET_EXPRESSION_FEAR: return FP_EMO_FEAR;
        case PET_EXPRESSION_SAD: return FP_EMO_SAD;
        case PET_EXPRESSION_IDLE:
        default: return FP_EMO_NEUTRAL;
    }
}

static fp_dirty_rect_t full_dirty(void)
{
    return (fp_dirty_rect_t){
        .x = 0, .y = 0,
        .width = fp_canvas_width(s_scene.player), .height = fp_canvas_height(s_scene.player),
    };
}

static void union_pending_dirty(fp_dirty_rect_t next)
{
    if (!next.width || !next.height) return;
    if (!s_scene.pending_dirty_valid) {
        s_scene.pending_dirty = next;
        s_scene.pending_dirty_valid = true;
        return;
    }
    int x0 = s_scene.pending_dirty.x < next.x ? s_scene.pending_dirty.x : next.x;
    int y0 = s_scene.pending_dirty.y < next.y ? s_scene.pending_dirty.y : next.y;
    int old_x1 = s_scene.pending_dirty.x + s_scene.pending_dirty.width;
    int old_y1 = s_scene.pending_dirty.y + s_scene.pending_dirty.height;
    int next_x1 = next.x + next.width;
    int next_y1 = next.y + next.height;
    int x1 = old_x1 > next_x1 ? old_x1 : next_x1;
    int y1 = old_y1 > next_y1 ? old_y1 : next_y1;
    s_scene.pending_dirty = (fp_dirty_rect_t){
        .x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0,
    };
}

static void expand_dirty(const uint16_t *logical, fp_dirty_rect_t dirty)
{
    fp_display_expand(logical, fp_canvas_width(s_scene.player), fp_canvas_height(s_scene.player),
                      s_scene.physical, s_scene.display_size, dirty);
}

static void sync_canvas_buffer(fp_dirty_rect_t dirty)
{
    if (!s_scene.canvas || !dirty.width || !dirty.height) return;
    lv_draw_buf_t *draw_buffer = lv_canvas_get_draw_buf(s_scene.canvas);
    if (!draw_buffer) return;
    fp_dirty_rect_t physical = fp_display_dirty(fp_canvas_width(s_scene.player), fp_canvas_height(s_scene.player),
                                              s_scene.display_size, dirty);
    if (!physical.width || !physical.height) return;
    lv_area_t area = {
        .x1 = physical.x,
        .y1 = physical.y,
        .x2 = physical.x + physical.width - 1,
        .y2 = physical.y + physical.height - 1,
    };
    /* The ESP32-S3 PPA handler maps this LVGL cache operation to a CPU-to-
     * memory esp_cache_msync.  The canvas lives in PSRAM, so publish CPU
     * writes before LVGL/PPA reads the image source or stale poses can remain
     * visible underneath the next frame. */
    lv_draw_buf_invalidate_cache(draw_buffer, &area);
}

static void invalidate_dirty(fp_dirty_rect_t dirty)
{
    if (!s_scene.canvas || !dirty.width || !dirty.height) return;
    lv_area_t canvas;
    lv_obj_get_coords(s_scene.canvas, &canvas);
    fp_dirty_rect_t physical = fp_display_dirty(fp_canvas_width(s_scene.player), fp_canvas_height(s_scene.player),
                                              s_scene.display_size, dirty);
    if (!physical.width || !physical.height) return;
    lv_area_t area = {
        .x1 = canvas.x1 + physical.x,
        .y1 = canvas.y1 + physical.y,
        .x2 = canvas.x1 + physical.x + physical.width - 1,
        .y2 = canvas.y1 + physical.y + physical.height - 1,
    };
    lv_obj_invalidate_area(s_scene.canvas, &area);
}

static void forget_player(void)
{
    /* The arena contains the player's borrowed flash pointers. No callback may
     * access it until a complete new binding succeeds. The canvas is a separate
     * RGB565 copy and can safely remain allocated while recovery UI is shown. */
    s_scene.player = NULL;
    s_scene.bound_external = false;
    s_scene.holding = false;
    if (s_scene.arena) memset(s_scene.arena, 0, fp_arena_size());
    s_scene.active_id[0] = 0;
    s_scene.gesture_until_ms = 0;
    s_scene.pending_dirty_valid = false;
    s_scene.shown_state_valid = false;
    s_scene.touch_was_pressed = false;
}

static esp_err_t bind_pack(const embedded_pack_t *pack)
{
    if (!pack || !s_scene.arena || !s_scene.physical) return ESP_ERR_INVALID_ARG;
    const pack_check_t *check = pack_check(pack);
    if (!check) return ESP_ERR_INVALID_CRC;
    const fp_pack_info_t info = check->info;
    fp_error_t error = FP_OK;
    uint32_t seed = esp_random() ^ info.payload_crc32;
    /* Init reuses the existing arena. If it fails, the old player has already
     * been overwritten and must never remain reachable through s_scene. The
     * pack was proven above, so binding only checks its structure. */
    forget_player();
    fp_player_t *player = fp_player_init_prevalidated(
        s_scene.arena, fp_arena_size(), pack->start, pack_bytes(pack), seed,
        &error);
    if (!player) {
        forget_player();
        ESP_LOGE(TAG, "full-frame player init failed for %s: %s",
                 pack->id, fp_error_string(error));
        return ESP_FAIL;
    }
    s_scene.player = player;
    s_scene.bound_external = pack == &s_external;
    strlcpy(s_scene.active_id, pack->id, sizeof(s_scene.active_id));
    s_scene.shown_expression = PET_EXPRESSION_IDLE;
    s_scene.shown_state_valid = false;
    s_scene.touch_was_pressed = false;
    s_scene.pending_dirty = full_dirty();
    s_scene.pending_dirty_valid = true;
    s_scene.presented_revision = fp_visual_revision(player);
    expand_dirty(fp_framebuffer(player), full_dirty());
    sync_canvas_buffer(full_dirty());
    if (s_scene.canvas) lv_obj_invalidate(s_scene.canvas);
    s_scene.last_present_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "full-frame pack %.*s v%.*s id=%s bytes=%lu frames=%u emotions=%u actions=%u",
             info.name_len, (const char *)info.name,
             info.version_len, (const char *)info.version, pack->id,
             (unsigned long)info.pack_bytes, info.frame_count,
             info.emotion_count,
             fp_motion_capabilities(player).action_count);
    return ESP_OK;
}

esp_err_t pet_face_pack_use_external(const char *id,const void *bytes,size_t length)
{
    if(!id||!id[0]||strlen(id)>=sizeof(s_external_id)||!bytes||length>UINT32_MAX)return ESP_ERR_INVALID_ARG;
    fp_pack_info_t info={0};
    fp_error_t result=fp_inspect_prevalidated(bytes,(uint32_t)length,&info);
    /* The caller passes the face ID it already matched to these bytes: the
     * pack's own ID, or a signed imported release's derived one. The pack is
     * registered and selected under that face ID. */
    if(result!=FP_OK||!info.approved||!pet_face_id_matches(id,info.id,info.id_len,true)){
        ESP_LOGE(TAG,"installed pack %s rejected: %s",id,result==FP_OK?"identity or approval":fp_error_string(result));
        return ESP_ERR_INVALID_CRC;
    }
    strcpy(s_external_id,id);
    s_external=(embedded_pack_t){s_external_id,bytes,(const uint8_t *)bytes+length,0};
    s_external_check=(pack_check_t){.checked=true,.valid=true,.info=info};
    /* Force a rebind even if a newer build has the same face ID. */
    return s_scene.arena?bind_pack(&s_external):ESP_OK;
}

void pet_face_pack_release_external(void)
{
    /* Track the binding independently of the mutable external descriptor: a
     * failed replacement registration may already have changed its address. */
    if (s_scene.bound_external) forget_player();
    s_scene.holding = false;
    memset(&s_external, 0, sizeof(s_external));
    memset(s_external_id, 0, sizeof(s_external_id));
    memset(&s_external_check, 0, sizeof(s_external_check));
}

void pet_face_pack_hold_external(void)
{
    bool held = s_scene.bound_external || s_scene.holding;
    pet_face_pack_release_external();
    s_scene.holding = held;
}

esp_err_t pet_face_pack_create(lv_obj_t *parent, const char *preferred_id,
                               lv_obj_t **root_out)
{
    if (!parent || !root_out) return ESP_ERR_INVALID_ARG;
    lv_display_t *display = lv_obj_get_display(parent);
    int32_t width = lv_display_get_horizontal_resolution(display);
    int32_t height = lv_display_get_vertical_resolution(display);
    int32_t side = width > height ? width : height;
    if (width <= 0 || height <= 0 || width > INT16_MAX || height > INT16_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* The scaler writes tightly packed RGB565 rows. Reject dimensions that
     * LVGL would pad instead of displaying misaligned rows. */
    if (lv_draw_buf_width_to_stride((uint32_t)side, LV_COLOR_FORMAT_RGB565) != (uint32_t)side * 2u)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_scene, 0, sizeof(s_scene));
    s_scene.display_size = (uint16_t)side;
    s_scene.arena = frame_alloc(fp_arena_size());
    s_scene.physical = frame_alloc((size_t)side * side * sizeof(uint16_t));
    if (!s_scene.arena || !s_scene.physical) return ESP_ERR_NO_MEM;

    s_scene.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scene.root);
    /* Cover rectangular panels with the same uniformly scaled square artwork.
     * The viewport clips the centered canvas without letterboxing or stretching. */
    lv_obj_set_size(s_scene.root, width, height);
    lv_obj_center(s_scene.root);
    lv_obj_remove_flag(s_scene.root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_scene.canvas = lv_canvas_create(s_scene.root);
    lv_canvas_set_buffer(s_scene.canvas, s_scene.physical,
                         side, side,
                         LV_COLOR_FORMAT_RGB565);
    lv_draw_buf_t *draw_buffer = lv_canvas_get_draw_buf(s_scene.canvas);
    if (draw_buffer) lv_draw_buf_set_flag(draw_buffer, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_obj_center(s_scene.canvas);
    lv_obj_remove_flag(s_scene.canvas, LV_OBJ_FLAG_CLICKABLE);

    const embedded_pack_t *pack = find_pack(preferred_id);
#if !CONFIG_PET_VNEXT_ENROLLMENT
    if (!pack) pack = find_pack("pablo");
#endif
    esp_err_t result = bind_pack(pack);
#if !CONFIG_PET_VNEXT_ENROLLMENT
    if (result != ESP_OK && pack && strcmp(pack->id, "pablo")) {
        result = bind_pack(find_pack("pablo"));
    }
#endif
    if (result != ESP_OK) return result;
    s_scene.last_stats_us = esp_timer_get_time();
    *root_out = s_scene.root;
    return ESP_OK;
}

esp_err_t pet_face_pack_select(const char *id)
{
    const embedded_pack_t *pack = find_pack(id);
    if (!pack) return ESP_ERR_NOT_FOUND;
    if (!strcmp(id, s_scene.active_id)) return ESP_OK;
    return bind_pack(pack);
}

esp_err_t pet_face_pack_list(pet_face_catalog_item_t *items, size_t capacity,
                             size_t *count_out)
{
    if (!count_out || (capacity && !items)) return ESP_ERR_INVALID_ARG;
    size_t count = 0;
    for (size_t index = 0; index < 1+sizeof(s_embedded) / sizeof(s_embedded[0]);
         ++index) {
        const embedded_pack_t *pack = index?&s_embedded[index-1]:&s_external;
        if(!pack->id)continue;
        pack_check_t *check = pack_check(pack);
        if (!check) continue;
        const fp_pack_info_t info = check->info;
        if (count < capacity) {
            pet_face_catalog_item_t *item = &items[count];
            memset(item, 0, sizeof(*item));
            strlcpy(item->id, pack->id, sizeof(item->id));
            size_t name_length = info.name_len < sizeof(item->name) - 1 ?
                info.name_len : sizeof(item->name) - 1;
            memcpy(item->name, info.name, name_length);
            item->name[name_length] = 0;
            size_t version_length = info.version_len < sizeof(item->version) - 1 ?
                info.version_len : sizeof(item->version) - 1;
            memcpy(item->version, info.version, version_length);
            item->version[version_length] = 0;
            item->pack_bytes = info.pack_bytes;
            item->format_version=(uint16_t)pack->start[8]|
                                 ((uint16_t)pack->start[9]<<8);
            /* Hashed once per boot, and never for the installed pack: its
             * size would stall the caller and its installer knows the hash. */
            uint8_t digest[32];
            if(pack!=&s_external&&!check->sha256[0]&&mbedtls_sha256(pack->start,item->pack_bytes,digest,0)==0){
                static const char hex[]="0123456789abcdef";
                for(size_t byte=0;byte<sizeof(digest);++byte){
                    check->sha256[byte*2]=hex[digest[byte]>>4];
                    check->sha256[byte*2+1]=hex[digest[byte]&0x0f];
                }
                check->sha256[64]='\0';
            }
            strlcpy(item->sha256,check->sha256,sizeof(item->sha256));
            item->payload_crc32 = info.payload_crc32;
            item->physical_slot = pack->slot;
            item->gender = info.gender;
            item->tier = 2;
            item->state = PET_FACE_CATALOG_RESIDENT;
        }
        count++;
    }
    *count_out = count < capacity ? count : capacity;
    return count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t pet_face_pack_identity(const char *id, pet_face_gender_t *gender_out)
{
    if (!gender_out) return ESP_ERR_INVALID_ARG;
    const embedded_pack_t *pack = find_pack(id);
    if (!pack) return ESP_ERR_NOT_FOUND;
    const pack_check_t *check = pack_check(pack);
    if (!check) return ESP_ERR_INVALID_CRC;
    *gender_out = check->info.gender < PET_FACE_GENDER_COUNT ?
        (pet_face_gender_t)check->info.gender : PET_FACE_GENDER_NEUTRAL;
    return ESP_OK;
}

esp_err_t pet_face_pack_current_id(char *id, size_t capacity)
{
    if (!id || !capacity) return ESP_ERR_INVALID_ARG;
    if (!s_scene.player || !s_scene.active_id[0]) return ESP_ERR_INVALID_STATE;
    strlcpy(id, s_scene.active_id, capacity);
    return ESP_OK;
}

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
                               bool *rendered_out)
{
    (void)viseme;
    (void)touch_x_q88;
    (void)touch_y_q88;
    if (rendered_out) *rendered_out = false;
    if (!s_scene.player || !s_scene.canvas) return s_scene.holding && s_scene.canvas ? ESP_OK : ESP_ERR_INVALID_STATE;
    int64_t started_us = esp_timer_get_time();
    s_scene.last_now_ms = now_ms;

    /* Every pet follows real device inputs. Authored idle performances remain
     * the shared director's responsibility, not a name-triggered demo. */
    fp_set_sys_state(s_scene.player, (fp_sys_state_t)state);
    fp_set_animation_profile(s_scene.player, (fp_animation_profile_t)profile);
#if CONFIG_PET_POCKET_TERMINAL
    /* A companion's idle: the pack's own performances at their authored gaps,
     * and after a stretch of calm one of its signature moves. */
    fp_set_idle_mode(s_scene.player, FP_IDLE_NATURAL);
#else
    fp_set_idle_showcase_enabled(s_scene.player, 1);
#endif
    fp_set_audio_level(s_scene.player, audio_level_to_percent(audio_level));
    int16_t tilt_x = imu_x_q88 / 2;
    int16_t tilt_y = imu_y_q88 / 2;
    if (tilt_x < -127) tilt_x = -127;
    if (tilt_x > 127) tilt_x = 127;
    if (tilt_y < -127) tilt_y = -127;
    if (tilt_y > 127) tilt_y = 127;
    if (!s_scene.gesture_until_ms ||
        (int32_t)(now_ms - s_scene.gesture_until_ms) >= 0) {
        fp_set_tilt(s_scene.player, (int8_t)tilt_x, (int8_t)tilt_y);
        s_scene.gesture_until_ms = 0;
    }
    if (expression != s_scene.shown_expression) {
        s_scene.shown_expression = expression;
        fp_set_emotion(s_scene.player, expression_to_emotion(expression));
    }
    if (pressed && !s_scene.touch_was_pressed) {
        fp_notify_touch(s_scene.player);
    }
    s_scene.touch_was_pressed = pressed;

    fp_tick(s_scene.player);
    uint32_t revision = fp_visual_revision(s_scene.player);
    if (revision != s_scene.presented_revision) {
        union_pending_dirty(fp_dirty_rect(s_scene.player));
    }

    int64_t now_us = esp_timer_get_time();
    bool state_changed = !s_scene.shown_state_valid || state != s_scene.shown_state;
    bool due = state_changed || state != PET_FACE_SPEAKING ||
        now_us - s_scene.last_present_us >= SPEAKING_PRESENT_INTERVAL_US;
    if (visible && revision != s_scene.presented_revision && due) {
        fp_dirty_rect_t dirty = s_scene.pending_dirty_valid ?
            s_scene.pending_dirty : full_dirty();
        if ((uint32_t)dirty.width * dirty.height >
            ((uint32_t)fp_canvas_width(s_scene.player) * fp_canvas_height(s_scene.player) * 3u) / 5u) dirty = full_dirty();
        int64_t present_started_us = esp_timer_get_time();
        expand_dirty(fp_framebuffer(s_scene.player), dirty);
        sync_canvas_buffer(dirty);
        invalidate_dirty(dirty);
        int64_t present_us = esp_timer_get_time() - present_started_us;
        if (present_us > s_scene.max_present_us) s_scene.max_present_us = present_us;
        s_scene.presented_revision = revision;
        s_scene.pending_dirty_valid = false;
        s_scene.presentations++;
        s_scene.last_present_us = now_us;
        s_scene.shown_state = state;
        s_scene.shown_state_valid = true;
        fp_report_health(s_scene.player, 1, (uint16_t)(present_us / 1000));
        if (rendered_out) *rendered_out = true;
    } else if (revision != s_scene.presented_revision) {
        s_scene.coalesced_presentations++;
    }

    int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (elapsed_us > s_scene.max_tick_us) s_scene.max_tick_us = elapsed_us;
    s_scene.ticks++;
    if (now_us - s_scene.last_stats_us >= 10000000) {
        fp_motion_capabilities_t capabilities =
            fp_motion_capabilities(s_scene.player);
        ESP_LOGI(TAG,
                 "id=%s ticks=%lu present=%lu coalesced=%lu frame=%u dirty=%dx%d actions=%u maxTick=%lldus maxPresent=%lldus",
                 s_scene.active_id, (unsigned long)s_scene.ticks,
                 (unsigned long)s_scene.presentations,
                 (unsigned long)s_scene.coalesced_presentations,
                 fp_debug_frame(s_scene.player),
                 fp_debug_dirty_width(s_scene.player),
                 fp_debug_dirty_height(s_scene.player),
                 capabilities.action_count,
                 (long long)s_scene.max_tick_us,
                 (long long)s_scene.max_present_us);
        s_scene.ticks = 0;
        s_scene.presentations = 0;
        s_scene.coalesced_presentations = 0;
        s_scene.max_tick_us = 0;
        s_scene.max_present_us = 0;
        s_scene.last_stats_us = now_us;
    }
    return ESP_OK;
}

uint8_t pet_face_pack_trigger_gesture(uint8_t gesture)
{
    if (!s_scene.player || gesture == FC_GESTURE_NONE ||
        gesture >= FC_GESTURE_COUNT) return FC_GESTURE_NONE;
    switch (gesture) {
        case FC_GESTURE_SPIN_CCW:
            fp_set_tilt(s_scene.player, -100, 0);
            break;
        case FC_GESTURE_SPIN_CW:
            fp_set_tilt(s_scene.player, 100, 0);
            break;
        case FC_GESTURE_NOD:
            fp_notify_gesture(s_scene.player, FP_GESTURE_NOD);
            break;
        case FC_GESTURE_ZOOM_IN:
            fp_notify_gesture(s_scene.player, FP_GESTURE_FOCUS);
            break;
        case FC_GESTURE_HEARTBEAT:
            fp_notify_gesture(s_scene.player, FP_GESTURE_HEARTBEAT);
            break;
        case FC_GESTURE_BOUNCE:
            fp_notify_gesture(s_scene.player, FP_GESTURE_BOUNCE);
            break;
        case FC_GESTURE_SHAKE:
            fp_notify_shake(s_scene.player);
            break;
        case FC_GESTURE_WOBBLE:
            fp_notify_gesture(s_scene.player, FP_GESTURE_WOBBLE);
            break;
        case FC_GESTURE_POP:
            fp_notify_gesture(s_scene.player, FP_GESTURE_POP);
            break;
        default:
            fp_notify_touch(s_scene.player);
            break;
    }
    s_scene.gesture_until_ms = s_scene.last_now_ms + 900;
    return gesture;
}

bool pet_face_pack_touch(void)
{
    if (!s_scene.player) return false;
    fp_notify_touch(s_scene.player);
    return true;
}

bool pet_face_pack_gesture_active(void)
{
    return s_scene.gesture_until_ms &&
        (int32_t)(s_scene.gesture_until_ms - s_scene.last_now_ms) > 0;
}

bool pet_face_pack_candidate_pending(void)
{
    return false;
}

uint32_t pet_face_pack_frame_crc(void)
{
    return s_scene.player ? fp_frame_crc32(s_scene.player) : 0;
}
