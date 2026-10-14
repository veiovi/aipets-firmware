#include "pet_sound_bank.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "pet_flash_layout.h"

/* Dedicated sound partition in one-pet-10m-v5. */
#define BANK_OFFSET 0u
#define BANK_ADDRESS 0x3c0000u
#define RANGE_BYTES 0x10000u
#define TRIES 3u
/* NVS, namespace aipet: the bank written and checked, as its SHA-256 in hex
 * and its size, little-endian. */
#define RECORD_KEY "sound_bank"
#define RECORD_BYTES 72u
#define HEAD_MAX (PET_SOUND_BANK_HEADER_BYTES + PET_SOUND_BANK_SOUNDS * PET_SOUND_BANK_ENTRY_BYTES)
#define HEX "0123456789abcdef"

static const char *TAG = "pet_sound_bank";

typedef struct {
    char sha256[65];
    uint32_t bytes;
} bank_t;

/* In PSRAM, allocated at open. The table of the bank that plays (`sounds`,
 * `count`) and the bank sound.bank names (`wanted`, `named`) are under
 * s_lock; the rest is the control task's. */
typedef struct {
    pet_sound_t sounds[PET_SOUND_BANK_SOUNDS];
    unsigned count;
    bank_t wanted;
    bool named;
    bank_t stored;
    bank_t refused;
    /* The download: the bank being written, how much of it, and its hash. */
    bank_t downloading;
    uint32_t written;
    mbedtls_sha256_context sha;
    uint8_t failures;
    uint64_t retry_at_ms;
    pet_control_download_t connection;
} state_t;

static SemaphoreHandle_t s_lock;
static const esp_partition_t *s_store;
static state_t *s_bank;
static atomic_uint s_generation, s_readers;

uint32_t pet_sound_bank_generation(void)
{
    return atomic_load(&s_generation) & 0xffffffu;
}

static uint16_t get16(const uint8_t *at)
{
    return (uint16_t)(at[0] | at[1] << 8);
}

static uint32_t get32(const uint8_t *at)
{
    return (uint32_t)get16(at) | (uint32_t)get16(at + 2) << 16;
}

static bool same(const bank_t *a, const bank_t *b)
{
    return a->bytes == b->bytes && !strcmp(a->sha256, b->sha256);
}

/* 1 to 23 of a-z, 0-9 and '-', NUL-padded to 24 bytes. */
static bool read_id(const uint8_t *at, char id[PET_SOUND_ID_BYTES + 1])
{
    const uint8_t *end = memchr(at, 0, PET_SOUND_ID_BYTES + 1);
    const size_t length = end ? (size_t)(end - at) : 0;
    if (!length)
        return false;
    for (size_t i = length; i <= PET_SOUND_ID_BYTES; ++i)
        if (at[i])
            return false;
    memcpy(id, at, length + 1);
    return strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") == length;
}

static bool read_entry(const uint8_t *at, uint32_t data, uint32_t data_bytes, pet_sound_t *sound)
{
    const uint32_t offset = get32(at + 24);
    sound->bytes = get32(at + 28);
    sound->rate = get16(at + 32);
    sound->kind = at[34];
    sound->offset = data + offset;
    return read_id(at, sound->id) && sound->bytes && !(sound->bytes % 256u) && sound->kind <= PET_SOUND_LOOP &&
           !at[35] && (sound->rate == 12000 || sound->rate == 16000) && offset <= data_bytes &&
           sound->bytes <= data_bytes - offset;
}

unsigned pet_sound_bank_parse(const uint8_t *head, size_t length, uint32_t bank_bytes,
                              pet_sound_t sounds[PET_SOUND_BANK_SOUNDS])
{
    if (!head || length < PET_SOUND_BANK_HEADER_BYTES || memcmp(head, "SBK1", 4) || get16(head + 4) != 1)
        return 0;
    const unsigned count = get16(head + 6);
    const uint32_t data = get32(head + 8);
    const uint32_t data_bytes = get32(head + 12);
    const size_t entries = PET_SOUND_BANK_HEADER_BYTES + (size_t)count * PET_SOUND_BANK_ENTRY_BYTES;
    if (!count || count > PET_SOUND_BANK_SOUNDS || length < entries || data < entries || data > bank_bytes ||
        data_bytes > bank_bytes - data)
        return 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const uint8_t *entry = head + PET_SOUND_BANK_HEADER_BYTES + i * PET_SOUND_BANK_ENTRY_BYTES;
        if (!read_entry(entry, data, data_bytes, &sounds[i]) || (i && strcmp(sounds[i - 1].id, sounds[i].id) >= 0))
            return 0;
    }
    return count;
}

static bool record_write(const bank_t *bank);

static bool record_read(bank_t *bank)
{
    nvs_handle_t nvs;
    uint8_t record[RECORD_BYTES];
    size_t length = sizeof(record);
    if (nvs_open("aipet", NVS_READONLY, &nvs) != ESP_OK)
        return false;
    esp_err_t error = nvs_get_blob(nvs, RECORD_KEY, record, &length);
    nvs_close(nvs);
    if (error != ESP_OK)
        return false;
    if (length != RECORD_BYTES || get32(record + 68) != BANK_ADDRESS)
    {
        /* The old 68-byte record names the former slot-1 bank. Clear it on
         * migration; the same key now also binds the physical bank address. */
        record_write(NULL);
        return false;
    }
    memcpy(bank->sha256, record, 64);
    bank->sha256[64] = 0;
    bank->bytes = get32(record + 64);
    return strspn(bank->sha256, HEX) == 64 && bank->bytes && bank->bytes <= PET_SOUND_BANK_MAX_BYTES;
}

/* Records `bank`; NULL erases the record. */
static bool record_write(const bank_t *bank)
{
    nvs_handle_t nvs;
    uint8_t record[RECORD_BYTES];
    esp_err_t error;
    if (nvs_open("aipet", NVS_READWRITE, &nvs) != ESP_OK)
        return false;
    if (bank)
    {
        memcpy(record, bank->sha256, 64);
        for (unsigned i = 0; i < 4; ++i)
            record[64 + i] = (uint8_t)(bank->bytes >> (8 * i));
        for (unsigned i = 0; i < 4; ++i)
            record[68 + i] = (uint8_t)(BANK_ADDRESS >> (8 * i));
        error = nvs_set_blob(nvs, RECORD_KEY, record, sizeof(record));
    }
    else
    {
        error = nvs_erase_key(nvs, RECORD_KEY);
        if (error == ESP_ERR_NVS_NOT_FOUND)
            error = ESP_OK;
    }
    if (error == ESP_OK)
        error = nvs_commit(nvs);
    nvs_close(nvs);
    return error == ESP_OK;
}

static void publish(unsigned count)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bank->count = count;
    atomic_fetch_add(&s_generation, 1);
    xSemaphoreGive(s_lock);
    /* Readers captured their descriptor before this publication. New copies
     * see count zero; drain old reads before any bank erase or table rewrite. */
    if (!count)
        while (atomic_load(&s_readers)) vTaskDelay(1);
}

/* The bank's header and entries, read from flash into the table. */
static unsigned load(uint32_t bytes)
{
    const size_t length = bytes < HEAD_MAX ? bytes : HEAD_MAX;
    uint8_t *head = heap_caps_calloc(1, HEAD_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned count = 0;
    if (head && esp_partition_read(s_store, BANK_OFFSET, head, length) == ESP_OK)
        count = pet_sound_bank_parse(head, length, bytes, s_bank->sounds);
    heap_caps_free(head);
    publish(count);
    return count;
}

void pet_sound_bank_open(bool usable)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    if (!s_bank)
        s_bank = heap_caps_calloc(1, sizeof(*s_bank), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lock || !s_bank)
        return;
    publish(0);
    s_store = usable ? esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x44, "pet_sound") : NULL;
    if (s_store && (s_store->address != BANK_ADDRESS || s_store->size != PET_SOUND_BANK_MAX_BYTES))
        s_store = NULL;
    s_bank->stored = s_bank->refused = s_bank->downloading = (bank_t){0};
    bank_t recorded;
    if (!s_store || !record_read(&recorded))
        return;
    s_bank->stored = recorded;
    if (load(s_bank->stored.bytes))
        ESP_LOGI(TAG, "sound bank %.8s", s_bank->stored.sha256);
    else
    {
        /* Not the bank recorded any more: it downloads again. */
        ESP_LOGW(TAG, "recorded sound bank %.8s does not read", s_bank->stored.sha256);
        s_bank->stored = (bank_t){0};
    }
}

void pet_sound_bank_want(const char *sha256, uint32_t bytes)
{
    if (!s_bank)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_bank->wanted.sha256, sizeof(s_bank->wanted.sha256), "%s", sha256);
    s_bank->wanted.bytes = bytes;
    s_bank->named = true;
    xSemaphoreGive(s_lock);
}

/* A new bank: the old one stops playing, and its record goes, before any of
 * its bytes change. */
static bool begin(const bank_t *bank)
{
    if (!bank->bytes || bank->bytes > PET_SOUND_BANK_MAX_BYTES)
        return false;
    publish(0);
    s_bank->stored = (bank_t){0};
    s_bank->downloading = (bank_t){0};
    if (!record_write(NULL))
        return false;
    pet_control_download_end(&s_bank->connection, false);
    mbedtls_sha256_free(&s_bank->sha);
    mbedtls_sha256_init(&s_bank->sha);
    if (mbedtls_sha256_starts(&s_bank->sha, 0))
        return false;
    s_bank->downloading = *bank;
    s_bank->written = 0;
    ESP_LOGI(TAG, "sound bank %.8s: downloading %lu bytes", bank->sha256, (unsigned long)bank->bytes);
    return true;
}

static bool failed(uint64_t now_ms, unsigned retry_seconds)
{
    ++s_bank->failures;
    s_bank->retry_at_ms = now_ms + retry_seconds * 1000ull;
    ESP_LOGW(TAG, "sound bank %.8s: range at %lu failed", s_bank->downloading.sha256,
             (unsigned long)s_bank->written);
    return true;
}

/* `bytes` at `at`, written in pet_sound and read back. */
static bool write_range(uint32_t at, const uint8_t *data, size_t bytes)
{
    const size_t erase = (bytes + PET_FLASH_SECTOR_BYTES - 1u) & ~(size_t)(PET_FLASH_SECTOR_BYTES - 1u);
    uint8_t back[512];
    if (esp_partition_erase_range(s_store, BANK_OFFSET + at, erase) != ESP_OK ||
        esp_partition_write(s_store, BANK_OFFSET + at, data, bytes) != ESP_OK)
        return false;
    for (size_t done = 0; done < bytes; done += sizeof(back))
    {
        const size_t piece = bytes - done < sizeof(back) ? bytes - done : sizeof(back);
        if (esp_partition_read(s_store, BANK_OFFSET + at + done, back, piece) != ESP_OK ||
            memcmp(back, data + done, piece))
            return false;
    }
    return true;
}

/* All of it is written: its SHA-256 decides whether it is kept. */
static void finish(void)
{
    uint8_t digest[32];
    char hex[65];
    const bool hashed = !mbedtls_sha256_finish(&s_bank->sha, digest);
    for (unsigned i = 0; i < sizeof(digest); ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    const bool kept = hashed && !strcmp(hex, s_bank->downloading.sha256);
    pet_control_download_end(&s_bank->connection, kept);
    if (!kept)
    {
        ESP_LOGE(TAG, "sound bank %.8s refused: its SHA-256 differs", s_bank->downloading.sha256);
        s_bank->refused = s_bank->downloading;
    }
    else if (!load(s_bank->downloading.bytes))
    {
        /* The bytes named, but not a bank this firmware reads: never recorded, so a restart doesn't keep it, and
         * refused like a wrong one until another bank is named. */
        ESP_LOGE(TAG, "sound bank %.8s refused: it does not read", s_bank->downloading.sha256);
        s_bank->refused = s_bank->downloading;
    }
    else
    {
        if (!record_write(&s_bank->downloading))
            ESP_LOGW(TAG, "sound bank %.8s not recorded: a restart downloads it again", s_bank->downloading.sha256);
        s_bank->stored = s_bank->downloading;
        ESP_LOGI(TAG, "sound bank %.8s ready", s_bank->stored.sha256);
    }
    s_bank->downloading = (bank_t){0};
}

bool pet_sound_bank_step(const pet_control_http_t *http, uint8_t *buffer, size_t capacity, uint64_t now_ms)
{
    if (!s_bank || !s_store || !http || !buffer || capacity < RANGE_BYTES)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bank_t wanted = s_bank->wanted;
    const bool named = s_bank->named;
    s_bank->named = false;
    xSemaphoreGive(s_lock);
    if (named)
        s_bank->failures = 0;
    if (!wanted.sha256[0] || same(&wanted, &s_bank->stored) || same(&wanted, &s_bank->refused) ||
        s_bank->failures >= TRIES || now_ms < s_bank->retry_at_ms)
        return false;
    if (!same(&wanted, &s_bank->downloading) && !begin(&wanted))
        return failed(now_ms, 30);
    const uint32_t at = s_bank->written;
    const size_t bytes = wanted.bytes - at < RANGE_BYTES ? wanted.bytes - at : RANGE_BYTES;
    pet_control_http_result_t result = {.retry_seconds = 30};
    if (!pet_control_http_sound_bank_range(http, &s_bank->connection, wanted.sha256, wanted.bytes, at, buffer,
                                           bytes, &result))
        return failed(now_ms, result.retry_seconds);
    /* What was read back equals `buffer`, which the hash then takes. */
    if (!write_range(at, buffer, bytes))
        return failed(now_ms, 30);
    pet_control_download_wrote(&s_bank->connection);
    if (mbedtls_sha256_update(&s_bank->sha, buffer, bytes))
    {
        s_bank->downloading = (bank_t){0};
        return failed(now_ms, 30);
    }
    s_bank->written = at + (uint32_t)bytes;
    s_bank->failures = 0;
    if (s_bank->written == wanted.bytes)
        finish();
    return true;
}

int pet_sound_bank_find(const char *id, pet_sound_kind_t kind, uint32_t *generation)
{
    int found = -1;
    if (!s_bank || !id)
        return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_bank->count && found < 0; ++i)
        if (s_bank->sounds[i].kind == kind && !strcmp(s_bank->sounds[i].id, id))
            found = (int)i;
    if (found >= 0 && generation) *generation = pet_sound_bank_generation();
    xSemaphoreGive(s_lock);
    return found;
}

bool pet_sound_bank_copy(int index, uint8_t *buffer, size_t capacity, size_t *bytes, uint32_t *rate)
{
    if (!s_bank || index < 0 || !buffer || !bytes || !rate) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool available = (unsigned)index < s_bank->count && s_bank->sounds[index].bytes <= capacity;
    pet_sound_t sound = {0};
    if (available)
    {
        sound = s_bank->sounds[index];
        atomic_fetch_add(&s_readers, 1);
    }
    xSemaphoreGive(s_lock);
    if (!available) return false;
    /* Small reads: each borrows an internal buffer as large as itself
     * (pet_slot_store.c STREAM_READ_BYTES). */
    bool copied = true;
    for (uint32_t at = 0; copied && at < sound.bytes; at += 1024u)
    {
        const uint32_t piece = sound.bytes - at < 1024u ? sound.bytes - at : 1024u;
        copied = esp_partition_read(s_store, BANK_OFFSET + sound.offset + at, buffer + at, piece) == ESP_OK;
    }
    *bytes = sound.bytes;
    *rate = sound.rate;
    atomic_fetch_sub(&s_readers, 1);
    return copied;
}
