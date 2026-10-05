/* The three-pet store on simulated flash: real partition-table checks, real
 * journal, real SHA-256. Slot bytes are written the way an installer or the
 * USB migration would leave them. Nothing may map a slot: a pack reaches RAM
 * only through the flash driver, and is hashed there. */
#include "pet_slot_store.h"
#include "esp_flash.h"
#include "esp_rom_md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[PET_FLASH_BYTES];
static esp_partition_t parts[10];
static size_t count;
static esp_flash_t chip;
esp_flash_t *esp_flash_default_chip = &chip;
static unsigned journal_writes, delays, reads;

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
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *data,size_t bytes)
{ bounds(p,offset,bytes); if (!strcmp(p->label,"pet_store")) ++reads; memcpy(data,flash+p->address+offset,bytes); return ESP_OK; }
/* This store writes only its journal; pets arrive through the installer. */
esp_err_t esp_partition_write(const esp_partition_t *p,size_t offset,const void *data,size_t bytes)
{ bounds(p,offset,bytes); assert(!strcmp(p->label,"pet_journal")); ++journal_writes; memcpy(flash+p->address+offset,data,bytes); return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t bytes)
{ bounds(p,offset,bytes); assert(!strcmp(p->label,"pet_journal") && !(offset%4096) && !(bytes%4096)); memset(flash+p->address+offset,0xff,bytes); return ESP_OK; }
esp_err_t esp_partition_mmap(const esp_partition_t *p,size_t offset,size_t bytes,int type,
                            const void **data,esp_partition_mmap_handle_t *handle)
{ (void)p; (void)offset; (void)bytes; (void)type; (void)data; (void)handle; assert(!"a slot is never mapped"); return ESP_FAIL; }
void esp_partition_munmap(esp_partition_mmap_handle_t handle) { (void)handle; assert(!"a slot is never mapped"); }
void vTaskDelay(TickType_t ticks) { (void)ticks; ++delays; }

static void put32(uint8_t *p,uint32_t value) { for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(i*8)); }
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
    uint8_t *p=flash+PET_PARTITION_TABLE_OFFSET+count*32;
    p[0]=p[1]=0xeb;memset(p+2,0xff,14);
    md5_context_t md5;esp_rom_md5_init(&md5);
    esp_rom_md5_update(&md5,flash+PET_PARTITION_TABLE_OFFSET,(uint32_t)count*32);
    esp_rom_md5_final(p+16,&md5);
}

/* Bytes in a slot's pack area plus a ready record for them, then shown. */
static void install(pet_slot_store_t *s, unsigned slot, size_t length, uint8_t fill)
{
    uint32_t pack_offset = 0, manifest_offset = 0;
    assert(pet_flash_layout_slot(&s->layout, slot, &pack_offset, &manifest_offset));
    uint8_t *bytes = flash + s->layout.pet_offset + pack_offset;
    for (size_t i = 0; i < length; ++i) bytes[i] = (uint8_t)(fill + i * 31u);
    uint8_t digest[32]; assert(!mbedtls_sha256(bytes, length, digest, 0));
    pet_slot_inventory_t next = s->inventory.state;
    pet_slot_t *t = &next.slots[slot];
    t->state = PET_SLOT_READY; t->pack.bytes = (uint32_t)length; t->validator_revision = 1;
    snprintf(t->pack.build_id, sizeof(t->pack.build_id), "00000000-0000-4000-8000-00000000000%u", slot);
    for (unsigned i = 0; i < 32; ++i) snprintf(t->pack.sha256 + 2 * i, 3, "%02x", digest[i]);
    next.active = (int)slot; next.selection_revision++; t->shown_at = next.selection_revision;
    assert(pet_slot_store_commit(s, &next) == ESP_OK);
}

int main(void)
{
    static pet_slot_store_t s;
    /* Single-pet layouts never open as the three-pet store. */
    configure(PET_LAYOUT_SINGLE_2M);
    assert(pet_slot_store_open(&s) == ESP_ERR_INVALID_STATE && !journal_writes);

    /* A new device: an empty inventory, persisted before anything else. */
    configure(PET_LAYOUT_THREE_3P5M);
    assert(pet_slot_store_open(&s) == ESP_OK && s.open && s.inventory.state.active == -1 && journal_writes);
    static uint8_t copy[PET_LAYOUT_THREE_SLOT_PACK_BYTES];
    size_t bytes = 0;
    for (unsigned slot = 0; slot < PET_SLOT_COUNT; ++slot)
        assert(pet_slot_store_load(&s, slot, copy, sizeof(copy), &bytes) == ESP_ERR_INVALID_STATE);
    assert(pet_slot_store_load(&s, PET_SLOT_COUNT, copy, sizeof(copy), &bytes) == ESP_ERR_INVALID_ARG);
    assert(pet_slot_store_load(&s, 0, NULL, sizeof(copy), &bytes) == ESP_ERR_INVALID_ARG && !reads);

    /* Installed pets survive a reboot. A pet is copied through the flash
     * driver in yielding 64 KiB slices and the copy is hashed, every time. */
    install(&s, 0, 300000, 3); install(&s, 2, 2999999, 7);
    assert(pet_slot_store_open(&s) == ESP_OK && s.inventory.state.active == 2);
    uint32_t pack_offset = 0, manifest_offset = 0;
    assert(pet_flash_layout_slot(&s.layout, 2, &pack_offset, &manifest_offset));
    const uint8_t *stored = flash + s.layout.pet_offset + pack_offset;
    unsigned before = delays;
    assert(pet_slot_store_load(&s, 2, copy, sizeof(copy), &bytes) == ESP_OK && bytes == 2999999);
    assert(!memcmp(copy, stored, bytes) && reads == 46 && delays - before == 46);
    assert(pet_slot_store_load(&s, 2, copy, sizeof(copy), &bytes) == ESP_OK && reads == 92);
    /* The copy is what is drawn: flash written afterwards does not reach it. */
    flash[s.layout.pet_offset + pack_offset + 77] ^= 0xff;
    assert(copy[77] != stored[77]);
    flash[s.layout.pet_offset + pack_offset + 77] ^= 0xff;
    assert(pet_slot_store_load(&s, 1, copy, sizeof(copy), &bytes) == ESP_ERR_INVALID_STATE);
    /* A buffer too small for the pack is refused before anything is read. */
    before = reads;
    assert(pet_slot_store_load(&s, 2, copy, 2999998, &bytes) == ESP_ERR_INVALID_SIZE && reads == before);

    /* Damaged bytes are refused, whenever the pet is copied. */
    flash[s.layout.pet_offset + pack_offset + 1234] ^= 0x40;
    bytes = 0;
    assert(pet_slot_store_load(&s, 2, copy, sizeof(copy), &bytes) == ESP_ERR_INVALID_CRC && !bytes);
    assert(pet_slot_store_open(&s) == ESP_OK);
    assert(pet_slot_store_load(&s, 2, copy, sizeof(copy), &bytes) == ESP_ERR_INVALID_CRC);
    assert(pet_slot_store_load(&s, 0, copy, sizeof(copy), &bytes) == ESP_OK && bytes == 300000);

    /* A swipe's selection is one journal record and survives a reboot. */
    pet_slot_inventory_t next = s.inventory.state;
    assert(pet_slot_inventory_step(&next, next.active, 1) == 0 && pet_slot_inventory_select(&next, 0));
    unsigned writes = journal_writes;
    assert(pet_slot_store_commit(&s, &next) == ESP_OK && journal_writes == writes + 1);
    assert(pet_slot_store_open(&s) == ESP_OK && s.inventory.state.active == 0);

    /* A corrupt journal fails closed and is never formatted. */
    memset(flash + s.layout.journal_offset, 0, 0x10000);
    writes = journal_writes;
    assert(pet_slot_store_open(&s) == ESP_ERR_INVALID_CRC && !s.open && journal_writes == writes);
    assert(pet_slot_store_commit(&s, &next) == ESP_ERR_INVALID_ARG);
    puts("three-pet store: exact layout, empty start, verified copies through the flash driver, damaged slot refused, persisted selection, corrupt journal closed");
    return 0;
}
