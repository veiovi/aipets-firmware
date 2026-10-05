#include "pet_asset_store.h"

#include <stdlib.h>
#include <string.h>

static bool journal_read(void *context,unsigned sector,uint8_t *record)
{
    pet_asset_store_t *store=context;
    return sector<PET_JOURNAL_SECTORS&&esp_partition_read(store->journal_partition,
        sector*PET_JOURNAL_SECTOR_BYTES,record,PET_JOURNAL_SECTOR_BYTES)==ESP_OK;
}
static bool journal_replace(void *context,unsigned sector,const uint8_t *record)
{
    pet_asset_store_t *store=context;
    if(sector>=PET_JOURNAL_SECTORS)return false;
    size_t offset=sector*PET_JOURNAL_SECTOR_BYTES;
    if(esp_partition_erase_range(store->journal_partition,offset,PET_JOURNAL_SECTOR_BYTES)!=ESP_OK||
       esp_partition_write(store->journal_partition,offset,record,PET_JOURNAL_SECTOR_BYTES)!=ESP_OK)return false;
    uint8_t readback[256];
    for(size_t n=0;n<PET_JOURNAL_SECTOR_BYTES;n+=sizeof(readback))
        if(esp_partition_read(store->journal_partition,offset+n,readback,sizeof(readback))!=ESP_OK||
           memcmp(readback,record+n,sizeof(readback)))return false;
    return true;
}

esp_err_t pet_asset_store_open(pet_asset_store_t *store)
{
    if(!store)return ESP_ERR_INVALID_ARG;
    if(store->initialized)return ESP_ERR_INVALID_STATE;
    memset(store,0,sizeof(*store));
    store->journal_partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x41,"pet_journal");
    store->slots[0]=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x42,"pet_a");
    store->slots[1]=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x42,"pet_b");
    if(!store->journal_partition||store->journal_partition->address!=0x830000||
       store->journal_partition->size!=0x10000||!store->slots[0]||!store->slots[1]||
       store->slots[0]->address!=0x840000||store->slots[1]->address!=0xc20000||
       store->slots[0]->size!=PET_ASSET_SLOT_BYTES||store->slots[1]->size!=PET_ASSET_SLOT_BYTES)
        return ESP_ERR_INVALID_STATE;
    store->initialized=true;
    char *payload=malloc(PET_JOURNAL_PAYLOAD_MAX);if(!payload)return ESP_ERR_NO_MEM;
    pet_journal_io_t io={.read=journal_read,.replace=journal_replace,.context=store};size_t bytes=0;
    pet_journal_result_t result=pet_journal_open(&store->journal,&io,payload,PET_JOURNAL_PAYLOAD_MAX,&bytes);
    bool valid=result==PET_JOURNAL_OK&&pet_control_install_decode(payload,bytes,&store->state);
    free(payload);
    if(result==PET_JOURNAL_EMPTY) {
        pet_install_t initial;pet_install_empty(&initial);
        /* Redundant empty baseline before the first selection side effect. */
        if(pet_asset_store_commit(store,&initial)!=ESP_OK)return ESP_FAIL;
        return pet_asset_store_commit(store,&initial);
    }
    if(!valid){store->journal.loaded=false;return ESP_FAIL;}
    return ESP_OK;
}

esp_err_t pet_asset_store_commit(pet_asset_store_t *store,const pet_install_t *next)
{
    if(!store||!store->initialized||!next||!store->journal.loaded||!pet_install_valid(next))return ESP_ERR_INVALID_STATE;
    for(unsigned i=0;i<2;++i)if(store->mapped[i]&&
        (strcmp(store->state.slots[i].build_id,next->slots[i].build_id)||
         strcmp(store->state.slots[i].sha256,next->slots[i].sha256)||
         store->state.slots[i].bytes!=next->slots[i].bytes))return ESP_ERR_INVALID_STATE;
    char *payload=malloc(PET_JOURNAL_PAYLOAD_MAX);if(!payload)return ESP_ERR_NO_MEM;
    size_t bytes;bool encoded=pet_control_install_encode(next,payload,PET_JOURNAL_PAYLOAD_MAX,&bytes);
    pet_journal_result_t result=encoded?pet_journal_save(&store->journal,payload,bytes):PET_JOURNAL_ARGUMENT;
    free(payload);
    if(result!=PET_JOURNAL_OK)return ESP_FAIL;
    store->state=*next;return ESP_OK;
}

esp_err_t pet_asset_store_write_block(pet_asset_store_t *store,const void *data,size_t bytes)
{
    if(!store||!store->journal.loaded||!pet_install_valid(&store->state)||
       store->state.phase!=PET_INSTALL_DOWNLOADING||!data||!bytes||bytes>PET_INSTALL_CHECKPOINT_BYTES)
        return ESP_ERR_INVALID_STATE;
    unsigned slot=(unsigned)store->state.candidate_slot;
    uint32_t offset=store->state.downloaded_bytes,total=store->state.slots[slot].bytes;
    if(store->mapped[slot]||offset%PET_INSTALL_CHECKPOINT_BYTES||bytes>total-offset||
       (bytes!=PET_INSTALL_CHECKPOINT_BYTES&&offset+bytes!=total))return ESP_ERR_INVALID_ARG;
    size_t erase_bytes=(bytes+4095u)&~4095u;
    if(esp_partition_erase_range(store->slots[slot],offset,erase_bytes)!=ESP_OK||
       esp_partition_write(store->slots[slot],offset,data,bytes)!=ESP_OK)return ESP_FAIL;
    uint8_t readback[512];
    for(size_t at=0;at<bytes;at+=sizeof(readback)) {
        size_t n=bytes-at<sizeof(readback)?bytes-at:sizeof(readback);
        if(esp_partition_read(store->slots[slot],offset+at,readback,n)!=ESP_OK||
           memcmp(readback,(const uint8_t *)data+at,n))return ESP_FAIL;
    }
    pet_install_t next=store->state;
    if(!pet_install_progress(&next,offset+(uint32_t)bytes))return ESP_FAIL;
    return pet_asset_store_commit(store,&next);
}

esp_err_t pet_asset_store_save_manifest(pet_asset_store_t *store,const pet_control_operation_t *operation)
{
    if(!store||!operation||!store->journal.loaded||!pet_install_valid(&store->state)||
       store->state.phase!=PET_INSTALL_DOWNLOADING||
       !memchr(operation->signed_payload,0,sizeof(operation->signed_payload))||
       !memchr(operation->key_id,0,sizeof(operation->key_id))||
       !memchr(operation->signature,0,sizeof(operation->signature))||
       !memchr(operation->id,0,sizeof(operation->id))||
       !memchr(operation->build_id,0,sizeof(operation->build_id))||
       !memchr(operation->sha256,0,sizeof(operation->sha256)))return ESP_ERR_INVALID_STATE;
    unsigned slot=(unsigned)store->state.candidate_slot;
    if(store->mapped[slot]||strcmp(operation->id,store->state.installation_id)||
       strcmp(operation->build_id,store->state.slots[slot].build_id)||
       strcmp(operation->sha256,store->state.slots[slot].sha256)||
       operation->bytes!=store->state.slots[slot].bytes)return ESP_ERR_INVALID_STATE;
    uint8_t *record=calloc(1,PET_ASSET_MANIFEST_BYTES);if(!record)return ESP_ERR_NO_MEM;
    memcpy(record,"PMF1",4);
    memcpy(record+16,operation->key_id,strlen(operation->key_id)+1);
    memcpy(record+177,operation->signature,strlen(operation->signature)+1);
    memcpy(record+512,operation->signed_payload,strlen(operation->signed_payload)+1);
    esp_err_t err=esp_partition_erase_range(store->slots[slot],PET_ASSET_MANIFEST_OFFSET,PET_ASSET_MANIFEST_BYTES);
    if(err==ESP_OK)err=esp_partition_write(store->slots[slot],PET_ASSET_MANIFEST_OFFSET,record,PET_ASSET_MANIFEST_BYTES);
    uint8_t readback[256];
    for(size_t n=0;err==ESP_OK&&n<PET_ASSET_MANIFEST_BYTES;n+=sizeof(readback)) {
        err=esp_partition_read(store->slots[slot],PET_ASSET_MANIFEST_OFFSET+n,readback,sizeof(readback));
        if(err==ESP_OK&&memcmp(readback,record+n,sizeof(readback)))err=ESP_FAIL;
    }
    free(record);return err;
}

esp_err_t pet_asset_store_load_manifest(pet_asset_store_t *store,unsigned slot,pet_control_operation_t *operation)
{
    if(!store||!store->initialized||!store->journal.loaded||slot>1||!operation||store->state.slots[slot].state==PET_SLOT_EMPTY)return ESP_ERR_INVALID_ARG;
    uint8_t *record=malloc(PET_ASSET_MANIFEST_BYTES);if(!record)return ESP_ERR_NO_MEM;
    esp_err_t err=esp_partition_read(store->slots[slot],PET_ASSET_MANIFEST_OFFSET,record,PET_ASSET_MANIFEST_BYTES);
    if(err==ESP_OK&&(!memcmp(record,"PMF1",4))&&memchr(record+16,0,161)&&
       memchr(record+177,0,87)&&memchr(record+512,0,7001)) {
        memset(operation,0,sizeof(*operation));
        strcpy(operation->build_id,store->state.slots[slot].build_id);
        strcpy(operation->sha256,store->state.slots[slot].sha256);
        operation->bytes=store->state.slots[slot].bytes;
        strcpy(operation->key_id,(const char *)record+16);
        strcpy(operation->signature,(const char *)record+177);
        strcpy(operation->signed_payload,(const char *)record+512);
    }else err=ESP_FAIL;
    free(record);return err;
}

esp_err_t pet_asset_store_map(pet_asset_store_t *store,unsigned slot,const void **address,size_t *bytes)
{
    if(!store||!store->initialized||!store->journal.loaded||slot>1||!address||!bytes||store->state.slots[slot].state==PET_SLOT_EMPTY)
        return ESP_ERR_INVALID_ARG;
    if(!store->mapped[slot]) {
        esp_err_t err=esp_partition_mmap(store->slots[slot],0,store->state.slots[slot].bytes,
            ESP_PARTITION_MMAP_DATA,&store->map_addresses[slot],&store->map_handles[slot]);
        if(err!=ESP_OK)return err;
        store->mapped[slot]=true;
    }
    *address=store->map_addresses[slot];*bytes=store->state.slots[slot].bytes;return ESP_OK;
}

void pet_asset_store_unmap(pet_asset_store_t *store,unsigned slot)
{
    if(store&&slot<2&&store->mapped[slot]) {
        esp_partition_munmap(store->map_handles[slot]);store->mapped[slot]=false;
        store->map_addresses[slot]=NULL;
    }
}

void pet_asset_store_close(pet_asset_store_t *store)
{
    if(!store)return;
    pet_asset_store_unmap(store,0);pet_asset_store_unmap(store,1);
    memset(store,0,sizeof(*store));
}
