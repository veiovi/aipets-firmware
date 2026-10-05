#include "pet_single_store.h"
#include "esp_flash.h"
#include "esp_rom_md5.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[PET_FLASH_BYTES];
static esp_partition_t parts[10];
static size_t count;
static esp_flash_t chip;
esp_flash_t *esp_flash_default_chip = &chip;
static uint32_t physical_bytes = PET_FLASH_BYTES;
static unsigned erases, writes, mappings, detach_calls;
static bool detach_failure, frozen;

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
esp_err_t esp_flash_get_physical_size(esp_flash_t *p,uint32_t *out)
{ assert(!p); *out = physical_bytes; return ESP_OK; }
esp_err_t esp_flash_read(esp_flash_t *p,void *data,uint32_t offset,uint32_t bytes)
{ assert(!p && offset <= sizeof(flash) && bytes <= sizeof(flash)-offset); memcpy(data,flash+offset,bytes); return ESP_OK; }
static void bounds(const esp_partition_t *p,size_t offset,size_t bytes)
{ assert(p >= parts && p < parts+count && offset <= p->size && bytes <= p->size-offset); }
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *data,size_t bytes)
{ bounds(p,offset,bytes); memcpy(data,flash+p->address+offset,bytes); return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t *p,size_t offset,const void *data,size_t bytes)
{
    bounds(p,offset,bytes); ++writes;
    assert(!strcmp(p->label,"pet_journal") || !strcmp(p->label,"pet_pack"));
    assert(strcmp(p->label,"pet_pack") || !mappings);
    memcpy(flash+p->address+offset,data,bytes); return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t bytes)
{
    bounds(p,offset,bytes); assert(!(offset%4096) && !(bytes%4096)); ++erases;
    assert(!strcmp(p->label,"pet_journal") || !strcmp(p->label,"pet_pack"));
    assert(strcmp(p->label,"pet_pack") || (!mappings && frozen));
    memset(flash+p->address+offset,0xff,bytes); return ESP_OK;
}
esp_err_t esp_partition_mmap(const esp_partition_t *p,size_t offset,size_t bytes,int type,
                            const void **data,esp_partition_mmap_handle_t *handle)
{
    (void)type; bounds(p,offset,bytes); assert(!mappings); ++mappings;
    *data=flash+p->address+offset; *handle=1; return ESP_OK;
}
void esp_partition_munmap(esp_partition_mmap_handle_t handle) { assert(handle == 1 && mappings == 1); --mappings; }
static void freeze(void *unused) { (void)unused; frozen = true; }
static bool detach(void *unused) { (void)unused; ++detach_calls; return !detach_failure; }
static const pet_single_readers_t readers = {.freeze=freeze,.detach=detach};
static void put32(uint8_t *p,uint32_t value)
{ for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(i*8)); }
static void checksum_table(void)
{
    uint8_t *p=flash+PET_PARTITION_TABLE_OFFSET+count*32;
    p[0]=p[1]=0xeb;memset(p+2,0xff,14);
    md5_context_t md5;esp_rom_md5_init(&md5);
    esp_rom_md5_update(&md5,flash+PET_PARTITION_TABLE_OFFSET,(uint32_t)count*32);
    esp_rom_md5_final(p+16,&md5);
}
static void configure(pet_layout_id_t id)
{
    pet_flash_layout_t v; assert(pet_flash_layout_known(id,&v)); count=v.count;
    memset(parts,0,sizeof(parts)); memset(flash,0xff,sizeof(flash));
    for (size_t i=0;i<count;++i) {
        const pet_flash_region_t *r=&v.regions[i]; esp_partition_t *p=&parts[i];
        p->flash_chip=&chip;p->type=r->type;p->subtype=r->subtype;p->address=r->offset;p->size=r->bytes;
        p->erase_size=4096;strcpy(p->label,r->name);
        uint8_t *raw=flash+PET_PARTITION_TABLE_OFFSET+32*i;memset(raw,0,32);
        raw[0]=0xaa;raw[1]=0x50;raw[2]=r->type;raw[3]=r->subtype;
        put32(raw+4,r->offset);put32(raw+8,r->bytes);memcpy(raw+12,r->name,strlen(r->name));
    }
    checksum_table();
}
int main(void)
{
    pet_single_store_t s={0}; configure(PET_LAYOUT_DUAL_V1);
    assert(pet_single_store_open(&s,&readers)==ESP_ERR_INVALID_STATE && !erases && !writes);
    for(unsigned id=PET_LAYOUT_SINGLE_2M;id<=PET_LAYOUT_SINGLE_4M;++id) {
        configure((pet_layout_id_t)id);
        assert(pet_single_store_open(&s,&readers)==ESP_OK && s.layout.id==id);
        assert(s.state.ready && s.state.journal.generation==2);
        assert(pet_single_store_open(&s,&readers)==ESP_ERR_INVALID_STATE);
        assert(pet_single_store_close(&s));
    }
    configure(PET_LAYOUT_SINGLE_2M);unsigned before=erases;
    physical_bytes=32*1024*1024;assert(pet_single_store_open(&s,&readers)==ESP_ERR_INVALID_SIZE);
    physical_bytes=PET_FLASH_BYTES;parts[0].encrypted=true;
    assert(pet_single_store_open(&s,&readers)==ESP_ERR_INVALID_STATE);parts[0].encrypted=false;
    parts[7].readonly=true;assert(pet_single_store_open(&s,&readers)==ESP_ERR_INVALID_STATE);parts[7].readonly=false;
    parts[7].erase_size=65536;assert(pet_single_store_open(&s,&readers)==ESP_ERR_INVALID_STATE);parts[7].erase_size=4096;
    flash[PET_PARTITION_TABLE_OFFSET+PET_PARTITION_TABLE_BYTES]=0;
    assert(pet_single_store_open(&s,&readers)==ESP_FAIL);
    flash[PET_PARTITION_TABLE_OFFSET+PET_PARTITION_TABLE_BYTES]=0xff;assert(erases==before);
    /* SDK-visible fields remain normalized/unchanged while raw flags differ. */
    for(unsigned flags=1;flags<=4;flags<<=1) {
        put32(flash+PET_PARTITION_TABLE_OFFSET+28,flags);checksum_table();
        assert(pet_single_store_open(&s,&readers)==ESP_FAIL && erases==before);
    }
    put32(flash+PET_PARTITION_TABLE_OFFSET+28,0);checksum_table();
    flash[PET_PARTITION_TABLE_OFFSET+count*32+16]^=1;
    assert(pet_single_store_open(&s,&readers)==ESP_FAIL && erases==before);checksum_table();
    memset(flash+parts[6].address,0,parts[6].size);
    assert(pet_single_store_open(&s,&readers)==ESP_ERR_INVALID_CRC);
    assert(s.layout_valid && s.initialized && !s.state.ready && erases==before);
    assert(pet_single_store_close(&s));configure(PET_LAYOUT_SINGLE_2M);
    assert(pet_single_store_open(&s,&readers)==ESP_OK);
    const char *uuid="00000000-0000-4000-8000-000000000001";
    pet_replace_pack_t pack={.bytes=3};strcpy(pack.build_id,uuid);
    strcpy(pack.sha256,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    pet_replace_t next=s.state.state;assert(pet_replace_request(&next,uuid,&pack,"r0"));
    assert(pet_replace_fenced(&next,uuid,uuid,&pack));assert(pet_replace_journal_commit(&s.state,&next)==PET_JOURNAL_OK);
    const void *mapped;size_t bytes;
    assert(pet_single_store_map(&s,&mapped,&bytes)!=ESP_OK);
    assert(pet_replace_writer_resume(&s.writer));
    assert(pet_replace_writer_block(&s.writer,"new",3));
    assert(pet_single_store_map(&s,&mapped,&bytes)==ESP_OK && bytes==3 && !memcmp(mapped,"new",3));
    assert(mappings==1 && !s.writer.resume_checked);
    assert(!pet_replace_writer_block(&s.writer,"new",3));
    detach_failure=true;assert(!pet_single_store_close(&s));assert(mappings==1 && s.initialized);
    detach_failure=false;assert(pet_single_store_close(&s));assert(!mappings && detach_calls);
    puts("single-pet ESP adapter: physical flash, exact layout, corrupt-journal recovery, detached mappings and bounded partition writes passed");
    return 0;
}
