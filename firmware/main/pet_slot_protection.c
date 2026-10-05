#include "pet_slot_protection.h"
#include <string.h>
#include "pet_flash_layout_esp.h"

_Static_assert(PET_SLOT_COUNT<=PET_FIRMWARE_INSTALLED_MAX,"every slot fits the protection");

bool pet_slot_protection(pet_slot_store_t *store,void *record,const char *account,
                         const pet_pack_trust_key_t *trust,size_t count,pet_firmware_protection_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));
    pet_flash_layout_t physical;char hash[65];
    if(!store||!store->open||!store->inventory.ready||!record||!account||!pet_slot_inventory_valid(&store->inventory.state)||
       pet_flash_layout_read(&physical,hash)!=ESP_OK||physical.id!=store->layout.id||strcmp(hash,store->partition_sha256))return false;
    const pet_slot_inventory_t *inventory=&store->inventory.state;
    /* A sole record cannot prove both the old and the new pet of the target
     * before invalidation, as on a single-pet device. */
    const pet_replace_phase_t phase=inventory->operation.phase;
    if(phase==PET_REPLACE_REQUESTED||phase==PET_REPLACE_FENCED)return false;
    const int shown=inventory->active>=0?inventory->active:inventory->bound;
    for(unsigned slot=0;slot<PET_SLOT_COUNT;++slot){
        const pet_slot_t *s=&inventory->slots[slot];
        if(s->state==PET_SLOT_FREE)continue;
        /* A valid inventory installs only into its target, after invalidation. */
        const bool writing=s->state==PET_SLOT_INSTALLING;
        pet_firmware_pack_identity_t *into=writing?&out->interrupted:(int)slot==shown?&out->active:
            &out->installed[out->installed_count++];
        pet_release_v2_t release;
        if(pet_slot_store_read_record(store,slot,record,PET_REPLACE_MANIFEST_BYTES)!=ESP_OK||
           !pet_release_v2_record_verify(record,PET_REPLACE_MANIFEST_BYTES,account,writing?&inventory->operation.target:&s->pack,
                trust,count,&release)||
           release.requirements.layout_id!=store->layout.id||strcmp(release.requirements.partition_sha256,store->partition_sha256)){
            memset(out,0,sizeof(*out));return false;
        }
        into->requirements=release.requirements;
        strcpy(into->build_id,release.pack.build_id);strcpy(into->sha256,release.pack.sha256);
    }
    return out->known=true;
}
