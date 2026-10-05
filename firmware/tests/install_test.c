#include "pet_install.h"
#include "pet_journal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *request_id="00000000-0000-4000-8000-000000000001";
static const char *build_id="00000000-0000-4000-8000-000000000002";
static const char *installation_id="00000000-0000-4000-8000-000000000003";
static const char *relationship_id="00000000-0000-4000-8000-000000000004";
static const char *sha="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static uint8_t sectors[PET_JOURNAL_SECTORS][PET_JOURNAL_SECTOR_BYTES];
static bool fail_write, commit_then_fail;
static unsigned erased_sector;

static bool read_sector(void *context, unsigned sector, uint8_t *bytes)
{ (void)context; memcpy(bytes,sectors[sector],PET_JOURNAL_SECTOR_BYTES); return true; }
static bool replace_sector(void *context, unsigned sector, const uint8_t *bytes)
{
    (void)context;
    erased_sector=sector;
    memset(sectors[sector],0xff,PET_JOURNAL_SECTOR_BYTES);
    if(!fail_write||commit_then_fail) memcpy(sectors[sector],bytes,PET_JOURNAL_SECTOR_BYTES);
    else memcpy(sectors[sector],bytes,101); /* power loss after partial programming */
    return !fail_write;
}

static void journal_cases(void)
{
    memset(sectors,0xff,sizeof(sectors));
    pet_journal_io_t io={.read=read_sector,.replace=replace_sector};
    pet_journal_t journal;
    char payload[64];size_t length;
    assert(pet_journal_open(&journal,&io,payload,sizeof(payload),&length)==PET_JOURNAL_EMPTY);
    assert(pet_journal_save(&journal,"first",6)==PET_JOURNAL_OK);
    assert(pet_journal_save(&journal,"second",7)==PET_JOURNAL_OK);
    unsigned previous=journal.newest_sector;
    fail_write=true;
    assert(pet_journal_save(&journal,"third",6)==PET_JOURNAL_IO);
    assert(erased_sector!=previous && !journal.loaded);
    assert(pet_journal_open(&journal,&io,payload,sizeof(payload),&length)==PET_JOURNAL_OK);
    assert(!strcmp(payload,"second"));
    commit_then_fail=true;
    assert(pet_journal_save(&journal,"third",6)==PET_JOURNAL_IO);
    assert(pet_journal_open(&journal,&io,payload,sizeof(payload),&length)==PET_JOURNAL_OK);
    assert(!strcmp(payload,"third"));
    fail_write=commit_then_fail=false;
    for(unsigned i=0;i<100;++i) {
        previous=journal.newest_sector;
        assert(pet_journal_save(&journal,"wrapped",8)==PET_JOURNAL_OK);
        assert(erased_sector!=previous);
    }
    assert(pet_journal_open(&journal,&io,payload,sizeof(payload),&length)==PET_JOURNAL_OK);
    assert(journal.generation==103 && !strcmp(payload,"wrapped"));
    sectors[journal.newest_sector][34]^=1;
    assert(pet_journal_open(&journal,&io,payload,sizeof(payload),&length)==PET_JOURNAL_OK);
    assert(journal.generation==102);
    memset(sectors,0,sizeof(sectors));
    assert(pet_journal_open(&journal,&io,payload,sizeof(payload),&length)==PET_JOURNAL_CORRUPT);
    assert(!journal.loaded);
}

static void install_cases(void)
{
    pet_install_t state;
    pet_install_empty(&state);
    assert(pet_install_valid(&state));
    assert(!pet_install_binding_matches(&state,"r1",relationship_id,build_id,sha,"1"));
    assert(!pet_install_request(&state,request_id,build_id,sha,PET_INSTALL_PACK_MAX+1,"r0"));
    assert(pet_install_request(&state,request_id,build_id,sha,0x20001,"r0"));
    assert(state.candidate_slot==0 && state.active_slot==-1);
    assert(!pet_install_request(&state,request_id,build_id,sha,0x20001,"r0"));
    assert(!pet_install_attach(&state,installation_id,relationship_id,sha,0x20001));
    assert(pet_install_attach(&state,installation_id,build_id,sha,0x20001));
    assert(!pet_install_progress(&state,10));
    assert(pet_install_progress(&state,0x10000));
    assert(!pet_install_progress(&state,0));
    assert(!pet_install_verified(&state));
    assert(pet_install_progress(&state,0x20001));
    assert(pet_install_verified(&state));
    assert(pet_install_activate(&state));
    /* Reboot is a copy of persisted state, not a transition or implicit rollback. */
    pet_install_t rebooted=state;
    assert(pet_install_valid(&rebooted));
    assert(!pet_install_cancel(&rebooted));
    assert(!pet_install_commit(&rebooted,request_id,build_id,sha,"r1",relationship_id,"1"));
    assert(!pet_install_commit(&rebooted,installation_id,relationship_id,sha,"r1",relationship_id,"1"));
    assert(pet_install_commit(&rebooted,installation_id,build_id,sha,"r1",relationship_id,"1"));
    assert(pet_install_binding_matches(&rebooted,"r1",relationship_id,build_id,sha,"1"));
    assert(!pet_install_binding_matches(&rebooted,"r0",relationship_id,build_id,sha,"1"));
    assert(!pet_install_binding_matches(&rebooted,"r1",relationship_id,build_id,sha,"2"));
    assert(pet_install_request(&rebooted,request_id,build_id,sha,0x20001,"r1"));
    assert(rebooted.candidate_slot==1 && rebooted.active_slot==0);
    assert(pet_install_binding_matches(&rebooted,"r1",relationship_id,build_id,sha,"1"));
    assert(pet_install_cancel(&rebooted));
    assert(rebooted.slots[0].state==PET_SLOT_ACTIVE && rebooted.slots[1].state==PET_SLOT_EMPTY);
    rebooted.active_slot=2;
    assert(!pet_install_valid(&rebooted));
}

int main(void)
{
    journal_cases();install_cases();
    puts("install journal: interrupted writes, ring wrap, progress, binding and ambiguous activation passed");
    return 0;
}
