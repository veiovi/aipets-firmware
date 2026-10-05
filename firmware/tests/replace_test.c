#include "pet_replace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *request = "00000000-0000-4000-8000-000000000001";
static const char *operation = "00000000-0000-4000-8000-000000000002";
static const char *fence = "00000000-0000-4000-8000-000000000003";
static const char *relationship = "00000000-0000-4000-8000-000000000004";
static const char *sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *prefix = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static pet_replace_pack_t pack;

static void valid(const pet_replace_t *s) { assert(pet_replace_valid(s)); }
static pet_replace_t requested(void)
{
    pet_replace_t s;
    assert(pet_replace_empty(&s, PET_REPLACE_MAX_BYTES));
    assert(pet_replace_request(&s, request, &pack, "r0")); valid(&s);
    assert(!pet_replace_has_active(&s)); assert(!pet_replace_can_erase(&s)); return s;
}
static pet_replace_t downloading(void)
{
    pet_replace_t s = requested();
    assert(pet_replace_fenced(&s, operation, fence, &pack)); valid(&s);
    assert(!pet_replace_can_erase(&s));
    assert(pet_replace_invalidate(&s)); valid(&s); assert(!pet_replace_can_erase(&s));
    assert(pet_replace_begin_download(&s)); valid(&s); assert(pet_replace_can_erase(&s)); return s;
}
static pet_replace_t activating(void)
{
    pet_replace_t s = downloading();
    assert(pet_replace_progress(&s, 0x10000, prefix)); valid(&s);
    assert(pet_replace_progress(&s, pack.bytes, sha)); valid(&s);
    assert(pet_replace_verified(&s, sha)); valid(&s);
    assert(!pet_replace_can_erase(&s)); assert(pet_replace_activate(&s)); valid(&s); return s;
}
static pet_replace_t active(void)
{
    pet_replace_t s = activating();
    assert(pet_replace_commit(&s, operation, fence, &pack, "r1", relationship, "1"));
    valid(&s); assert(pet_replace_has_active(&s)); assert(!pet_replace_can_erase(&s)); return s;
}
static void transitions(void)
{
    pet_replace_t s = requested(), before = s;
    assert(!pet_replace_begin_download(&s)); assert(!pet_replace_invalidate(&s));
    assert(!pet_replace_verified(&s, sha)); assert(!pet_replace_activate(&s));
    assert(!pet_replace_progress(&s, 0x10000, prefix));
    assert(!pet_replace_request(&s, request, &pack, "r0"));
    assert(!memcmp(&s, &before, sizeof(s)));
    pet_replace_pack_t wrong = pack; wrong.bytes++;
    assert(!pet_replace_fenced(&s, operation, fence, &wrong));
    assert(!memcmp(&s, &before, sizeof(s)));
    s = downloading(); before = s;
    assert(!pet_replace_progress(&s, 0, prefix));
    assert(!pet_replace_progress(&s, 1, prefix));
    assert(!pet_replace_progress(&s, pack.bytes, sha)); /* Cannot skip a checkpoint. */
    assert(!pet_replace_progress(&s, 0x10000, "bad"));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(pet_replace_resume_matches(&s, &pack, 0, ""));
    assert(!pet_replace_resume_matches(&s, &wrong, 0, ""));
    assert(pet_replace_progress(&s, 0x10000, prefix));
    assert(!pet_replace_verified(&s, sha));
    assert(!pet_replace_resume_matches(&s, &pack, 0x10000, sha));
    assert(!pet_replace_resume_matches(&s, &pack, 0, prefix));
    assert(pet_replace_resume_matches(&s, &pack, 0x10000, prefix));
    assert(!pet_replace_restart(&s, &wrong));
    assert(pet_replace_restart(&s, &pack)); valid(&s);
    assert(s.downloaded_bytes == 0 && s.phase == PET_REPLACE_INVALIDATED);
    assert(!strcmp(s.fence_id, fence)); assert(!pet_replace_can_erase(&s));
    assert(pet_replace_begin_download(&s));
    assert(pet_replace_progress(&s, 0x10000, prefix));
    assert(pet_replace_progress(&s, pack.bytes, prefix));
    assert(!pet_replace_verified(&s, sha)); /* Final read-back hash mismatch. */
    assert(pet_replace_restart(&s, &pack));
    s = activating(); before = s;
    assert(!pet_replace_cancel(&s));
    assert(!pet_replace_commit(&s, request, fence, &pack, "r1", relationship, "1"));
    assert(!pet_replace_commit(&s, operation, request, &pack, "r1", relationship, "1"));
    assert(!pet_replace_commit(&s, operation, fence, &wrong, "r1", relationship, "1"));
    assert(!pet_replace_commit(&s, operation, fence, &pack, "", relationship, "1"));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(pet_replace_commit(&s, operation, fence, &pack, "r1", relationship, "1")); valid(&s);
    assert(s.phase == PET_REPLACE_ACTIVE && !s.fence_id[0] && !s.target.bytes);
}
static void replacement_and_cancellation(void)
{
    pet_replace_t s = active();
    assert(pet_replace_request(&s, request, &pack, "r1"));
    assert(pet_replace_has_active(&s)); assert(!pet_replace_can_erase(&s));
    pet_replace_t cancel = s;
    assert(pet_replace_cancel(&cancel)); valid(&cancel); assert(pet_replace_has_active(&cancel));
    assert(pet_replace_fenced(&s, operation, fence, &pack)); valid(&s);
    assert(!pet_replace_has_active(&s));
    for (int phase = PET_REPLACE_FENCED; phase <= PET_REPLACE_VERIFIED; ++phase) {
        assert(s.phase == (pet_replace_phase_t)phase); valid(&s);
        cancel = s; assert(pet_replace_cancel(&cancel)); valid(&cancel);
        assert(cancel.phase == PET_REPLACE_RECOVERY && !pet_replace_has_active(&cancel));
        assert(!cancel.active.bytes && !strcmp(cancel.fence_id, fence));
        assert(!pet_replace_can_erase(&cancel));
        pet_replace_t reboot = cancel; valid(&reboot); /* Never resurrect old pack. */
        assert(!pet_replace_request(&reboot, request, &reboot.target, "r1")); valid(&reboot);
        assert(!memcmp(&reboot, &cancel, sizeof(reboot)));
        assert(!pet_replace_retry(&reboot, request, fence, &pack));
        assert(pet_replace_retry(&reboot, operation, fence, &pack)); valid(&reboot);
        assert(reboot.phase == PET_REPLACE_INVALIDATED && reboot.downloaded_bytes == 0);
        assert(!pet_replace_has_active(&reboot)); assert(!pet_replace_can_erase(&reboot));
        pet_replace_t next = requested();
        assert(pet_replace_fenced(&next, relationship, request, &pack));
        assert(!pet_replace_supersede(&cancel, request, fence, &next));
        assert(pet_replace_supersede(&cancel, operation, fence, &next)); valid(&cancel);
        assert(!pet_replace_can_erase(&cancel) && cancel.phase == PET_REPLACE_FENCED);
        if (phase == PET_REPLACE_FENCED) assert(pet_replace_invalidate(&s));
        else if (phase == PET_REPLACE_INVALIDATED) assert(pet_replace_begin_download(&s));
        else if (phase == PET_REPLACE_DOWNLOADING) {
            assert(pet_replace_progress(&s, 0x10000, prefix));
            assert(pet_replace_progress(&s, pack.bytes, sha)); assert(pet_replace_verified(&s, sha));
        }
    }
}
static void malformed_and_boundaries(void)
{
    pet_replace_t s, before;
    const uint32_t capacities[] = {0xbbe000, 0xabe000, 0x9be000, 0x7be000};
    for (unsigned i = 0; i < 4; ++i) {
        assert(pet_replace_empty(&s, capacities[i])); valid(&s);
        pet_replace_pack_t limit = pack; limit.bytes = capacities[i];
        assert(pet_replace_request(&s, request, &limit, "r0"));
        assert(pet_replace_cancel(&s)); limit.bytes++;
        assert(!pet_replace_request(&s, request, &limit, "r0"));
        assert(!pet_replace_empty(&s, capacities[i] + 1));
        assert(!pet_replace_empty(&s, capacities[i] - 1));
    }
    before = activating();
    s = before; s.downloaded_bytes--; assert(!pet_replace_valid(&s));
    s = before; s.prefix_sha256[0] = 'b'; assert(!pet_replace_valid(&s));
    s = before; s.fence_id[0] = 0; assert(!pet_replace_valid(&s));
    s = before; s.capacity_bytes = UINT32_MAX; assert(!pet_replace_valid(&s));
    s = before; memset(s.target.sha256, 'a', sizeof(s.target.sha256)); assert(!pet_replace_valid(&s));
    s = before; s.phase = PET_REPLACE_ACTIVE; assert(!pet_replace_valid(&s));
    s = active(); s.phase = PET_REPLACE_DOWNLOADING; assert(!pet_replace_valid(&s));
    s = active(); s.target.bytes = 1; assert(!pet_replace_valid(&s));
    s = active(); memset(s.config_version, 'x', sizeof(s.config_version)); assert(!pet_replace_valid(&s));
    assert(!pet_replace_valid(NULL));
}
/* Three-pet devices move the conversation between installed pets, between
 * installations only; the result is an ordinary ACTIVE state. */
static void rebinding(void)
{
    pet_replace_pack_t other = pack; other.build_id[35] = '9'; memset(other.sha256, 'c', 64); other.bytes = 0x20000;
    pet_replace_t s; assert(pet_replace_empty(&s, PET_REPLACE_MAX_BYTES));
    assert(pet_replace_rebind(&s, &other, "r1", relationship, "c1")); valid(&s);
    assert(s.phase == PET_REPLACE_ACTIVE && pet_replace_has_active(&s) && !strcmp(s.active.build_id, other.build_id));
    s = active();
    assert(pet_replace_rebind(&s, &other, "r2", relationship, "c2")); valid(&s);
    assert(!strcmp(s.binding_revision, "r2") && !s.request_id[0] && !s.operation_id[0] && !s.target.bytes);
    pet_replace_t during = requested();
    assert(!pet_replace_rebind(&during, &other, "r3", relationship, "c3"));
    during = downloading(); assert(!pet_replace_rebind(&during, &other, "r3", relationship, "c3"));
    s = active(); pet_replace_pack_t bad = other; bad.bytes = PET_REPLACE_MAX_BYTES + 1;
    assert(!pet_replace_rebind(&s, &bad, "r3", relationship, "c3"));
    assert(!pet_replace_rebind(&s, &other, "", relationship, "c3"));
    assert(!pet_replace_rebind(&s, &other, "r3", "not-a-uuid", "c3"));
    assert(!pet_replace_rebind(&s, NULL, "r3", relationship, "c3"));
}
int main(void)
{
    strcpy(pack.build_id, request); strcpy(pack.sha256, sha); pack.bytes = 0x10003;
    transitions(); replacement_and_cancellation(); malformed_and_boundaries(); rebinding();
    puts("single-slot state: fence/invalidate ordering, bounded checkpoints, exact resume, lost ACK, recovery and rebinding passed");
    return 0;
}
