#include "pet_enrollment.h"

#include <string.h>

void pet_enrollment_clear(void *data, size_t bytes)
{
    volatile uint8_t *p = data;
    while (bytes--) *p++ = 0;
}

static uint32_t checksum(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void encode(const pet_enrollment_t *state, uint8_t *record)
{
    memset(record, 0, PET_ENROLLMENT_RECORD_BYTES);
    memcpy(record, "PEN1", 4);
    record[4] = 1;
    record[5] = state->complete ? 2 : 1;
    for (unsigned i = 0; i < 8; ++i)
        record[8 + i] = (uint8_t)(state->generation >> (8 * i));
    memcpy(record + 16, state->identity, 16);
    memcpy(record + 32, state->secret, 32);
    uint32_t crc = checksum(record, 68);
    for (unsigned i = 0; i < 4; ++i) record[68 + i] = (uint8_t)(crc >> (8 * i));
}

static bool decode(const uint8_t *record, pet_enrollment_t *state)
{
    if (memcmp(record, "PEN1", 4) || record[4] != 1 ||
        (record[5] != 1 && record[5] != 2) || record[6] || record[7] ||
        record[64] || record[65] || record[66] || record[67]) return false;
    uint32_t crc = 0;
    for (unsigned i = 0; i < 4; ++i) crc |= (uint32_t)record[68 + i] << (8 * i);
    if (crc != checksum(record, 68)) return false;
    state->generation = 0;
    for (unsigned i = 0; i < 8; ++i)
        state->generation |= (uint64_t)record[8 + i] << (8 * i);
    if (!state->generation) return false;
    state->complete = record[5] == 2;
    memcpy(state->identity, record + 16, 16);
    memcpy(state->secret, record + 32, 32);
    return true;
}

static bool persist(pet_enrollment_t *state)
{
    if (state->generation == UINT64_MAX) {
        state->loaded = false;
        return false;
    }
    ++state->generation;
    uint8_t record[PET_ENROLLMENT_RECORD_BYTES];
    encode(state, record);
    bool ok = state->io.write(state->io.context, (unsigned)(state->generation & 1), record);
    pet_enrollment_clear(record, sizeof(record));
    /* A failed commit can have reached flash. Never send requests or create a
     * new identity from this in-memory instance until both records are reread. */
    if (!ok) state->loaded = false;
    return ok;
}

pet_enrollment_result_t pet_enrollment_open(pet_enrollment_t *state,
                                           const pet_enrollment_io_t *io)
{
    if (!state || !io || !io->read || !io->write || !io->random || !io->request)
        return PET_ENROLL_STORAGE_ERROR;
    pet_enrollment_io_t callbacks = *io;
    pet_enrollment_clear(state, sizeof(*state));
    state->io = callbacks;
    pet_enrollment_t candidates[2] = {0};
    uint8_t records[2][PET_ENROLLMENT_RECORD_BYTES] = {{0}};
    int found[2];
    bool valid[2];
    for (unsigned i = 0; i < 2; ++i) {
        found[i] = callbacks.read(callbacks.context, i, records[i]);
        valid[i] = found[i] == 1 && decode(records[i], &candidates[i]);
    }
    bool conflict = valid[0] && valid[1] &&
        (memcmp(candidates[0].identity, candidates[1].identity, 16) ||
         memcmp(candidates[0].secret, candidates[1].secret, 32) ||
         (candidates[0].generation == candidates[1].generation &&
          candidates[0].complete != candidates[1].complete));
    pet_enrollment_result_t result = PET_ENROLL_STORAGE_ERROR;
    /* An unreadable key might hold a newer schema: fail closed on I/O errors.
     * A readable torn/corrupt record may fall back to the other valid copy. */
    if (found[0] < 0 || found[1] < 0 || conflict) goto done;
    if (valid[0] || valid[1]) {
        unsigned chosen = !valid[0] || (valid[1] &&
            candidates[1].generation > candidates[0].generation) ? 1 : 0;
        state->generation = candidates[chosen].generation;
        state->complete = candidates[chosen].complete;
        memcpy(state->identity, candidates[chosen].identity, 16);
        memcpy(state->secret, candidates[chosen].secret, 32);
        state->loaded = true;
        if ((!valid[0] || !valid[1]) && !persist(state)) goto done;
    } else {
        if (found[0] || found[1]) goto done;
        if (!callbacks.random(callbacks.context, state->identity, 16) ||
            !callbacks.random(callbacks.context, state->secret, 32)) goto done;
        state->loaded = true;
        /* Two committed copies before the first network side effect. */
        if (!persist(state) || !persist(state)) goto done;
    }
    result = state->complete ? PET_ENROLL_COMPLETE : PET_ENROLL_RETRY;
done:
    pet_enrollment_clear(records, sizeof(records));
    pet_enrollment_clear(candidates, sizeof(candidates));
    return result;
}

bool pet_enrollment_identity(const pet_enrollment_t *state,
                             char device_id[PET_ENROLLMENT_DEVICE_ID_BYTES],
                             char credential[PET_ENROLLMENT_CREDENTIAL_BYTES])
{
    if (!state || !state->loaded || !device_id || !credential) return false;
    static const char hex[] = "0123456789abcdef";
    static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    memcpy(device_id, "pet-", 4);
    for (unsigned i = 0; i < 16; ++i) {
        device_id[4 + 2 * i] = hex[state->identity[i] >> 4];
        device_id[5 + 2 * i] = hex[state->identity[i] & 15];
    }
    device_id[36] = 0;
    unsigned output = 0;
    uint32_t bits = 0;
    unsigned count = 0;
    for (unsigned i = 0; i < 32; ++i) {
        bits = (bits << 8) | state->secret[i];
        count += 8;
        while (count >= 6) {
            count -= 6;
            credential[output++] = base64[(bits >> count) & 63];
        }
    }
    if (count) credential[output++] = base64[(bits << (6 - count)) & 63];
    credential[output] = 0;
    return true;
}

bool pet_enrollment_code_valid(const char *code)
{
    if (!code) return false;
    for (unsigned i = 0; i < PET_ENROLLMENT_CODE_LENGTH; ++i)
        if (!code[i] || !strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789", code[i])) return false;
    return code[PET_ENROLLMENT_CODE_LENGTH] == 0;
}

static pet_enrollment_reply_t request(pet_enrollment_t *state, const char *code)
{
    char id[PET_ENROLLMENT_DEVICE_ID_BYTES];
    char credential[PET_ENROLLMENT_CREDENTIAL_BYTES];
    if (!pet_enrollment_identity(state, id, credential)) return PET_ENROLL_REPLY_RETRY;
    pet_enrollment_reply_t reply = state->io.request(state->io.context, code != NULL,
                                                     id, credential, code);
    pet_enrollment_clear(credential, sizeof(credential));
    return reply;
}

static pet_enrollment_result_t completed(pet_enrollment_t *state)
{
    if (!state->complete) {
        state->complete = true;
        if (!persist(state)) return PET_ENROLL_STORAGE_ERROR;
    }
    return PET_ENROLL_COMPLETE;
}

pet_enrollment_result_t pet_enrollment_resume(pet_enrollment_t *state)
{
    if (!state || !state->loaded) return PET_ENROLL_STORAGE_ERROR;
    state->may_redeem = false;
    switch (request(state, NULL)) {
        case PET_ENROLL_REPLY_COMPLETE: return completed(state);
        case PET_ENROLL_REPLY_NOT_CLAIMED:
            if (state->complete) return PET_ENROLL_REJECTED;
            state->may_redeem = true;
            return PET_ENROLL_NEEDS_CODE;
        case PET_ENROLL_REPLY_REJECTED: return PET_ENROLL_REJECTED;
        default: return PET_ENROLL_RETRY;
    }
}

pet_enrollment_result_t pet_enrollment_submit(pet_enrollment_t *state, const char *code)
{
    if (!state || !state->loaded) return PET_ENROLL_STORAGE_ERROR;
    if (!pet_enrollment_code_valid(code)) return PET_ENROLL_INVALID_CODE;
    if (!state->may_redeem) return PET_ENROLL_RETRY;
    state->may_redeem = false;
    switch (request(state, code)) {
        case PET_ENROLL_REPLY_COMPLETE: return completed(state);
        case PET_ENROLL_REPLY_CONFIRM_REQUIRED: return pet_enrollment_resume(state);
        case PET_ENROLL_REPLY_REJECTED:
            /* Invalid/expired codes can be retried, but only after completion
             * proves this identity was not claimed in the meantime. */
            return PET_ENROLL_REJECTED;
        default: return PET_ENROLL_RETRY;
    }
}
