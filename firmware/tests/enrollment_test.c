#include "pet_enrollment.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t records[2][PET_ENROLLMENT_RECORD_BYTES];
    bool exists[2];
    unsigned writes, random_calls, requests, redeems, completions;
    unsigned fail_write;
    bool commit_then_fail;
    int read_error;
    pet_enrollment_reply_t replies[8];
    char id[PET_ENROLLMENT_DEVICE_ID_BYTES];
    char credential[PET_ENROLLMENT_CREDENTIAL_BYTES];
} fake_t;

static int read_record(void *context, unsigned slot, uint8_t *record)
{
    fake_t *fake = context;
    if (fake->read_error) return -1;
    if (!fake->exists[slot]) return 0;
    memcpy(record, fake->records[slot], PET_ENROLLMENT_RECORD_BYTES);
    return 1;
}

static bool write_record(void *context, unsigned slot, const uint8_t *record)
{
    fake_t *fake = context;
    bool fail = ++fake->writes == fake->fail_write;
    if (!fail || fake->commit_then_fail) {
        memcpy(fake->records[slot], record, PET_ENROLLMENT_RECORD_BYTES);
        fake->exists[slot] = true;
    }
    return !fail;
}

static bool random_bytes(void *context, uint8_t *bytes, size_t length)
{
    fake_t *fake = context;
    ++fake->random_calls;
    for (size_t i = 0; i < length; ++i) bytes[i] = (uint8_t)i;
    return true;
}

static pet_enrollment_reply_t request(void *context, bool redeem, const char *id,
                                     const char *credential, const char *code)
{
    fake_t *fake = context;
    assert(fake->writes >= 2); /* both copies committed before any HTTP */
    if (fake->id[0]) {
        assert(!strcmp(fake->id, id));
        assert(!strcmp(fake->credential, credential));
    } else {
        strcpy(fake->id, id);
        strcpy(fake->credential, credential);
    }
    if (redeem) { ++fake->redeems; assert(code && !strcmp(code, "A2B3")); }
    else { ++fake->completions; assert(!code); }
    assert(fake->requests < 8);
    return fake->replies[fake->requests++];
}

static pet_enrollment_io_t io(fake_t *fake)
{
    return (pet_enrollment_io_t){.read=read_record, .write=write_record,
        .random=random_bytes, .request=request, .context=fake};
}

static void happy_and_lost_ack(void)
{
    fake_t fake = {.replies={PET_ENROLL_REPLY_NOT_CLAIMED,
        PET_ENROLL_REPLY_RETRY, PET_ENROLL_REPLY_COMPLETE, PET_ENROLL_REPLY_COMPLETE}};
    pet_enrollment_io_t callbacks = io(&fake);
    pet_enrollment_t state;
    assert(pet_enrollment_open(&state, &callbacks) == PET_ENROLL_RETRY);
    assert(fake.random_calls == 2 && fake.writes == 2 && fake.requests == 0);
    char id[37], credential[44];
    assert(pet_enrollment_identity(&state, id, credential));
    assert(!strcmp(id, "pet-000102030405060708090a0b0c0d0e0f"));
    assert(!strcmp(credential, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"));
    assert(pet_enrollment_submit(&state, "A2B3") == PET_ENROLL_RETRY);
    assert(fake.requests == 0);
    assert(pet_enrollment_resume(&state) == PET_ENROLL_NEEDS_CODE);
    assert(pet_enrollment_submit(&state, "A2B3") == PET_ENROLL_RETRY);
    assert(pet_enrollment_submit(&state, "A2B3") == PET_ENROLL_RETRY);
    assert(fake.redeems == 1);
    /* Lost redeem ACK -> reboot -> completion only, no fresh ID or code. */
    assert(pet_enrollment_open(&state, &callbacks) == PET_ENROLL_RETRY);
    assert(pet_enrollment_resume(&state) == PET_ENROLL_COMPLETE);
    assert(fake.random_calls == 2 && fake.writes == 3 && fake.redeems == 1);
    assert(pet_enrollment_open(&state, &callbacks) == PET_ENROLL_COMPLETE);
    assert(pet_enrollment_resume(&state) == PET_ENROLL_COMPLETE);
    assert(fake.writes == 3); /* no unnecessary NVS wear */
}

static void confirmation_and_rejection(void)
{
    fake_t fake = {.replies={PET_ENROLL_REPLY_NOT_CLAIMED,
        PET_ENROLL_REPLY_CONFIRM_REQUIRED, PET_ENROLL_REPLY_RETRY,
        PET_ENROLL_REPLY_COMPLETE, PET_ENROLL_REPLY_REJECTED, PET_ENROLL_REPLY_NOT_CLAIMED}};
    pet_enrollment_io_t callbacks = io(&fake);
    pet_enrollment_t state;
    pet_enrollment_open(&state, &callbacks);
    assert(pet_enrollment_resume(&state) == PET_ENROLL_NEEDS_CODE);
    assert(pet_enrollment_submit(&state, "A2B3") == PET_ENROLL_RETRY);
    assert(fake.requests == 3);
    pet_enrollment_open(&state, &callbacks);
    assert(pet_enrollment_resume(&state) == PET_ENROLL_COMPLETE);
    assert(pet_enrollment_resume(&state) == PET_ENROLL_REJECTED);
    assert(pet_enrollment_resume(&state) == PET_ENROLL_REJECTED);
    assert(pet_enrollment_submit(&state, "A2B3") == PET_ENROLL_RETRY);
    assert(fake.random_calls == 2); /* revoked never rekeys */
}

static void write_failures(void)
{
    for (unsigned fail = 1; fail <= 3; ++fail) {
        for (unsigned ambiguous = 0; ambiguous <= 1; ++ambiguous) {
            fake_t fake = {.fail_write=fail, .commit_then_fail=ambiguous,
                .replies={PET_ENROLL_REPLY_COMPLETE, PET_ENROLL_REPLY_COMPLETE}};
            pet_enrollment_io_t callbacks = io(&fake);
            pet_enrollment_t state;
            pet_enrollment_result_t opened = pet_enrollment_open(&state, &callbacks);
            if (fail < 3) assert(opened == PET_ENROLL_STORAGE_ERROR);
            else assert(pet_enrollment_resume(&state) == PET_ENROLL_STORAGE_ERROR);
            assert(!state.loaded);
            assert(pet_enrollment_resume(&state) == PET_ENROLL_STORAGE_ERROR);
            assert(fake.requests == (fail == 3 ? 1u : 0u));
            fake.fail_write = 0;
            unsigned prior_random = fake.random_calls;
            opened = pet_enrollment_open(&state, &callbacks);
            assert(opened == PET_ENROLL_RETRY || opened == PET_ENROLL_COMPLETE);
            if (fail != 1 || ambiguous) assert(fake.random_calls == prior_random);
            assert(pet_enrollment_resume(&state) == PET_ENROLL_COMPLETE);
        }
    }
}

static void corruption(void)
{
    fake_t original = {0};
    pet_enrollment_io_t callbacks = io(&original);
    pet_enrollment_t state;
    pet_enrollment_open(&state, &callbacks);
    /* Every byte, including schema/generation/CRC, torn in either copy. */
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned byte = 0; byte < PET_ENROLLMENT_RECORD_BYTES; ++byte) {
            fake_t fake = original;
            fake.records[slot][byte] ^= 0x40;
            callbacks = io(&fake);
            assert(pet_enrollment_open(&state, &callbacks) == PET_ENROLL_RETRY);
            assert(fake.random_calls == 2);
        }
    }
    original.records[0][20] ^= 1;
    original.records[1][21] ^= 1;
    callbacks = io(&original);
    assert(pet_enrollment_open(&state, &callbacks) == PET_ENROLL_STORAGE_ERROR);
    assert(original.random_calls == 2 && original.requests == 0);
    original.read_error = 1;
    assert(pet_enrollment_open(&state, &callbacks) == PET_ENROLL_STORAGE_ERROR);
}

static void invalid_codes(void)
{
    assert(!pet_enrollment_code_valid(NULL));
    assert(!pet_enrollment_code_valid(""));
    assert(!pet_enrollment_code_valid("A2B"));
    assert(!pet_enrollment_code_valid("A2B34"));
    assert(!pet_enrollment_code_valid("ABCD2345")); /* never truncate old codes */
    assert(!pet_enrollment_code_valid("a2b3"));
    assert(!pet_enrollment_code_valid("ABC1"));
    assert(!pet_enrollment_code_valid("ABCI"));
    assert(!pet_enrollment_code_valid("ABCO"));
    assert(!pet_enrollment_code_valid("ABC0"));
    assert(pet_enrollment_code_valid("A2B3"));
    assert(pet_enrollment_code_valid("ABCD"));
    assert(pet_enrollment_code_valid("2345"));
}

int main(void)
{
    happy_and_lost_ack(); confirmation_and_rejection(); write_failures();
    corruption(); invalid_codes();
    puts("enrollment: durable identity, lost ACK, retries, corruption and code tests passed");
    return 0;
}
