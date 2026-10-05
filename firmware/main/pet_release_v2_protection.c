#include "pet_release_v2_protection.h"
#include <string.h>

bool pet_release_v2_protection(const pet_replace_journal_t *journal,
                               const pet_flash_layout_t *physical,const char *partition_sha256,
                               const void *record,size_t record_bytes,const char *account,
                               const pet_pack_trust_key_t *trust,size_t count,
                               pet_firmware_pack_identity_t *active,pet_firmware_pack_identity_t *interrupted)
{
    if(!active||!interrupted||active==interrupted)return false;
    memset(active,0,sizeof(*active));memset(interrupted,0,sizeof(*interrupted));
    if(!journal||!journal->ready||!journal->journal.loaded||!pet_replace_valid(&journal->state)||
       !physical||physical->pet_slots!=1||!partition_sha256||!memchr(partition_sha256,0,65)||strlen(partition_sha256)!=64||
       strspn(partition_sha256,"0123456789abcdef")!=64)return false;
    pet_flash_layout_t known;
    if(!pet_flash_layout_known(physical->id,&known)||known.pet_slots!=1||
       physical->app_slot_bytes!=known.app_slot_bytes||physical->pet_partition_bytes!=known.pet_partition_bytes||
       physical->pack_capacity_bytes!=known.pack_capacity_bytes||physical->journal_offset!=known.journal_offset||
       physical->pet_offset!=known.pet_offset||journal->state.capacity_bytes!=known.pack_capacity_bytes)return false;
    const pet_replace_t *state=&journal->state;
    if(state->phase==PET_REPLACE_EMPTY)return true;
    if(state->phase==PET_REPLACE_REQUESTED||state->phase==PET_REPLACE_FENCED)return false;
    bool is_active=state->phase==PET_REPLACE_ACTIVE;
    const pet_replace_pack_t *expected=is_active?&state->active:&state->target;
    pet_release_v2_t release;
    if(!pet_release_v2_record_verify(record,record_bytes,account,expected,trust,count,&release)||
       release.requirements.layout_id!=known.id||strcmp(release.requirements.partition_sha256,partition_sha256))return false;
    pet_firmware_pack_identity_t *out=is_active?active:interrupted;
    out->requirements=release.requirements;
    strcpy(out->build_id,release.pack.build_id);strcpy(out->sha256,release.pack.sha256);return true;
}
