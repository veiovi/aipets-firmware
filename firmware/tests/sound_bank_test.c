/* The sound bank (pet_sound_bank.c): the SBK1 parser, and a bank downloaded
 * into slot 1's region of a simulated pet store, checked, recorded in NVS and
 * read in again after a reboot. Flash, NVS and the server are fakes; the
 * module, its parser and SHA-256 are real. */
#include "pet_sound_bank.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define SLOT_1 0u

/* The pet store: slot 0 holds a pet, slot 1's region the bank. */
static uint8_t flash[0x200000];
static const esp_partition_t store = {.address = 0x3c0000, .size = sizeof(flash), .label = "pet_sound"};
static bool record_gone_before_writes = true;
static bool table_locked;

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == 0x44 && !strcmp(label, "pet_sound"));
    return &store;
}

static void inside_slot_1(const esp_partition_t *partition, size_t offset, size_t length)
{
    assert(partition == &store && length <= sizeof(flash) - offset);
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *bytes, size_t length)
{
    assert(!table_locked); /* Lookups stay available throughout driver I/O. */
    inside_slot_1(partition, offset, length);
    memcpy(bytes, flash + offset, length);
    return ESP_OK;
}

/* NVS: the bank's record. */
static uint8_t record[72];
static size_t record_length;
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    (void)mode;
    assert(!strcmp(name, "aipet"));
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    (void)handle;
    assert(!strcmp(key, "sound_bank"));
    if (!record_length) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < record_length) return ESP_FAIL;
    memcpy(value, record, record_length);
    *length = record_length;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    assert(!strcmp(key, "sound_bank") && length == sizeof(record));
    memcpy(record, value, length);
    record_length = length;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    assert(!strcmp(key, "sound_bank"));
    if (!record_length) return ESP_ERR_NVS_NOT_FOUND;
    record_length = 0;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

/* Writes reach only slot 1's region, never before the old record is gone. */
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t length)
{
    inside_slot_1(partition, offset, length);
    assert(!(offset % 4096) && !(length % 4096));
    record_gone_before_writes = record_gone_before_writes && !record_length;
    memset(flash + offset, 0xff, length);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *bytes, size_t length)
{
    inside_slot_1(partition, offset, length);
    for (size_t i = 0; i < length; ++i)
        flash[offset + i] &= ((const uint8_t *)bytes)[i];
    return ESP_OK;
}

/* The server: `served` by ranges of at most 64 KiB, or a failure. */
static const uint8_t *served;
static uint32_t served_bytes;
static unsigned ranges, fail_next;
bool pet_control_http_sound_bank_range(const pet_control_http_t *http, pet_control_download_t *download,
                                       const char *sha256, uint32_t bank_bytes, uint32_t offset, void *response,
                                       size_t bytes, pet_control_http_result_t *result)
{
    (void)http;
    (void)download;
    (void)sha256;
    assert(bank_bytes == served_bytes && bytes && bytes <= 0x10000 && offset + bytes <= bank_bytes);
    ++ranges;
    result->retry_seconds = 7;
    if (fail_next && fail_next--) return false;
    memcpy(response, served + offset, bytes);
    return true;
}
void pet_control_download_wrote(pet_control_download_t *download) { (void)download; }
void pet_control_download_end(pet_control_download_t *download, bool complete) { (void)download; (void)complete; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int mutex; return &mutex; }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{
    (void)mutex;
    (void)wait;
    assert(!table_locked);
    table_locked = true;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    (void)mutex;
    assert(table_locked);
    table_locked = false;
    return pdTRUE;
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) { (void)caps; return calloc(count, size); }
void heap_caps_free(void *memory) { free(memory); }

static void put16(uint8_t *at, uint16_t value) { at[0] = (uint8_t)value; at[1] = (uint8_t)(value >> 8); }
static void put32(uint8_t *at, uint32_t value) { put16(at, (uint16_t)value); put16(at + 2, (uint16_t)(value >> 16)); }

typedef struct {
    const char *id;
    unsigned blocks;
    uint16_t rate;
    uint8_t kind;
} sound_t;
/* A bank of `count` sounds in id order, each block filled with its index. */
static uint32_t bank(uint8_t *out, const sound_t *sounds, unsigned count)
{
    const uint32_t data = 16 + 36 * count;
    uint32_t at = 0;
    memset(out, 0, data);
    memcpy(out, "SBK1", 4);
    put16(out + 4, 1);
    put16(out + 6, (uint16_t)count);
    put32(out + 8, data);
    for (unsigned i = 0; i < count; ++i)
    {
        uint8_t *entry = out + 16 + 36 * i;
        memcpy(entry, sounds[i].id, strlen(sounds[i].id));
        put32(entry + 24, at);
        put32(entry + 28, sounds[i].blocks * 256);
        put16(entry + 32, sounds[i].rate);
        entry[34] = sounds[i].kind;
        memset(out + data + at, 'A' + (int)i, sounds[i].blocks * 256);
        at += sounds[i].blocks * 256;
    }
    put32(out + 12, at);
    return data + at;
}
static void hex_sha256(const uint8_t *bytes, size_t length, char out[65])
{
    uint8_t digest[32];
    assert(!mbedtls_sha256(bytes, length, digest, 0));
    for (unsigned i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

static const sound_t SOUNDS[] = {
    {"campfire-night", 300, 12000, PET_SOUND_LOOP},
    {"dice-wood-1", 47, 16000, PET_SOUND_ONE_SHOT},
    {"wonder-relic", 187, 16000, PET_SOUND_ONE_SHOT},
};
static uint8_t file[0x80000], copy[0x10000];
static pet_sound_t sounds[PET_SOUND_BANK_SOUNDS];

static void refused(const uint8_t *head, size_t length, uint32_t bytes)
{
    assert(!pet_sound_bank_parse(head, length, bytes, sounds));
}

/* Valid, truncated, bad magic, an entry out of range, and each malformed field. */
static void parser(void)
{
    const uint32_t bytes = bank(file, SOUNDS, 3);
    const size_t head = 16 + 3 * 36;
    assert(pet_sound_bank_parse(file, head, bytes, sounds) == 3);
    assert(!strcmp(sounds[0].id, "campfire-night") && sounds[0].kind == PET_SOUND_LOOP && sounds[0].rate == 12000);
    assert(sounds[0].offset == head && sounds[0].bytes == 300 * 256);
    assert(!strcmp(sounds[2].id, "wonder-relic") && sounds[2].offset == head + 347 * 256);
    assert(sounds[2].bytes == 187 * 256);
    refused(file, 15, bytes);
    refused(file, head - 1, bytes);
    refused(file, head, bytes - 1);
    static uint8_t bad[16 + 3 * 36];
    const struct { size_t at; uint8_t value; } fields[] = {
        {0, 'X'}, {4, 2}, {6, 0}, {6, 129}, {7, 1},          /* magic, version, count */
        {8, 16}, {12, 0xff},                                 /* dataOffset, dataBytes */
        {16, 'C'}, {16, 0}, {16 + 20, 'x'}, {16 + 14, '_'},  /* ids: upper case, empty, after the NUL, a character */
        {16 + 36 + 28, 1}, {16 + 36 + 29, 0},                /* sizes: not whole blocks, none */
        {16 + 72 + 30, 1},                                   /* out of range: past the data */
        {16 + 32, 0}, {16 + 34, 2}, {16 + 35, 1},            /* rate, kind, reserved */
        {16 + 36, 'z'},                                      /* ids out of order */
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
    {
        memcpy(bad, file, sizeof(bad));
        bad[fields[i].at] = fields[i].value;
        refused(bad, sizeof(bad), bytes);
    }
    sound_t twice[] = {SOUNDS[1], SOUNDS[1]};
    refused(file, sizeof(file), bank(file, twice, 2));
}

static void serve(const uint8_t *bytes, uint32_t length, char sha256[65])
{
    served = bytes;
    served_bytes = length;
    hex_sha256(bytes, length, sha256);
}
/* Steps until nothing is left to do; returns how many worked. */
static unsigned steps(uint64_t now_ms)
{
    const pet_control_http_t http = {0};
    unsigned worked = 0;
    while (pet_sound_bank_step(&http, copy, sizeof(copy), now_ms)) ++worked;
    return worked;
}

static void download(void)
{
    char sha256[65];
    size_t bytes = 0;
    uint32_t rate = 0;
    memset(flash, 0xff, sizeof(flash));
    /* A legacy sound-bank record must be forgotten even if its old hash
     * happens to name the requested bank. */
    memset(record, 'a', 64);
    record_length = 68;
    pet_sound_bank_open(true);
    assert(!record_length);
    assert(pet_sound_bank_find("dice-wood-1", PET_SOUND_ONE_SHOT, NULL) < 0 && !steps(0));
    serve(file, bank(file, SOUNDS, 3), sha256);
    pet_sound_bank_want(sha256, served_bytes);
    /* A range that fails is tried again after the server's delay. */
    fail_next = 1;
    assert(steps(1000) == 1 && !steps(7999) && ranges == 1);
    const unsigned needed = (served_bytes + 0xffff) / 0x10000;
    assert(steps(8000) == needed && ranges == needed + 1);
    assert(record_length == 72 && !memcmp(record, sha256, 64) && record_gone_before_writes);
    assert(!memcmp(flash + SLOT_1, file, served_bytes));
    int dice = pet_sound_bank_find("dice-wood-1", PET_SOUND_ONE_SHOT, NULL);
    assert(dice == 1 && pet_sound_bank_find("dice-wood-1", PET_SOUND_LOOP, NULL) < 0);
    assert(pet_sound_bank_find("campfire-night", PET_SOUND_LOOP, NULL) == 0 &&
           pet_sound_bank_find("nothing", 0, NULL) < 0);
    assert(pet_sound_bank_copy(dice, copy, sizeof(copy), &bytes, &rate) && bytes == 47 * 256 && rate == 16000);
    assert(copy[0] == 'B' && copy[bytes - 1] == 'B');
    assert(!pet_sound_bank_copy(0, copy, 300 * 256 - 1, &bytes, &rate)); /* Larger than the buffer. */
    /* Named again, it is kept: nothing downloads. */
    pet_sound_bank_want(sha256, served_bytes);
    assert(!steps(9000) && ranges == needed + 1);

    /* After a reboot the record reads it in again, without a download. */
    pet_sound_bank_open(true);
    pet_sound_bank_want(sha256, served_bytes);
    assert(!steps(0) && ranges == needed + 1 && pet_sound_bank_find("wonder-relic", PET_SOUND_ONE_SHOT, NULL) == 2);
    /* Not while older firmware's pet holds slot 1. */
    pet_sound_bank_open(false);
    assert(pet_sound_bank_find("wonder-relic", PET_SOUND_ONE_SHOT, NULL) < 0);

    /* A new bank: the old one stops playing and its record goes first. Bytes
     * that fail its SHA-256 are refused and never tried again this boot. */
    pet_sound_bank_open(true);
    static uint8_t second[0x80000];
    sound_t one[] = {{"hit-stab", 47, 16000, PET_SOUND_ONE_SHOT}};
    char named[65];
    serve(second, bank(second, one, 1), named);
    second[served_bytes - 1] ^= 1;
    pet_sound_bank_want(named, served_bytes);
    record_gone_before_writes = true;
    assert(steps(9000) == 1 && record_gone_before_writes && !record_length);
    assert(pet_sound_bank_find("dice-wood-1", PET_SOUND_ONE_SHOT, NULL) < 0 &&
           pet_sound_bank_find("hit-stab", 0, NULL) < 0);
    pet_sound_bank_want(named, served_bytes);
    assert(!steps(10000));
    /* Three failures wait until sound.bank names the bank again. */
    sound_t other[] = {{"hit-stab", 48, 16000, PET_SOUND_ONE_SHOT}};
    serve(second, bank(second, other, 1), named);
    pet_sound_bank_want(named, served_bytes);
    fail_next = 3;
    assert(steps(20000) == 1 && steps(27000) == 1 && steps(34000) == 1 && !steps(99000));
    pet_sound_bank_want(named, served_bytes);
    assert(steps(99000) == 1 && pet_sound_bank_find("hit-stab", PET_SOUND_ONE_SHOT, NULL) == 0 && record_length);

    /* Bytes that match their SHA-256 but don't read as a bank are refused too, and never recorded, so a restart
     * doesn't download them again; nothing plays. */
    static uint8_t broken[0x1000];
    serve(broken, sizeof(broken), named);
    pet_sound_bank_want(named, served_bytes);
    assert(steps(100000) == 1 && !record_length && pet_sound_bank_find("hit-stab", 0, NULL) < 0);
    pet_sound_bank_want(named, served_bytes);
    assert(!steps(101000));
}

/* A bank file read as the firmware reads it: a line for each sound. */
static int print_bank(const char *path)
{
    FILE *bank_file = fopen(path, "rb");
    assert(bank_file && !fseek(bank_file, 0, SEEK_END));
    const long bytes = ftell(bank_file);
    rewind(bank_file);
    const size_t head = fread(file, 1, sizeof(file), bank_file);
    fclose(bank_file);
    const unsigned count = pet_sound_bank_parse(file, head, (uint32_t)bytes, sounds);
    for (unsigned i = 0; i < count; ++i)
        printf("%s %u %u %lu %lu\n", sounds[i].id, sounds[i].kind, sounds[i].rate, (unsigned long)sounds[i].offset,
               (unsigned long)sounds[i].bytes);
    return count ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return print_bank(argv[1]);
    parser();
    download();
    puts("sound bank: SBK1 parsed strictly, downloaded into slot 1 only, retried, checked, recorded, "
         "read in after a reboot, refused when it fails its hash");
    return 0;
}
