#include "pet_asset_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_partition_t parts[3]={{0x830000,0x10000,"pet_journal"},{0x840000,0x3e0000,"pet_a"},{0xc20000,0x3e0000,"pet_b"}};
static uint8_t memory[3][PET_ASSET_SLOT_BYTES];
static bool fail_journal,wrong_map;
static unsigned writes[3],erases[3];
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label)
{
    (void)type;(void)subtype;if(wrong_map)return NULL;
    for(unsigned i=0;i<3;++i)if(!strcmp(parts[i].label,label))return &parts[i];return NULL;
}
static unsigned index_of(const esp_partition_t *p,size_t offset,size_t length)
{
    assert(p>=parts&&p<parts+3);assert(offset<=p->size&&length<=p->size-offset);
    return (unsigned)(p-parts);
}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *bytes,size_t length)
{ unsigned i=index_of(p,offset,length);memcpy(bytes,memory[i]+offset,length);return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t *p,size_t offset,const void *bytes,size_t length)
{
    unsigned i=index_of(p,offset,length);++writes[i];
    if(!i&&fail_journal){memcpy(memory[i]+offset,bytes,25);return ESP_FAIL;}
    memcpy(memory[i]+offset,bytes,length);return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t length)
{ unsigned i=index_of(p,offset,length);assert(!(offset%4096)&&!(length%4096));++erases[i];memset(memory[i]+offset,0xff,length);return ESP_OK; }
esp_err_t esp_partition_mmap(const esp_partition_t *p,size_t offset,size_t length,int kind,const void **address,esp_partition_mmap_handle_t *handle)
{ (void)kind;unsigned i=index_of(p,offset,length);*address=memory[i]+offset;*handle=i;return ESP_OK; }
void esp_partition_munmap(esp_partition_mmap_handle_t handle){assert(handle<3);}

static const char *id="00000000-0000-4000-8000-000000000001";
static const char *hash="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
int main(void)
{
    memset(memory,0xff,sizeof(memory));pet_asset_store_t store={0};
    wrong_map=true;assert(pet_asset_store_open(&store)!=ESP_OK);
    wrong_map=false;assert(pet_asset_store_open(&store)==ESP_OK);
    assert(store.journal.generation==2);
    pet_install_t next=store.state;
    assert(pet_install_request(&next,id,id,hash,0x10003,"prior"));
    assert(pet_asset_store_commit(&store,&next)==ESP_OK);
    next=store.state;assert(pet_install_attach(&next,id,id,hash,0x10003));
    assert(pet_asset_store_commit(&store,&next)==ESP_OK);
    uint8_t block[PET_INSTALL_CHECKPOINT_BYTES];memset(block,0x55,sizeof(block));
    /* Flash data succeeded, journal commit tore: reboot resumes old checkpoint
     * and erases/replays that block without corrupting a protected active slot. */
    fail_journal=true;assert(pet_asset_store_write_block(&store,block,sizeof(block))!=ESP_OK);
    assert(!store.journal.loaded);fail_journal=false;
    assert(pet_asset_store_open(&store)!=ESP_OK);
    pet_asset_store_close(&store);
    assert(pet_asset_store_open(&store)==ESP_OK&&store.state.downloaded_bytes==0);
    assert(pet_asset_store_write_block(&store,block,sizeof(block))==ESP_OK);
    assert(pet_asset_store_write_block(&store,"end",3)==ESP_OK);
    assert(store.state.downloaded_bytes==0x10003&&!memcmp(memory[1]+0x10000,"end",3));
    pet_control_operation_t *operation=calloc(1,sizeof(*operation));assert(operation);
    strcpy(operation->signed_payload,"synthetic-untrusted");strcpy(operation->key_id,"not-trusted");
    memset(operation->signature,'A',86);operation->signature[86]=0;
    assert(pet_asset_store_save_manifest(&store,operation)!=ESP_OK);
    strcpy(operation->id,id);strcpy(operation->build_id,id);strcpy(operation->sha256,hash);
    operation->bytes=0x10003;
    assert(pet_asset_store_save_manifest(&store,operation)==ESP_OK);
    assert(pet_asset_store_load_manifest(&store,0,operation)==ESP_OK);
    assert(!strcmp(operation->signed_payload,"synthetic-untrusted")&&!strcmp(operation->sha256,hash));
    const void *mapped;size_t bytes;
    assert(pet_asset_store_map(&store,0,&mapped,&bytes)==ESP_OK&&bytes==0x10003);
    assert(pet_asset_store_open(&store)!=ESP_OK&&store.mapped[0]);
    assert(pet_asset_store_save_manifest(&store,operation)!=ESP_OK);
    pet_asset_store_unmap(&store,0);
    next=store.state;assert(pet_install_verified(&next)&&pet_install_activate(&next));
    assert(pet_install_commit(&next,id,id,hash,"bound",id,"1"));
    assert(pet_asset_store_commit(&store,&next)==ESP_OK);
    assert(pet_asset_store_map(&store,0,&mapped,&bytes)==ESP_OK);
    unsigned active_writes=writes[1],active_erases=erases[1];
    next=store.state;assert(pet_install_request(&next,id,id,hash,3,"bound"));
    assert(pet_asset_store_commit(&store,&next)==ESP_OK);
    next=store.state;assert(pet_install_attach(&next,id,id,hash,3));
    assert(pet_asset_store_commit(&store,&next)==ESP_OK);
    assert(pet_asset_store_write_block(&store,"new",3)==ESP_OK);
    const void *candidate;size_t candidate_bytes;
    assert(pet_asset_store_map(&store,1,&candidate,&candidate_bytes)==ESP_OK);
    next=store.state;assert(pet_install_cancel(&next));
    assert(pet_asset_store_commit(&store,&next)!=ESP_OK);
    assert(pet_asset_store_map(&store,1,&candidate,&candidate_bytes)==ESP_OK&&candidate_bytes==3);
    pet_asset_store_unmap(&store,1);
    assert(pet_asset_store_commit(&store,&next)==ESP_OK);
    assert(writes[1]==active_writes&&erases[1]==active_erases);
    assert(!memcmp(mapped,block,sizeof(block)));
    pet_asset_store_close(&store);
    free(operation);puts("asset store: exact map, interrupted checkpoint, inactive-only writes and mapping protection passed");
    return 0;
}
