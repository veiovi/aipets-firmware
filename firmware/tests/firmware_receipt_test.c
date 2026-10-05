#include "pet_firmware_receipt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint8_t records[2][PET_FIRMWARE_RECEIPT_BYTES];bool present[2];int cut;bool bad_read;} fixture_t;
static int read_record(void *context,unsigned slot,uint8_t out[PET_FIRMWARE_RECEIPT_BYTES])
{fixture_t *f=context;assert(slot<2);if(f->bad_read)return -1;if(!f->present[slot])return 0;memcpy(out,f->records[slot],PET_FIRMWARE_RECEIPT_BYTES);return 1;}
static bool write_record(void *context,unsigned slot,const uint8_t data[PET_FIRMWARE_RECEIPT_BYTES])
{fixture_t *f=context;assert(slot<2);f->present[slot]=true;size_t n=f->cut<0?PET_FIRMWARE_RECEIPT_BYTES:(size_t)f->cut;memcpy(f->records[slot],data,n);return f->cut<0;}
static const char *operation="00000000-0000-4000-8000-000000000005",*boot="00000000-0000-4000-8000-000000000003",*new_boot="00000000-0000-4000-8000-000000000004";
static void hash_fill(char out[65],char c){memset(out,c,64);out[64]=0;}
static pet_firmware_release_t release(void)
{
    pet_firmware_release_t r={.bytes=1650784};strcpy(r.release_id,"00000000-0000-4000-8000-000000000002");
    strcpy(r.version,"fixture-v2");hash_fill(r.sha256,'a');hash_fill(r.requirements.partition_sha256,'c');
    assert(pet_flash_layout_known(PET_LAYOUT_SINGLE_2M,&r.requirements.layout));r.requirements.firmware_epoch=3;
    return r;
}
static void states(void)
{
    pet_firmware_receipt_t r={0};pet_firmware_release_t candidate=release();char previous[65];hash_fill(previous,'b');
    assert(pet_firmware_receipt_valid(&r));assert(!pet_firmware_receipt_status(&r));
    pet_firmware_receipt_t after_boot={0};
    assert(pet_firmware_receipt_begin_at_boot(&after_boot,&candidate,operation,previous,boot,new_boot,0x230000));
    assert(!strcmp(after_boot.previous_boot_id,boot)&&!strcmp(after_boot.report_boot_id,new_boot));
    assert(!pet_firmware_receipt_begin(&r,&candidate,operation,previous,boot,123));
    assert(pet_firmware_receipt_begin(&r,&candidate,operation,previous,boot,0x230000));
    assert(!pet_firmware_receipt_progress(&r,100,boot)); // No acknowledged download acceptance.
    assert(!pet_firmware_receipt_ack(&r,2));assert(pet_firmware_receipt_ack(&r,1));
    assert(pet_firmware_receipt_progress(&r,100,boot));assert(r.sequence==2);
    assert(!pet_firmware_receipt_progress(&r,99,boot));assert(!pet_firmware_receipt_stage(&r,candidate.sha256));
    assert(pet_firmware_receipt_progress(&r,candidate.bytes,new_boot));
    assert(!pet_firmware_receipt_stage(&r,previous));assert(pet_firmware_receipt_stage(&r,candidate.sha256));
    assert(!strcmp(pet_firmware_receipt_status(&r),"downloading"));assert(!pet_firmware_receipt_reboot(&r));
    assert(pet_firmware_receipt_ack(&r,r.sequence));assert(pet_firmware_receipt_reboot(&r));
    assert(!strcmp(pet_firmware_receipt_status(&r),"rebooting"));
    assert(!pet_firmware_receipt_healthy(&r,boot,candidate.sha256,true,true,true,true,PET_FW_STORAGE_SETUP));
    assert(!pet_firmware_receipt_healthy(&r,new_boot,previous,true,true,true,true,PET_FW_STORAGE_SETUP));
    assert(!pet_firmware_receipt_healthy(&r,new_boot,candidate.sha256,false,true,true,true,PET_FW_STORAGE_SETUP));
    assert(!pet_firmware_receipt_healthy(&r,new_boot,candidate.sha256,true,true,true,false,PET_FW_STORAGE_SETUP));
    pet_firmware_receipt_t failed=r;
    assert(!pet_firmware_receipt_fail(&failed,true,boot,previous,"ROLLBACK"));
    assert(!pet_firmware_receipt_fail(&failed,true,new_boot,candidate.sha256,"ROLLBACK"));
    assert(pet_firmware_receipt_fail(&failed,true,new_boot,previous,"HEALTH_TIMEOUT"));
    assert(!strcmp(pet_firmware_receipt_status(&failed),"rolled_back"));
    assert(pet_firmware_receipt_healthy(&r,new_boot,candidate.sha256,true,true,true,true,PET_FW_STORAGE_RECOVERY));
    assert(!pet_firmware_receipt_begin(&r,&candidate,operation,previous,boot,0x230000));
    assert(!pet_firmware_receipt_fail(&r,false,new_boot,candidate.sha256,"LATE_FAILURE"));
    assert(pet_firmware_receipt_ack(&r,r.sequence));
    assert(!pet_firmware_receipt_begin(&r,&candidate,operation,previous,boot,0x230000));
    assert(pet_firmware_receipt_begin(&r,&candidate,"00000000-0000-4000-8000-000000000006",previous,boot,0x230000));
    assert(pet_firmware_receipt_ack(&r,1));assert(pet_firmware_receipt_fail(&r,false,boot,previous,"DOWNLOAD_FAILED"));
    assert(!strcmp(pet_firmware_receipt_status(&r),"failed"));
    r.sequence=UINT32_MAX;r.phase=PET_FW_DOWNLOADING;r.error_code[0]=0;
    assert(!pet_firmware_receipt_progress(&r,100,boot));
}
static void faults(void)
{
    fixture_t baseline={.cut=-1};pet_firmware_receipt_store_t store;
    pet_firmware_receipt_io_t io={read_record,write_record,&baseline};
    assert(pet_firmware_receipt_open(&store,&io));assert(store.generation==1&&store.value.phase==PET_FW_EMPTY);
    pet_firmware_release_t candidate=release();char previous[65];hash_fill(previous,'b');
    pet_firmware_receipt_t next=store.value;
    assert(pet_firmware_receipt_begin(&next,&candidate,operation,previous,boot,0x230000));
    assert(pet_firmware_receipt_save(&store,&next));assert(pet_firmware_receipt_ack(&next,1));
    assert(pet_firmware_receipt_save(&store,&next));assert(store.generation==3);
    assert(pet_firmware_receipt_open(&store,&store.io)); // Callback alias survives reopen.
    pet_firmware_receipt_t illegal=store.value;
    strcpy(illegal.operation_id,"00000000-0000-4000-8000-000000000006");
    assert(pet_firmware_receipt_valid(&illegal)&&!pet_firmware_receipt_save(&store,&illegal)&&store.loaded);
    illegal=store.value;illegal.acknowledged_sequence=0;
    assert(pet_firmware_receipt_valid(&illegal)&&!pet_firmware_receipt_save(&store,&illegal));
    illegal=store.value;illegal.downloaded_bytes=65536;
    assert(pet_firmware_receipt_valid(&illegal)&&!pet_firmware_receipt_save(&store,&illegal));
    illegal=store.value;illegal.target_offset=0x30000;
    assert(pet_firmware_receipt_valid(&illegal)&&!pet_firmware_receipt_save(&store,&illegal));
    for(unsigned cut=0;cut<=PET_FIRMWARE_RECEIPT_BYTES;++cut){
        fixture_t f=baseline;io.context=&f;assert(pet_firmware_receipt_open(&store,&io));
        next=store.value;assert(pet_firmware_receipt_progress(&next,65536,new_boot));f.cut=(int)cut;
        assert(!pet_firmware_receipt_save(&store,&next));assert(!store.loaded);
        assert(!pet_firmware_receipt_save(&store,&next));f.cut=-1;
        assert(pet_firmware_receipt_open(&store,&io));
        assert(store.value.sequence==1||store.value.sequence==2);
        assert(store.value.downloaded_bytes==(store.value.sequence==1?0:65536));
        assert(!strcmp(store.value.operation_id,operation));
        if(cut==PET_FIRMWARE_RECEIPT_BYTES)assert(store.value.sequence==2);
    }
    fixture_t f=baseline;io.context=&f;f.bad_read=true;assert(!pet_firmware_receipt_open(&store,&io));
    f.bad_read=false;memset(f.records,0,sizeof(f.records));assert(!pet_firmware_receipt_open(&store,&io));
    assert(!store.loaded); // Corruption is never an empty record or auto-format.
    f=baseline;assert(pet_firmware_receipt_open(&store,&io));store.generation=UINT64_MAX;
    assert(!pet_firmware_receipt_save(&store,&store.value));
    f=baseline;for(unsigned slot=0;slot<2;++slot)f.records[slot][PET_FIRMWARE_RECEIPT_BYTES-1]^=1;
    assert(!pet_firmware_receipt_open(&store,&io));
    f=(fixture_t){.cut=-1};io.context=&f;assert(pet_firmware_receipt_open(&store,&io));next=store.value;
    assert(pet_firmware_receipt_begin_at_boot(&next,&candidate,operation,previous,boot,new_boot,0x230000));
    assert(pet_firmware_receipt_save(&store,&next)&&!strcmp(store.value.report_boot_id,new_boot));
}
int main(void)
{states();faults();puts("firmware receipts: boot/health/ACK invariants, 769 torn writes, lost commit ACK, corruption and generation bounds passed");return 0;}
