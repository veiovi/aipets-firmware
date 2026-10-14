#include "pet_atmosphere.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "pet_ima_adpcm.h"
#include "pet_sound_bank.h"

/* firmware/main/atmospheres and the sound bank's sounds (pet_sound_bank.h):
 * mono IMA ADPCM in 256-byte blocks that each decode on their own
 * (pet_ima_adpcm.h). A loop's last block flows into its first; a one-shot
 * ends after its last. The built-in loops are 12 kHz. */
#define LOOP_RATE 12000u
#define BLOCK_BYTES 256u
#define BLOCK_SAMPLES ((BLOCK_BYTES - PET_IMA_ADPCM_HEADER_BYTES) * 2u)
/* A loop fades in over this, and moves to a new level (speech, hold) as fast. */
#define FADE_MS 300u
/* Back after the codec reopened under it (a new reply). */
#define RESUME_MS 100u
/* PET_ATMOSPHERE_NONE fades a loop out over this. */
#define OFF_MS 500u
/* Under loud speech the loop gives way at once, and comes back over this. */
#define DUCK_RECOVER_MS 200u
#define DUCK_UNITY 32768
#define NO_CUE 0xffu
#define PHASE_ONE 65536u
/* A loop is a built-in atmosphere, or from here on a bank loop's index. */
#define BANK_LOOP ((unsigned)PET_ATMOSPHERE_COUNT)
/* A bank sound plays from its copy in PSRAM, never from flash: one for the
 * loop and one for the one-shot, each allocated when first needed. */
#define COPY_BYTES 0x10000u
/* At most three pending bank-bound cues. */
#define CUES_MAX 3u

/* In pet_atmosphere_t order. EMBED_FILES names each file's bytes. */
#define EACH_LOOP(X) X(calm) X(forest) X(water) X(night) X(cave) X(storm) X(danger) X(battle) X(magic) X(victory)
#define DECLARE_LOOP(name) \
    extern const uint8_t name##_start[] __asm__("_binary_" #name "_adpcm_start"); \
    extern const uint8_t name##_end[] __asm__("_binary_" #name "_adpcm_end");
#define LOOP_ENTRY(name) { #name, name##_start, name##_end },
EACH_LOOP(DECLARE_LOOP)

static const struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} LOOPS[] = { { NULL, NULL, NULL }, EACH_LOOP(LOOP_ENTRY) };
_Static_assert(sizeof(LOOPS) / sizeof(LOOPS[0]) == PET_ATMOSPHERE_COUNT, "a loop for every atmosphere");

/* A sound's decoder: its blocks, the next block, the next sample of the block
 * decoded, and the two samples of the sound the output falls between (Q16
 * phase). */
typedef struct {
    const uint8_t *blocks;
    size_t count;
    uint32_t rate;
    bool loop;
    uint16_t block;
    uint16_t index;
    int16_t s0;
    int16_t s1;
    uint32_t phase;
    int16_t *decoded;
} voice_t;

/* In PSRAM, allocated with the first loop or one-shot. */
typedef struct {
    int16_t decoded[BLOCK_SAMPLES];
    int16_t sound_decoded[BLOCK_SAMPLES];
    /* The loop with the one-shot over it, before they are added. */
    int16_t loop[PET_ATMOSPHERE_BLOCK_MAX];
    int16_t block[PET_ATMOSPHERE_BLOCK_MAX];
} loop_buffers_t;

static const char *TAG = "pet_atmosphere";
/* The latest cue not yet taken, or NO_CUE. */
static atomic_uint s_cue = NO_CUE;
/* A stinger not yet started: bank generation and index token, or 0. */
static atomic_uint s_stinger;
static atomic_flag s_cues_lock = ATOMIC_FLAG_INIT;
static atomic_uint s_cues_reset, s_cues_revision, s_cues_count;
static unsigned s_cues[CUES_MAX], s_cue_count;
static atomic_bool s_drop_pending;
static loop_buffers_t *s_buffers;
/* The media worker owns LOADING, playback owns PLAYING. Bytes and metadata
 * transfer with the atomic state; the playback task never waits on a reader. */
enum
{
    COPY_EMPTY,
    COPY_LOADING,
    COPY_READY,
    COPY_PLAYING,
    COPY_FAILED
};
typedef struct
{
    atomic_uint state, wanted;
    uint8_t *data;
    size_t bytes;
    uint32_t rate;
    unsigned token;
} sound_copy_t;
static sound_copy_t s_loop_copy, s_sound_copy;
static unsigned s_loop_waiting = NO_CUE;
static unsigned s_sound_waiting;

static void copy_drop(sound_copy_t *copy)
{
    atomic_store(&copy->wanted, 0);
    unsigned state = atomic_load(&copy->state);
    while (state != COPY_LOADING && state != COPY_EMPTY &&
           !atomic_compare_exchange_weak(&copy->state, &state, COPY_EMPTY))
    {
    }
}

static void copy_prepare(sound_copy_t *copy)
{
    unsigned wanted = atomic_load(&copy->wanted), expected = COPY_EMPTY;
    if (!wanted || !atomic_compare_exchange_strong(&copy->state, &expected, COPY_LOADING)) return;
    if (!copy->data) copy->data = heap_caps_calloc(1, COPY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t generation = wanted >> 8;
    bool ok = generation == pet_sound_bank_generation() && copy->data &&
              pet_sound_bank_copy((int)(wanted & 255u) - 1, copy->data, COPY_BYTES, &copy->bytes, &copy->rate);
    copy->token = wanted;
    if (wanted != atomic_load(&copy->wanted)) atomic_store(&copy->state, COPY_EMPTY);
    else atomic_store(&copy->state, ok && generation == pet_sound_bank_generation() ? COPY_READY : COPY_FAILED);
}

void pet_atmosphere_prepare(void)
{
    copy_prepare(&s_loop_copy);
    copy_prepare(&s_sound_copy);
}

/* 1 ready, 0 loading, -1 failed. Called only after the old voice stopped. */
static int copy_take(sound_copy_t *copy, unsigned token)
{
    if ((token >> 8) != pet_sound_bank_generation())
    {
        copy_drop(copy);
        return -1;
    }
    unsigned state = atomic_load(&copy->state);
    if (state == COPY_READY || state == COPY_FAILED)
    {
        if (copy->token == token)
        {
            atomic_store(&copy->wanted, 0);
            atomic_store(&copy->state, state == COPY_READY ? COPY_PLAYING : COPY_EMPTY);
            return state == COPY_READY ? 1 : -1;
        }
        atomic_store(&copy->state, COPY_EMPTY);
    }
    atomic_store(&copy->wanted, token);
    return 0;
}
/* Guarded by pet_audio.c's codec lock. */
static struct {
    unsigned playing;
    /* Fading out to silence, then `after` plays (NONE: the loop ends). */
    bool fading;
    unsigned after;
    /* The speaker's. */
    uint32_t rate;
    uint32_t ramp_ms;
    /* Q30 gains: the level now, where it heads, and its change per sample. */
    int32_t gain;
    int32_t target;
    int32_t step;
    /* Q15: how far the sounds give way to the speech over them. */
    int32_t duck;
    /* Whose blocks the voice holds. */
    unsigned decoded;
    voice_t voice;
} s_loop;
/* The one-shot over the loop. Guarded by the codec lock. */
static struct {
    bool playing;
    voice_t voice;
} s_sound;

pet_atmosphere_t pet_atmosphere_from_name(const char *name)
{
    for (int i = PET_ATMOSPHERE_NONE + 1; name && i < PET_ATMOSPHERE_COUNT; i++) {
        if (!strcmp(name, LOOPS[i].name)) return (pet_atmosphere_t)i;
    }
    return PET_ATMOSPHERE_NONE;
}

void pet_atmosphere_cue(pet_atmosphere_t atmosphere)
{
    if ((unsigned)atmosphere >= PET_ATMOSPHERE_COUNT) atmosphere = PET_ATMOSPHERE_NONE;
    atomic_store(&s_cue, (unsigned)atmosphere);
}

/* A request keeps the bank revision observed under its lookup lock. The low
 * byte holds index + 1; the upper 24 bits are the bank's generation. */
static unsigned sound_token(const char *id, pet_sound_kind_t kind)
{
    uint32_t generation = 0;
    int index = pet_sound_bank_find(id, kind, &generation);
    return index < 0 ? 0 : (generation << 8) | ((unsigned)index + 1u);
}

void pet_atmosphere_cue_name(const char *name)
{
    pet_atmosphere_t built_in = pet_atmosphere_from_name(name);
    unsigned token = name && built_in == PET_ATMOSPHERE_NONE ? sound_token(name, PET_SOUND_LOOP) : 0;
    atomic_store(&s_cue, token ? (token & ~255u) | ((token & 255u) - 1u + BANK_LOOP) : (unsigned)built_in);
}

/* A bounded try-lock never waits on the audio task. A busy dequeue waits for
 * the next audio slice; an overlapping enqueue is best-effort, like a full queue. */
static void sync_cues(void)
{
    unsigned revision = atomic_load(&s_cues_reset);
    if (revision != atomic_load(&s_cues_revision))
    {
        s_cue_count = 0;
        atomic_store(&s_cues_count, 0);
        atomic_store(&s_cues_revision, revision);
    }
}

bool pet_atmosphere_queue_sound(const char *id)
{
    unsigned token = sound_token(id, PET_SOUND_ONE_SHOT);
    if (!token || atomic_flag_test_and_set(&s_cues_lock)) return false;
    sync_cues();
    bool queued = s_cue_count < CUES_MAX;
    if (queued) s_cues[s_cue_count++] = token;
    atomic_store(&s_cues_count, s_cue_count);
    atomic_flag_clear(&s_cues_lock);
    return queued;
}

/* 0 empty, UINT32_MAX busy, otherwise the oldest bank-bound token. */
static unsigned next_cue(void)
{
    if (atomic_flag_test_and_set(&s_cues_lock)) return UINT32_MAX;
    sync_cues();
    unsigned token = s_cue_count ? s_cues[0] : 0;
    if (s_cue_count)
    {
        --s_cue_count;
        memmove(s_cues, s_cues + 1, s_cue_count * sizeof(s_cues[0]));
    }
    atomic_store(&s_cues_count, s_cue_count);
    atomic_flag_clear(&s_cues_lock);
    return token;
}

bool pet_atmosphere_play_sound(const char *id)
{
    unsigned token = sound_token(id, PET_SOUND_ONE_SHOT);
    if (!token) return false;
    atomic_store(&s_stinger, token);
    return true;
}

void pet_atmosphere_drop_sounds(void)
{
    atomic_fetch_add(&s_cues_reset, 1);
    atomic_store(&s_stinger, 0);
    atomic_store(&s_drop_pending, true);
}

static void drop_waiting_sound(void)
{
    if (atomic_exchange(&s_drop_pending, false) && s_sound_waiting != 0)
    {
        s_sound_waiting = 0;
        copy_drop(&s_sound_copy);
    }
}

bool pet_atmosphere_sounds_waiting(void)
{
    return atomic_load(&s_cues_revision) == atomic_load(&s_cues_reset) && atomic_load(&s_cues_count) != 0;
}

static bool buffers(void)
{
    if (!s_buffers) {
        s_buffers = heap_caps_calloc(1, sizeof(*s_buffers), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_loop.voice.decoded = s_buffers ? s_buffers->decoded : NULL;
        s_sound.voice.decoded = s_buffers ? s_buffers->sound_decoded : NULL;
    }
    return s_buffers != NULL;
}

/* The sound's next sample; false after a one-shot's last. */
static bool voice_sample(voice_t *v, int16_t *sample)
{
    if (v->index >= BLOCK_SAMPLES) {
        if (v->block >= v->count && !v->loop) return false;
        if (v->block >= v->count) v->block = 0;
        if (pet_ima_adpcm_decode(v->blocks + (size_t)v->block * BLOCK_BYTES, BLOCK_BYTES, v->decoded,
                                 BLOCK_SAMPLES) != BLOCK_SAMPLES) {
            memset(v->decoded, 0, BLOCK_SAMPLES * sizeof(int16_t));
        }
        v->block++;
        v->index = 0;
    }
    *sample = v->decoded[v->index++];
    return true;
}

static void voice_start(voice_t *v, const uint8_t *blocks, size_t bytes, uint32_t rate, bool loop)
{
    v->blocks = blocks;
    v->count = bytes / BLOCK_BYTES;
    v->rate = rate;
    v->loop = loop;
    v->block = 0;
    v->index = BLOCK_SAMPLES;
    v->phase = PHASE_ONE;
    voice_sample(v, &v->s1);
}

/* The next output sample, linearly interpolated from the sound's rate across
 * blocks and a loop's seam; false once a one-shot has played to its end. */
static bool voice_next(voice_t *v, int16_t *out)
{
    while (v->phase >= PHASE_ONE) {
        v->phase -= PHASE_ONE;
        v->s0 = v->s1;
        if (!voice_sample(v, &v->s1)) return false;
    }
    *out = (int16_t)(v->s0 + (int32_t)(((int64_t)(v->s1 - v->s0) * v->phase) >> 16));
    v->phase += (v->rate << 16) / s_loop.rate;
    return true;
}

static void set_ramp(int32_t level_q15, uint32_t ms)
{
    int32_t samples = (int32_t)((uint64_t)ms * s_loop.rate / 1000u);
    s_loop.target = level_q15 << 15;
    s_loop.step = (s_loop.target - s_loop.gain) / (samples > 0 ? samples : 1);
    if (!s_loop.step) s_loop.step = s_loop.target > s_loop.gain ? 1 : -1;
}

/* A loop's blocks: a built-in's in the app image, a bank loop's copied out of
 * the bank. */
static int loop_blocks(unsigned loop, const uint8_t **blocks, size_t *bytes, uint32_t *rate)
{
    if (loop < BANK_LOOP) {
        *blocks = LOOPS[loop].start;
        *bytes = (size_t)(LOOPS[loop].end - LOOPS[loop].start);
        *rate = LOOP_RATE;
        return *bytes >= BLOCK_BYTES ? 1 : -1;
    }
    int ready = copy_take(&s_loop_copy, (loop & ~255u) | ((loop & 255u) - BANK_LOOP + 1u));
    if (ready == 1)
    {
        *blocks = s_loop_copy.data;
        *bytes = s_loop_copy.bytes;
        *rate = s_loop_copy.rate;
    }
    return ready;
}

static void start(unsigned loop)
{
    const uint8_t *blocks = NULL;
    size_t bytes = 0;
    uint32_t rate = 0;
    /* The loop played last carries on where it was. */
    if (loop != s_loop.decoded) {
        if (s_loop.decoded >= BANK_LOOP) copy_drop(&s_loop_copy);
        s_loop.decoded = PET_ATMOSPHERE_NONE;
        int ready = buffers() ? loop_blocks(loop, &blocks, &bytes, &rate) : -1;
        if (ready != 1)
        {
            s_loop.playing = PET_ATMOSPHERE_NONE;
            s_loop_waiting = ready == 0 ? loop : NO_CUE;
            if (ready < 0) ESP_LOGW(TAG, "loop %u does not play", loop);
            return;
        }
        s_loop.decoded = loop;
        voice_start(&s_loop.voice, blocks, bytes, rate, true);
    }
    s_loop_waiting = NO_CUE;
    s_loop.playing = loop;
    s_loop.fading = false;
    s_loop.gain = 0;
    s_loop.target = 0;
    s_loop.ramp_ms = FADE_MS;
    s_loop.duck = DUCK_UNITY;
}

/* Start once the media task publishes the immutable one-shot copy. */
static void start_sound(unsigned token)
{
    if (s_sound_waiting != token)
    {
        s_sound.playing = false;
        copy_drop(&s_sound_copy);
        s_sound_waiting = token;
    }
    int ready = buffers() ? copy_take(&s_sound_copy, token) : -1;
    if (ready == 0) return;
    s_sound_waiting = 0;
    s_sound.playing = ready == 1;
    if (s_sound.playing) voice_start(&s_sound.voice, s_sound_copy.data, s_sound_copy.bytes, s_sound_copy.rate, false);
    else ESP_LOGW(TAG, "sound %u does not play", token & 255u);
}

static void finish_fade(void)
{
    s_loop.fading = false;
    s_loop.playing = PET_ATMOSPHERE_NONE;
    if (s_loop.after != PET_ATMOSPHERE_NONE) start(s_loop.after);
    else
    {
        copy_drop(&s_loop_copy);
        s_loop.decoded = PET_ATMOSPHERE_NONE;
    }
}

static void apply_cue(unsigned cue)
{
    if (s_loop_waiting != NO_CUE)
    {
        s_loop_waiting = NO_CUE;
        copy_drop(&s_loop_copy);
    }
    if (cue == s_loop.playing) {
        /* Asked for again: it keeps playing, even one fading out. */
        s_loop.fading = false;
    } else if (s_loop.playing == PET_ATMOSPHERE_NONE || !s_loop.gain) {
        if (cue == PET_ATMOSPHERE_NONE) s_loop.playing = PET_ATMOSPHERE_NONE;
        else start(cue);
    } else {
        /* Out first, then the next one in: never a jump. */
        s_loop.after = cue;
        if (!s_loop.fading) set_ramp(0, cue == PET_ATMOSPHERE_NONE ? OFF_MS : FADE_MS);
        s_loop.fading = true;
    }
}

/* Takes new cues; true while a loop or a one-shot plays. */
static bool take_cue(uint32_t rate)
{
    drop_waiting_sound();
    unsigned cue = atomic_exchange(&s_cue, NO_CUE);
    unsigned stinger = atomic_exchange(&s_stinger, 0);
    s_loop.rate = rate;
    if (cue != NO_CUE) apply_cue(cue);
    if (s_loop_waiting != NO_CUE) start(s_loop_waiting);
    if (stinger) start_sound(stinger);
    else if (s_sound_waiting != 0) start_sound(s_sound_waiting);
    return (s_loop.playing != PET_ATMOSPHERE_NONE || s_sound.playing) && rate;
}

/* The loop's next sample at its gain, which ramps; a fade that reaches
 * silence ends it. */
static int32_t loop_sample(void)
{
    if (s_loop.gain != s_loop.target) {
        s_loop.gain += s_loop.step;
        if ((s_loop.step > 0) == (s_loop.gain > s_loop.target)) s_loop.gain = s_loop.target;
    }
    int16_t sample = 0;
    voice_next(&s_loop.voice, &sample);
    int32_t scaled = (sample * (s_loop.gain >> 15)) >> 15;
    if (s_loop.fading && !s_loop.gain) finish_fade();
    return scaled;
}

/* The loop, and the one-shot over it, into s_buffers->loop; returns the
 * samples written before both ended, if they did. */
static size_t render(size_t samples, int32_t level_q15)
{
    /* Under a one-shot the loop gives way to half its level. */
    if (s_sound.playing) level_q15 /= 2;
    if (!s_loop.fading && s_loop.target != level_q15 << 15) {
        set_ramp(level_q15, s_loop.ramp_ms);
        s_loop.ramp_ms = FADE_MS;
    }
    size_t count = 0;
    for (; count < samples && (s_loop.playing != PET_ATMOSPHERE_NONE || s_sound.playing); count++) {
        int32_t sample = s_loop.playing != PET_ATMOSPHERE_NONE ? loop_sample() : 0;
        int16_t sound = 0;
        if (s_sound.playing && voice_next(&s_sound.voice, &sound)) sample += sound;
        else if (s_sound.playing)
        {
            s_sound.playing = false;
            copy_drop(&s_sound_copy);
        }
        s_buffers->loop[count] = (int16_t)(sample > INT16_MAX ? INT16_MAX : sample < INT16_MIN ? INT16_MIN : sample);
    }
    return count;
}

/* Adds the rendered sounds into `pcm` within the headroom the speech leaves
 * there, so the sum stays below full scale. Their scale drops at once and
 * rises over DUCK_RECOVER_MS, linearly across the slice. Returns the samples
 * that still clipped: none, unless the headroom is miscounted. */
static uint32_t add_loop(int16_t *pcm, size_t samples)
{
    uint32_t speech_peak = 0;
    uint32_t loop_peak = 0;
    for (size_t i = 0; i < samples; i++) {
        uint32_t speech = (uint32_t)abs(pcm[i]);
        uint32_t loop = (uint32_t)abs(s_buffers->loop[i]);
        if (speech > speech_peak) speech_peak = speech;
        if (loop > loop_peak) loop_peak = loop;
    }
    int32_t target = DUCK_UNITY;
    if (speech_peak >= INT16_MAX - 1) {
        target = 0;
    } else if (speech_peak + loop_peak > INT16_MAX - 1) {
        target = (int32_t)(((INT16_MAX - 1 - speech_peak) << 15) / loop_peak);
    }
    int32_t rise = (int32_t)((uint64_t)DUCK_UNITY * samples * 1000u / ((uint64_t)DUCK_RECOVER_MS * s_loop.rate));
    int32_t from = s_loop.duck;
    int32_t to = target < from + rise ? target : from + rise;
    if (to < from) from = to;
    uint32_t clipped = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t duck = from + (int32_t)((int64_t)(to - from) * (int64_t)i / (int64_t)samples);
        int32_t mixed = pcm[i] + ((s_buffers->loop[i] * duck) >> 15);
        if (mixed > INT16_MAX || mixed < INT16_MIN) {
            clipped++;
            mixed = mixed > INT16_MAX ? INT16_MAX : INT16_MIN;
        }
        pcm[i] = (int16_t)mixed;
    }
    s_loop.duck = to;
    return clipped;
}

uint32_t pet_atmosphere_mix(int16_t *pcm, size_t samples, uint32_t rate, int32_t level_q15)
{
    if (!pcm || samples > PET_ATMOSPHERE_BLOCK_MAX || !take_cue(rate)) return 0;
    return add_loop(pcm, render(samples, level_q15));
}

int16_t *pet_atmosphere_block(size_t samples, uint32_t rate, int32_t level_q15)
{
    if (samples > PET_ATMOSPHERE_BLOCK_MAX || !take_cue(rate)) return NULL;
    memset(s_buffers->block, 0, samples * sizeof(int16_t));
    add_loop(s_buffers->block, render(samples, level_q15));
    return s_buffers->block;
}

bool pet_atmosphere_active(void)
{
    return s_loop.playing != PET_ATMOSPHERE_NONE || s_loop_waiting != NO_CUE || s_sound.playing ||
           s_sound_waiting != 0 || atomic_load(&s_stinger);
}

bool pet_atmosphere_looping(void) { return s_loop.playing != PET_ATMOSPHERE_NONE; }

bool pet_atmosphere_play_queued(void)
{
    drop_waiting_sound();
    while (!s_sound.playing) {
        unsigned next = s_sound_waiting ? s_sound_waiting : next_cue();
        if (next == UINT32_MAX) return true;
        if (!next) return false;
        start_sound(next);
        if (s_sound_waiting != 0) return true;
    }
    return true;
}

void pet_atmosphere_fade_out(uint32_t ms)
{
    if (s_loop.playing == PET_ATMOSPHERE_NONE || s_loop.fading) return;
    s_loop.after = PET_ATMOSPHERE_NONE;
    s_loop.fading = true;
    set_ramp(0, ms);
}

void pet_atmosphere_stop(bool resume)
{
    if (s_loop.fading) finish_fade();
    s_loop.gain = 0;
    s_loop.target = 0;
    s_loop.ramp_ms = RESUME_MS;
    s_sound.playing = false;
    s_sound_waiting = 0;
    copy_drop(&s_sound_copy);
    if (resume) return;
    s_loop_waiting = NO_CUE;
    copy_drop(&s_loop_copy);
    s_loop.decoded = PET_ATMOSPHERE_NONE;
    s_loop.playing = PET_ATMOSPHERE_NONE;
    pet_atmosphere_drop_sounds();
}
