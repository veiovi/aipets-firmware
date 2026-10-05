/* The v2 install machine on the three-pet store, on simulated flash: the
 * real writer, journal, inventory and SHA-256. An installation writes only its
 * target slot, never the pet on screen or the bound pet, and every step is one
 * inventory record that survives a reboot. */
#include "pet_slot_store.h"
#include "frame_player.h"
#include "esp_flash.h"
#include "esp_rom_md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash[PET_FLASH_BYTES];
static esp_partition_t parts[10];
static size_t count;
static esp_flash_t chip;
esp_flash_t *esp_flash_default_chip = &chip;
static unsigned freezes, detaches;
static uint8_t copy[PET_LAYOUT_THREE_SLOT_PACK_BYTES];
static uint32_t lowest_write = UINT32_MAX, highest_write;

esp_partition_iterator_t esp_partition_find(int type,int subtype,const char *label)
{ assert(type == ESP_PARTITION_TYPE_ANY && subtype == ESP_PARTITION_SUBTYPE_ANY && !label); return count ? (void *)1 : NULL; }
const esp_partition_t *esp_partition_get(esp_partition_iterator_t it)
{ assert((uintptr_t)it > 0 && (uintptr_t)it <= count); return &parts[(uintptr_t)it - 1]; }
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it)
{ return (uintptr_t)it < count ? (void *)((uintptr_t)it + 1) : NULL; }
void esp_partition_iterator_release(esp_partition_iterator_t it) { assert(it); }
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label)
{
    for (size_t i = 0; i < count; ++i)
        if (parts[i].type == type && parts[i].subtype == subtype && !strcmp(parts[i].label,label)) return &parts[i];
    return NULL;
}
esp_err_t esp_flash_get_physical_size(esp_flash_t *p,uint32_t *out) { assert(!p); *out = PET_FLASH_BYTES; return ESP_OK; }
esp_err_t esp_flash_read(esp_flash_t *p,void *data,uint32_t offset,uint32_t bytes)
{ assert(!p && offset <= sizeof(flash) && bytes <= sizeof(flash)-offset); memcpy(data,flash+offset,bytes); return ESP_OK; }
static void bounds(const esp_partition_t *p,size_t offset,size_t bytes)
{ assert(p >= parts && p < parts+count && offset <= p->size && bytes <= p->size-offset); }
static void note_store(const esp_partition_t *p,size_t offset,size_t bytes)
{
    if (strcmp(p->label,"pet_store")) return;
    if (offset < lowest_write) lowest_write = (uint32_t)offset;
    if (offset + bytes > highest_write) highest_write = (uint32_t)(offset + bytes);
}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *data,size_t bytes)
{ bounds(p,offset,bytes); memcpy(data,flash+p->address+offset,bytes); return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t *p,size_t offset,const void *data,size_t bytes)
{ bounds(p,offset,bytes); note_store(p,offset,bytes); memcpy(flash+p->address+offset,data,bytes); return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t bytes)
{ bounds(p,offset,bytes); assert(!(offset%4096) && !(bytes%4096)); note_store(p,offset,bytes); memset(flash+p->address+offset,0xff,bytes); return ESP_OK; }
esp_err_t esp_partition_mmap(const esp_partition_t *p,size_t offset,size_t bytes,int type,
                            const void **data,esp_partition_mmap_handle_t *handle)
{ (void)p; (void)offset; (void)bytes; (void)type; (void)data; (void)handle; assert(!"a slot is never mapped"); return ESP_FAIL; }
void esp_partition_munmap(esp_partition_mmap_handle_t handle) { (void)handle; assert(!"a slot is never mapped"); }
void vTaskDelay(TickType_t ticks) { (void)ticks; }

static void put32(uint8_t *p,uint32_t value) { for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(i*8)); }
static void configure(void)
{
    pet_flash_layout_t v; assert(pet_flash_layout_known(PET_LAYOUT_THREE_3P5M,&v)); count=v.count;
    memset(parts,0,sizeof(parts)); memset(flash,0xff,sizeof(flash));
    for (size_t i=0;i<count;++i) {
        const pet_flash_region_t *r=&v.regions[i]; esp_partition_t *p=&parts[i];
        p->flash_chip=&chip;p->type=r->type;p->subtype=r->subtype;p->address=r->offset;p->size=r->bytes;
        p->erase_size=4096;strcpy(p->label,r->name);
        uint8_t *raw=flash+PET_PARTITION_TABLE_OFFSET+32*i;memset(raw,0,32);
        raw[0]=0xaa;raw[1]=0x50;raw[2]=r->type;raw[3]=r->subtype;
        put32(raw+4,r->offset);put32(raw+8,r->bytes);memcpy(raw+12,r->name,strlen(r->name));
    }
    uint8_t *p=flash+PET_PARTITION_TABLE_OFFSET+count*32;
    p[0]=p[1]=0xeb;memset(p+2,0xff,14);
    md5_context_t md5;esp_rom_md5_init(&md5);
    esp_rom_md5_update(&md5,flash+PET_PARTITION_TABLE_OFFSET,(uint32_t)count*32);
    esp_rom_md5_final(p+16,&md5);
}

static void hex_sha256(const uint8_t *bytes,size_t length,char out[65])
{
    uint8_t digest[32]; assert(!mbedtls_sha256(bytes,length,digest,0));
    for (unsigned i=0;i<32;++i) snprintf(out+2*i,3,"%02x",digest[i]);
}
static uint8_t *slot_bytes(pet_slot_store_t *s,unsigned slot)
{
    uint32_t pack_offset=0,record_offset=0; assert(pet_flash_layout_slot(&s->layout,slot,&pack_offset,&record_offset));
    return flash+s->layout.pet_offset+pack_offset;
}
/* A pet placed the way the USB installation places it, then shown. */
static void place(pet_slot_store_t *s,unsigned slot,size_t length,uint8_t fill)
{
    uint8_t *bytes=slot_bytes(s,slot);
    for (size_t i=0;i<length;++i) bytes[i]=(uint8_t)(fill+i*13u);
    pet_slot_inventory_t next=s->inventory.state; pet_slot_t *t=&next.slots[slot];
    t->state=PET_SLOT_READY;t->pack.bytes=(uint32_t)length;t->validator_revision=FP_VALIDATOR_REVISION;
    snprintf(t->pack.build_id,sizeof(t->pack.build_id),"00000000-0000-4000-8000-00000000000%u",slot);
    hex_sha256(bytes,length,t->pack.sha256);
    next.active=(int)slot;next.selection_revision++;t->shown_at=next.selection_revision;
    assert(pet_slot_store_commit(s,&next)==ESP_OK);
}

static void freeze(void *context){(void)context;++freezes;}
static bool detach(void *context){(void)context;++detaches;return true;}
static const pet_slot_readers_t readers={.freeze=freeze,.detach=detach};

static void reboot(pet_slot_store_t *s)
{
    pet_replace_writer_close(&s->writer);
    assert(pet_slot_store_open(s)==ESP_OK);
    assert(pet_slot_store_attach_installer(s,&readers)==ESP_OK);
}
static void step(pet_slot_store_t *s,const pet_replace_t *next)
{ assert(pet_replace_journal_commit(&s->machine,next)==PET_JOURNAL_OK); }

/* One installation through the signed v2 steps, with the writer the control
 * worker uses. Returns the slot it was written to. */
static const pet_replace_pack_t *replacement;
static unsigned install(pet_slot_store_t *s,const uint8_t *content,uint32_t length,const char *build,
                        const char *operation,const char *fence,bool interrupt)
{
    pet_replace_pack_t target={.bytes=length}; strcpy(target.build_id,build); hex_sha256(content,length,target.sha256);
    assert(pet_slot_store_prepare_replacement(s,&target,replacement));
    pet_replace_t next=s->machine.state;
    assert(pet_replace_request(&next,operation,&target,next.binding_revision[0]?next.binding_revision:"r0"));step(s,&next);
    int slot=s->inventory.state.target;
    assert(slot>=0&&(replacement||(slot!=s->inventory.state.active&&slot!=s->inventory.state.bound)));
    assert(pet_replace_fenced(&next,operation,fence,&target));step(s,&next);
    unsigned frozen=freezes;
    assert(pet_replace_writer_resume(&s->writer));
    assert(freezes>frozen&&s->machine.state.phase==PET_REPLACE_DOWNLOADING&&s->inventory.state.bound==-1);
    assert(s->inventory.state.slots[slot].state==PET_SLOT_INSTALLING);
    uint8_t manifest[PET_REPLACE_MANIFEST_BYTES];memset(manifest,0x42,sizeof(manifest));
    assert(pet_replace_writer_manifest(&s->writer,manifest,sizeof(manifest)));
    assert(!memcmp(slot_bytes(s,(unsigned)slot)+PET_LAYOUT_THREE_SLOT_PACK_BYTES,manifest,sizeof(manifest)));
    /* Firmware protection reads the record of the slot being written. */
    uint8_t written[PET_REPLACE_MANIFEST_BYTES];
    assert(pet_slot_store_read_record(s,(unsigned)slot,written,sizeof(written))==ESP_OK&&!memcmp(written,manifest,sizeof(written)));
    for (uint32_t at=0;at<length;at+=PET_REPLACE_CHECKPOINT_BYTES) {
        if (interrupt&&at==PET_REPLACE_CHECKPOINT_BYTES) {
            /* Power is lost after one checkpoint: the next boot resumes it. */
            reboot(s);assert(s->inventory.state.target==slot&&s->machine.state.downloaded_bytes==at);
            assert(pet_replace_writer_resume(&s->writer));
        }
        uint32_t chunk=length-at<PET_REPLACE_CHECKPOINT_BYTES?length-at:PET_REPLACE_CHECKPOINT_BYTES;
        assert(pet_replace_writer_block(&s->writer,content+at,chunk));
    }
    /* Verification reads a copy, hashed against the request. */
    size_t bytes=0;
    slot_bytes(s,(unsigned)slot)[length/2]^=1;
    assert(pet_slot_store_load_target(s,copy,sizeof(copy),&bytes)==ESP_ERR_INVALID_CRC);
    slot_bytes(s,(unsigned)slot)[length/2]^=1;
    assert(pet_slot_store_load_target(s,copy,length-1,&bytes)==ESP_ERR_INVALID_SIZE);
    assert(pet_slot_store_load_target(s,copy,sizeof(copy),&bytes)==ESP_OK&&bytes==length&&!memcmp(copy,content,length));
    assert(!s->writer.detached); /* Reading the target ends write authority until the writer resumes. */
    next=s->machine.state;assert(pet_replace_verified(&next,target.sha256));step(s,&next);
    assert(pet_replace_activate(&next));step(s,&next);
    assert(pet_replace_commit(&next,operation,fence,&target,"r1","11111111-1111-4111-8111-111111111111","7"));step(s,&next);
    return (unsigned)slot;
}

int main(void)
{
    static pet_slot_store_t s;
    static uint8_t first[300000],second[250001],third[200000];
    for (size_t i=0;i<sizeof(first);++i)first[i]=(uint8_t)(i*7u);
    for (size_t i=0;i<sizeof(second);++i)second[i]=(uint8_t)(i*11u+5u);
    for (size_t i=0;i<sizeof(third);++i)third[i]=(uint8_t)(i*3u+1u);
    configure();
    assert(pet_slot_store_open(&s)==ESP_OK);
    place(&s,0,120000,9);place(&s,1,90000,21); /* Pablo and Luna from the USB installation, Luna on screen */
    static uint8_t before[PET_FLASH_BYTES];memcpy(before,flash,sizeof(before));
    assert(pet_slot_store_attach_installer(&s,&readers)==ESP_OK);
    assert(pet_slot_store_attach_installer(&s,&readers)==ESP_ERR_INVALID_STATE);
    size_t bytes=0;
    assert(pet_slot_store_load_target(&s,copy,sizeof(copy),&bytes)==ESP_ERR_INVALID_STATE);

    // The first website installation goes to the free slot; the pets already
    // installed are not written, and the new pet is shown.
    unsigned slot=install(&s,first,sizeof(first),"22222222-2222-4222-8222-222222222222","33333333-3333-4333-8333-333333333333",
                          "44444444-4444-4444-8444-444444444444",false);
    uint32_t pack_offset=0,record_offset=0;assert(pet_flash_layout_slot(&s.layout,2,&pack_offset,&record_offset));
    assert(slot==2&&lowest_write>=pack_offset&&highest_write<=record_offset+PET_REPLACE_MANIFEST_BYTES);
    assert(!memcmp(slot_bytes(&s,0),before+(slot_bytes(&s,0)-flash),120000));
    assert(!memcmp(slot_bytes(&s,1),before+(slot_bytes(&s,1)-flash),90000));
    const pet_slot_inventory_t *inventory=&s.inventory.state;
    assert(inventory->active==2&&inventory->bound==2&&inventory->target==-1&&inventory->operation.phase==PET_REPLACE_ACTIVE);
    assert(inventory->slots[2].state==PET_SLOT_READY&&inventory->slots[2].validator_revision==FP_VALIDATOR_REVISION);
    assert(inventory->slots[0].state==PET_SLOT_READY&&inventory->slots[1].state==PET_SLOT_READY);

    // It is all one record: after a reboot the new pet copies with its hash.
    reboot(&s);inventory=&s.inventory.state;
    assert(inventory->active==2&&inventory->bound==2&&pet_slot_store_load(&s,2,copy,sizeof(copy),&bytes)==ESP_OK&&bytes==sizeof(first));
    // Its signed record follows the pack; a pet placed over USB has none.
    static uint8_t record[PET_REPLACE_MANIFEST_BYTES],erased[PET_REPLACE_MANIFEST_BYTES];
    memset(erased,0xff,sizeof(erased));
    assert(pet_slot_store_read_record(&s,2,record,sizeof(record))==ESP_OK);
    for(size_t i=0;i<sizeof(record);++i)assert(record[i]==0x42);
    assert(pet_slot_store_read_record(&s,0,record,sizeof(record))==ESP_OK&&!memcmp(record,erased,sizeof(record)));
    assert(pet_slot_store_read_record(&s,2,record,sizeof(record)-1)==ESP_ERR_INVALID_ARG);
    assert(pet_slot_store_read_record(&s,PET_SLOT_COUNT,record,sizeof(record))==ESP_ERR_INVALID_ARG);

    // With every slot full, the next installation replaces the stalest pet that
    // is neither on screen nor bound, while Pablo's new neighbour stays shown.
    lowest_write=UINT32_MAX;highest_write=0;
    assert(pet_slot_store_load(&s,0,copy,sizeof(copy),&bytes)==ESP_OK);unsigned detached=detaches;
    slot=install(&s,second,sizeof(second),"55555555-5555-4555-8555-555555555555","66666666-6666-4666-8666-666666666666",
                 "77777777-7777-4777-8777-777777777777",true);
    assert(slot==0&&detaches>detached); /* Pablo was shown longest ago; his readers let go first */
    assert(pet_flash_layout_slot(&s.layout,0,&pack_offset,&record_offset));
    assert(lowest_write>=pack_offset&&highest_write<=record_offset+PET_REPLACE_MANIFEST_BYTES);
    assert(inventory->active==0&&inventory->bound==0&&inventory->slots[1].state==PET_SLOT_READY);
    assert(!memcmp(slot_bytes(&s,2),first,sizeof(first)));

    // A request cancelled before its fence leaves every slot as it was.
    pet_replace_pack_t target={.bytes=sizeof(third)};strcpy(target.build_id,"88888888-8888-4888-8888-888888888888");
    hex_sha256(third,sizeof(third),target.sha256);
    pet_replace_t next=s.machine.state;
    assert(pet_replace_request(&next,"99999999-9999-4999-8999-999999999999",&target,next.binding_revision));step(&s,&next);
    assert(inventory->target==1);
    assert(pet_replace_cancel(&next));step(&s,&next);
    assert(inventory->target==-1&&inventory->slots[1].state==PET_SLOT_READY&&inventory->bound==0&&inventory->active==0);
    s.inventory.state.slots[1].state=PET_SLOT_FREE;
    assert(pet_slot_store_read_record(&s,1,record,sizeof(record))==ESP_ERR_INVALID_STATE);
    s.inventory.state.slots[1].state=PET_SLOT_INSTALLING; /* Only the target is ever written. */
    assert(pet_slot_store_read_record(&s,1,record,sizeof(record))==ESP_ERR_INVALID_STATE);
    s.inventory.state.slots[1].state=PET_SLOT_READY;
    reboot(&s);assert(s.inventory.state.slots[1].state==PET_SLOT_READY&&s.inventory.state.bound==0);
    // Same-pet update replaces the exact active/bound slot, not older Luna.
    const pet_replace_pack_t old=s.inventory.state.slots[0].pack;
    pet_replace_pack_t missing=old;missing.bytes++;
    assert(!pet_slot_store_prepare_replacement(&s,&target,&missing));
    memcpy(before,flash,sizeof(before));replacement=&old;lowest_write=UINT32_MAX;highest_write=0;
    slot=install(&s,third,sizeof(third),target.build_id,"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                 "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",true);
    assert(slot==0&&s.inventory.state.bound==0&&s.inventory.state.active==0);
    assert(pet_flash_layout_slot(&s.layout,0,&pack_offset,&record_offset));
    assert(lowest_write>=pack_offset&&highest_write<=record_offset+PET_REPLACE_MANIFEST_BYTES);
    for(unsigned other=1;other<PET_SLOT_COUNT;++other)
        assert(!memcmp(slot_bytes(&s,other),before+(slot_bytes(&s,other)-flash),PET_LAYOUT_THREE_SLOT_PACK_BYTES+PET_REPLACE_MANIFEST_BYTES));
    reboot(&s);assert(s.inventory.state.bound==0&&!strcmp(s.inventory.state.slots[0].pack.build_id,target.build_id));
    puts("three-pet installation: v2 machine per slot, target never shown or bound, other pets untouched, resume, reboot, cancel");
    return 0;
}
