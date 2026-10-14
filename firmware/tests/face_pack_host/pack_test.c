/* Real pack renderer, player and LVGL; only allocation/time and an injected
 * player-init failure replace hardware. ASan detects stale flash references. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "frame_player.h"
#include "frame_stream.h"
#include "../../components/frame_player/src/frame_internal.h"
static const char *test_face_id;
static unsigned touch_calls, shake_calls, gesture_calls;
static void test_notify_touch(fp_player_t *);
static void test_notify_shake(fp_player_t *);
static void test_notify_gesture(fp_player_t *, fp_gesture_t);
static size_t test_strlcpy(char *dst, const char *src, size_t capacity)
{
    size_t length = strlen(src);
    if (capacity) { size_t n = length < capacity - 1 ? length : capacity - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
#define strlcpy test_strlcpy
static fp_player_t *test_player_init(void *, uint32_t, const void *, uint32_t, uint32_t, fp_error_t *);
#define fp_player_init_prevalidated test_player_init
#define fp_notify_touch test_notify_touch
#define fp_notify_shake test_notify_shake
#define fp_notify_gesture test_notify_gesture
#include "../../main/pet_face_pack.c"
#undef fp_player_init_prevalidated
#undef fp_notify_touch
#undef fp_notify_shake
#undef fp_notify_gesture
static void test_notify_touch(fp_player_t *p) { ++touch_calls; fp_notify_touch(p); }
static void test_notify_shake(fp_player_t *p) { ++shake_calls; fp_notify_shake(p); }
static void test_notify_gesture(fp_player_t *p, fp_gesture_t g) { ++gesture_calls; fp_notify_gesture(p, g); }

static unsigned allocation_failure;
static bool init_failure;
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{
    (void)caps;
    if (allocation_failure && !--allocation_failure) return NULL;
    void *memory = NULL;
    return posix_memalign(&memory, alignment, size) ? NULL : memory;
}
void heap_caps_free(void *memory) { free(memory); }
uint32_t esp_random(void) { return 17; }
int64_t esp_timer_get_time(void) { return 1000000; }
static fp_player_t *test_player_init(void *arena, uint32_t bytes, const void *pack,
                                    uint32_t length, uint32_t seed, fp_error_t *error)
{
    if (init_failure) {
        memset(arena, 0xa5, bytes); /* A failed init may destroy the old player. */
        *error = FP_ERR_ARENA;
        return NULL;
    }
    return fp_player_init_prevalidated(arena, bytes, pack, length, seed, error);
}
static esp_err_t tick(void)
{
    bool rendered = true;
    esp_err_t result = pet_face_pack_update(PET_FACE_IDLE, PET_EXPRESSION_IDLE,
        0, 0, false, 0, 0, 0, 0, PET_ANIMATION_FULL, 100, true, &rendered);
    if (result != ESP_OK) assert(!rendered);
    return result;
}
static void assert_detached(void)
{
    assert(!s_scene.player && !s_scene.bound_external && !s_external.start && !s_external.id);
    assert(!s_scene.gesture_until_ms && !s_scene.pending_dirty_valid);
    for (size_t i = 0; i < fp_arena_size(); ++i) assert(!((uint8_t *)s_scene.arena)[i]);
    assert(tick() == ESP_ERR_INVALID_STATE);
    assert(!pet_face_pack_frame_crc() && !pet_face_pack_gesture_active());
    assert(pet_face_pack_trigger_gesture(FC_GESTURE_SHAKE) == FC_GESTURE_NONE);
    char id[PET_FACE_ID_MAX];
    assert(pet_face_pack_current_id(id, sizeof(id)) == ESP_ERR_INVALID_STATE);
    assert(pet_face_pack_select(test_face_id) == ESP_ERR_NOT_FOUND);
    pet_face_gender_t gender;
    assert(pet_face_pack_identity(test_face_id, &gender) == ESP_ERR_NOT_FOUND);
    pet_face_catalog_item_t item;
    size_t count = 99;
    assert(pet_face_pack_list(&item, 1, &count) == ESP_ERR_NOT_FOUND && !count);
}
static void assert_live_inputs(void)
{
    bool rendered;
    touch_calls = shake_calls = gesture_calls = 0;
    /* Cross the old three-minute trigger and every scripted demo slot. */
    for (uint32_t now = 0; now <= 360000; now += 1000) {
        assert(pet_face_pack_update(PET_FACE_IDLE, PET_EXPRESSION_IDLE,
            0, 0, false, 0, 0, 40, -60, PET_ANIMATION_REDUCED,
            now, true, &rendered) == ESP_OK);
        assert(s_scene.player->sys_state == FP_SYS_IDLE);
        assert(s_scene.player->audio_level == 0);
        assert(s_scene.player->animation_profile == FP_PROFILE_REDUCED);
#if CONFIG_PET_POCKET_TERMINAL
        assert(s_scene.player->idle_showcase_enabled == FP_IDLE_NATURAL);
#else
        assert(s_scene.player->idle_showcase_enabled == FP_IDLE_SHOWCASE);
#endif
        assert(s_scene.player->tilt_x == 20 && s_scene.player->tilt_y == -30);
        assert(touch_calls == 0 && shake_calls == 0 && gesture_calls == 0);
    }
    for (int state = PET_FACE_BOOTING; state <= PET_FACE_ERROR; ++state) {
        assert(pet_face_pack_update((pet_face_state_t)state, PET_EXPRESSION_HAPPY,
            128, 0, false, 0, 0, 400, -400, PET_ANIMATION_BALANCED,
            361000 + state, true, &rendered) == ESP_OK);
        assert(s_scene.player->sys_state == state);
        assert(s_scene.player->audio_level == 50);
        assert(s_scene.player->animation_profile == FP_PROFILE_BALANCED);
        /* The adapter's int8 bound is further clamped to +/-100 by the player. */
        assert(s_scene.player->tilt_x == 100 && s_scene.player->tilt_y == -100);
    }
    for (unsigned pressed = 0; pressed < 4; ++pressed) {
        assert(pet_face_pack_update(PET_FACE_SPEAKING, PET_EXPRESSION_IDLE,
            pressed ? 255 : 0, 0, pressed != 0, 0, 0, 0, 0, PET_ANIMATION_FULL,
            362000 + pressed, true, &rendered) == ESP_OK);
        assert(s_scene.player->audio_level == (pressed ? 100 : 0));
        assert(touch_calls == (pressed ? 1u : 0u));
    }
    assert(pet_face_pack_trigger_gesture(FC_GESTURE_SHAKE) == FC_GESTURE_SHAKE);
    assert(shake_calls == 1);
    assert(pet_face_pack_trigger_gesture(FC_GESTURE_POP) == FC_GESTURE_POP);
    assert(gesture_calls == 1);
    /* Existing gesture tilt owns its interval, then real IMU input resumes. */
    s_scene.gesture_until_ms = 365000;
    fp_set_tilt(s_scene.player, 12, -15);
    assert(pet_face_pack_update(PET_FACE_IDLE, PET_EXPRESSION_IDLE,
        0, 0, false, 0, 0, 80, -80, PET_ANIMATION_FULL,
        364000, false, &rendered) == ESP_OK);
    assert(s_scene.player->tilt_x == 12 && s_scene.player->tilt_y == -15);
    assert(pet_face_pack_update(PET_FACE_IDLE, PET_EXPRESSION_IDLE,
        0, 0, false, 0, 0, 80, -80, PET_ANIMATION_FULL,
        365000, true, &rendered) == ESP_OK);
    assert(s_scene.player->tilt_x == 40 && s_scene.player->tilt_y == -40);
    assert(!s_scene.gesture_until_ms);
}

/* A story event (pet_face_pack_play_event) over the bound pet `id`, here the
 * synthetic, unapproved fuzz seed, whose base-plane ambient loop shows frames
 * 0, 2 and 1 for 6 ticks each: it plays that loop three times whatever the pet
 * does, then the pet's player is bound again. Stopped early or held, it ends
 * at once, each event exactly once. The face stays the pet's, and the
 * unapproved pack can never be registered as a pet. */
static unsigned events_ended;
static unsigned stream_reads;
static int stream_read(void *context, uint32_t offset, void *out, uint32_t bytes)
{
    ++stream_reads;
    memcpy(out, (const uint8_t *)context + offset, bytes);
    return 1;
}

static void event_ended(void) { ++events_ended; }
static void assert_story_event(const char *id, const uint8_t *event, size_t length)
{
    bool rendered;
    char face[PET_FACE_ID_MAX];
    const fp_pack_info_t pet = *fp_pack_info(s_scene.player); /* Its id points into the pet's bytes. */
    assert(pet_face_pack_play_event(event, length, event_ended) == ESP_OK);
    assert(pet_face_pack_play_event(event, length, event_ended) == ESP_ERR_INVALID_STATE); /* one at a time */
    static const uint16_t frames[] = {0, 2, 1};
    assert(!fp_pack_info(s_scene.player)->approved);
    assert(pet_face_pack_current_id(face, sizeof(face)) == ESP_OK && !strcmp(face, id));
    const uint32_t loop = fp_idle_loop_ticks(s_scene.player);
    assert(loop == 18);
    touch_calls = 0;
    assert(pet_face_pack_trigger_gesture(FC_GESTURE_SHAKE) == FC_GESTURE_NONE && !pet_face_pack_touch());
    for (uint32_t t = 0; t < 3 * loop; ++t) {
        assert(pet_face_pack_update(PET_FACE_SPEAKING, PET_EXPRESSION_ANGRY, 255, 0, t & 1, 0, 0, 400, 400,
                                    PET_ANIMATION_FULL, 1000 + t, true, &rendered) == ESP_OK);
        assert(s_scene.player->sys_state == FP_SYS_IDLE && fp_debug_frame(s_scene.player) == frames[(t / 6) % 3]);
        assert(!events_ended && !touch_calls);
    }
    assert(tick() == ESP_OK && events_ended == 1 && fp_pack_info(s_scene.player)->id == pet.id && !s_scene.event_done);
    /* Stopped early, then stopping again changes nothing. */
    assert(pet_face_pack_play_event(event, length, event_ended) == ESP_OK && tick() == ESP_OK);
    pet_face_pack_stop_event();
    pet_face_pack_stop_event();
    assert(events_ended == 2 && fp_pack_info(s_scene.player)->id == pet.id);
    /* The pet's copy is about to hold another pet: the event ends with it. */
    assert(pet_face_pack_play_event(event, length, event_ended) == ESP_OK);
    pet_face_pack_hold_external();
    assert(events_ended == 3 && s_scene.holding && !s_scene.player);
    assert(pet_face_pack_play_event(event, length, event_ended) == ESP_ERR_INVALID_STATE);
    assert(pet_face_pack_use_external("fuzz-seed", event, length) == ESP_ERR_INVALID_CRC);
}

int main(int argc, char **argv)
{
    assert(argc >= 2 && argc <= 5);
    int display_size = argc >= 3 ? atoi(argv[2]) : 360;
    int display_height = argc == 4 ? atoi(argv[3]) : display_size;
    assert(display_size > 0 && display_size <= 502);
    int canvas_side = display_size > display_height ? display_size : display_height;
    test_face_id = argv[1];
    static const struct { const char *name; fp_emotion_t emotion; } cases[] = {
        {"idle", FP_EMO_NEUTRAL}, {"happy", FP_EMO_JOY},
        {"curious", FP_EMO_CURIOUS}, {"surprised", FP_EMO_SURPRISED},
        {"sleepy", FP_EMO_SLEEPY}, {"concerned", FP_EMO_CONCERNED},
        {"excited", FP_EMO_EXCITED}, {"shy", FP_EMO_SHY},
        {"angry", FP_EMO_ANGRY}, {"confused", FP_EMO_CONFUSED},
        {"determined", FP_EMO_DETERMINED}, {"disgust", FP_EMO_DISGUST},
        {"embarrassed", FP_EMO_EMBARRASSED}, {"fear", FP_EMO_FEAR},
        {"sad", FP_EMO_SAD},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pet_expression_t expression = PET_EXPRESSION_IDLE;
        assert(pet_expression_from_wire(cases[i].name, &expression));
        assert(expression_to_emotion(expression) == cases[i].emotion);
    }
    pet_expression_t unchanged = PET_EXPRESSION_HAPPY;
    assert(!pet_expression_from_wire("not-an-expression", &unchanged));
    assert(unchanged == PET_EXPRESSION_HAPPY);
    uint8_t fixture[200000];
    size_t length = fread(fixture, 1, sizeof(fixture), stdin);
    assert(length && length < sizeof(fixture));
    lv_init();
    lv_display_t *display = lv_display_create(display_size, display_height);
    assert(display);
    lv_obj_t *parent = lv_obj_create(NULL), *root = NULL;
    uint8_t *flash = malloc(length);
    memcpy(flash, fixture, length);
    assert(pet_face_pack_use_external(test_face_id, flash, length) == ESP_OK);
    lv_display_set_resolution(display, 0, display_height);
    assert(pet_face_pack_create(parent, test_face_id, &root) == ESP_ERR_INVALID_ARG && !root);
    lv_display_set_resolution(display, 239, 239);
    assert(pet_face_pack_create(parent, test_face_id, &root) == ESP_ERR_INVALID_ARG && !root);
    lv_display_set_resolution(display, display_size, display_height);
    lv_display_t *other_display = lv_display_create(400, 400);
    assert(other_display);
    lv_display_set_default(other_display);
    assert(pet_face_pack_create(parent, test_face_id, &root) == ESP_OK && root);
    lv_obj_update_layout(parent);
    assert(s_scene.display_size == canvas_side);
    assert(lv_obj_get_x(root) == 0 && lv_obj_get_y(root) == 0);
    assert(lv_obj_get_width(root) == display_size && lv_obj_get_height(root) == display_height);
    assert(lv_obj_get_width(s_scene.canvas) == canvas_side);
    assert(lv_obj_get_height(s_scene.canvas) == canvas_side);
    assert(lv_obj_get_x(s_scene.canvas) == (display_size - canvas_side) / 2);
    assert(lv_obj_get_y(s_scene.canvas) == (display_height - canvas_side) / 2);
    assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_SCROLLABLE));
    assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    if (display_size == 410 && display_height == 502) {
        assert(fp_display_touch(-lv_obj_get_x(s_scene.canvas), s_scene.display_size) == -209);
        assert(fp_display_touch(-lv_obj_get_y(s_scene.canvas), s_scene.display_size) == -256);
    }
    if (display_size == 502 && display_height == 410) {
        assert(fp_display_touch(-lv_obj_get_x(s_scene.canvas), s_scene.display_size) == -256);
        assert(fp_display_touch(-lv_obj_get_y(s_scene.canvas), s_scene.display_size) == -209);
    }
    assert(s_scene.bound_external && tick() == ESP_OK);
#if CONFIG_PET_POCKET_TERMINAL
    assert(s_scene.player->idle_showcase_enabled == FP_IDLE_NATURAL); /* a companion's idle */
#else
    assert(s_scene.player->idle_showcase_enabled == FP_IDLE_SHOWCASE);
#endif
    pet_face_pack_trigger_gesture(FC_GESTURE_SHAKE);
    uint16_t *canvas = s_scene.physical;
    uint32_t before = fp_frame_crc32(s_scene.player);
    pet_face_pack_release_external();
    free(flash); /* Every subsequent public reader must survive the unmapped pack. */
    assert_detached();
    assert(s_scene.physical == canvas);
    pet_face_pack_release_external();
    assert_detached();
    pet_face_pack_hold_external(); /* Nothing bound: nothing to hold. */
    assert(!s_scene.holding);
    assert_detached();

    /* Holding while the owner copies another pet into the same bytes: the
     * canvas keeps the last frame, updates succeed without reading the old
     * bytes or drawing, and the next binding resumes. A release ends it. */
    flash = malloc(length);
    memcpy(flash, fixture, length);
    assert(pet_face_pack_use_external(test_face_id, flash, length) == ESP_OK && tick() == ESP_OK);
    size_t shown_bytes = (size_t)canvas_side * canvas_side * sizeof(uint16_t);
    uint16_t *shown = malloc(shown_bytes);
    assert(shown);
    memcpy(shown, s_scene.physical, shown_bytes);
    pet_face_pack_hold_external();
    memset(flash, 0xff, length);
    free(flash);
    for (int i = 0; i < 3; ++i) {
        bool rendered = true;
        assert(pet_face_pack_update(PET_FACE_SPEAKING, PET_EXPRESSION_HAPPY, 200, 0, true, 0, 0, 0, 0,
                                    PET_ANIMATION_FULL, 200 + i, true, &rendered) == ESP_OK && !rendered);
    }
    assert(!memcmp(shown, s_scene.physical, shown_bytes) && !s_scene.player && !s_external.start);
    free(shown);
    assert(pet_face_pack_trigger_gesture(FC_GESTURE_SHAKE) == FC_GESTURE_NONE && !pet_face_pack_touch());
    flash = malloc(length);
    memcpy(flash, fixture, length);
    assert(pet_face_pack_use_external(test_face_id, flash, length) == ESP_OK && !s_scene.holding && tick() == ESP_OK);
    pet_face_pack_hold_external();
    assert(s_scene.holding);
    pet_face_pack_release_external();
    free(flash);
    assert(!s_scene.holding);
    assert_detached();

    /* Rebinding the same face ID after detach must recreate the player in-place. */
    flash = malloc(length);
    memcpy(flash, fixture, length);
    assert(pet_face_pack_use_external(test_face_id, flash, length) == ESP_OK);
    assert(s_scene.bound_external && tick() == ESP_OK);
    assert(pet_face_pack_frame_crc() == before);

    uint8_t *replacement = malloc(length);
    memcpy(replacement, fixture, length);
    /* Registration checks structure, identity and approval only: the caller
     * validated these bytes in full. A damaged header is refused. */
    replacement[20] ^= 1;
    fp_player_t *old = s_scene.player;
    assert(pet_face_pack_use_external(test_face_id, replacement, length) == ESP_ERR_INVALID_CRC);
    assert(s_scene.player == old && tick() == ESP_OK); /* Registration did not commit. */
    replacement[20] ^= 1;
    /* Binding proven bytes allocates nothing, so an exhausted heap cannot fail it. */
    allocation_failure = 1;
    assert(pet_face_pack_use_external(test_face_id, replacement, length) == ESP_OK && allocation_failure == 1);
    allocation_failure = 0;
    assert(s_scene.player && s_scene.bound_external && s_external.start == replacement && tick() == ESP_OK);
    pet_face_pack_release_external();
    free(flash);
    free(replacement);
    assert_detached();

    flash = malloc(length);
    memcpy(flash, fixture, length);
    assert(pet_face_pack_use_external(test_face_id, flash, length) == ESP_OK);
    init_failure = true;
    assert(pet_face_pack_use_external(test_face_id, flash, length) == ESP_FAIL);
    assert(!s_scene.player && !s_scene.bound_external && tick() == ESP_ERR_INVALID_STATE);
    pet_face_pack_release_external();
    free(flash);
    assert_detached();
    init_failure = false;
    assert(pet_face_pack_use_external(test_face_id, fixture, length) == ESP_OK);
    assert(tick() == ESP_OK);

    /* A signed imported release names the face `<pack ID>-<8 hex>`: the pack is
     * registered, bound and selected under that face ID. Other forms fail. */
    char derived[PET_FACE_ID_MAX], current[PET_FACE_ID_MAX];
    snprintf(derived, sizeof(derived), "%s-7c4253d1", test_face_id);
    assert(pet_face_pack_use_external(derived, fixture, length) == ESP_OK && tick() == ESP_OK);
    assert(pet_face_pack_current_id(current, sizeof(current)) == ESP_OK && !strcmp(current, derived));
    assert(pet_face_pack_select(derived) == ESP_OK);
    const char *suffixes[] = {"-7C4253D1", "-7c4253d", "-7c4253d1x", "_7c4253d1"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        snprintf(derived, sizeof(derived), "%s%s", test_face_id, suffixes[i]);
        assert(pet_face_pack_use_external(derived, fixture, length) == ESP_ERR_INVALID_CRC);
    }
    assert(pet_face_pack_use_external(test_face_id, fixture, length) == ESP_OK && tick() == ESP_OK);

    /* The live adapter binds resident spans, returns from story events to
     * those spans, and detaches them without any render-thread reads. */
    void *stream_buffer = NULL;
    assert(!posix_memalign(&stream_buffer, 16, 4000000));
    fp_stream_t *stream = NULL;
    assert(fp_stream_open(stream_buffer, 4000000, length, stream_read, fixture, 1, 256000, &stream) == FP_OK);
    unsigned opened_reads = stream_reads;
    assert(pet_face_pack_use_source(test_face_id, fp_stream_source(stream), fp_stream_info(stream)) == ESP_OK);
    assert(tick() == ESP_OK && s_scene.player->source.frame);
    assert_live_inputs();
    if (argc == 5) {
        FILE *file = fopen(argv[4], "rb");
        assert(file);
        uint8_t *event = malloc(262144);
        size_t event_length = fread(event, 1, 262144, file);
        fclose(file);
        assert(event_length && event_length < 262144);
        assert_story_event(test_face_id, event, event_length);
        free(event);
        assert(pet_face_pack_use_source(test_face_id, fp_stream_source(stream), fp_stream_info(stream)) == ESP_OK);
        assert(tick() == ESP_OK);
    }

    /* Clearing the external catalog must not destroy a separately bound pack. */
    s_scene.bound_external = false;
    old = s_scene.player;
    pet_face_pack_release_external();
    assert(s_scene.player == old && tick() == ESP_OK);
    assert(stream_reads == opened_reads);
    forget_player();
    free(stream_buffer);
    lv_obj_delete(parent);
    free(s_scene.arena);
    free(s_scene.physical);
    memset(&s_scene, 0, sizeof(s_scene));
    lv_display_delete(display);
    lv_display_delete(other_display);
    lv_deinit();
    puts("external renderer: detach/unmap, hold for a new copy, same-ID rebind and partial-init failure passed");
}
