#include "pet_ota_selection.h"
#include "esp_rom_crc.h"
#include <string.h>

static uint32_t u32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
bool pet_ota_selection_parse(const uint8_t raw[2][PET_OTA_RECORD_BYTES],pet_ota_selection_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));out->active=-1;
    if(!raw)return false;
    pet_ota_selection_t value={.active=-1};
    for(unsigned i=0;i<2;++i){
        pet_ota_record_t *r=&value.records[i];r->sequence=u32(raw[i]);r->state=u32(raw[i]+24);
        r->erased=true;for(unsigned j=0;j<PET_OTA_RECORD_BYTES;++j)if(raw[i][j]!=0xff)r->erased=false;
        /* Same seed/complement semantics as bootloader_common_ota_select_crc. */
        r->crc_valid=r->sequence!=UINT32_MAX&&u32(raw[i]+28)==esp_rom_crc32_le(UINT32_MAX,raw[i],4);
        if(!r->crc_valid)continue;
        if(!r->sequence||(r->state>PET_OTA_STATE_ABORTED&&r->state!=PET_OTA_STATE_UNDEFINED))return false;
        r->slot=(r->sequence-1)%2;
        r->bootable=r->state!=PET_OTA_STATE_INVALID&&r->state!=PET_OTA_STATE_ABORTED;
        if(r->bootable&&(value.active<0||r->sequence>value.records[value.active].sequence))value.active=(int)i;
    }
    if(value.records[0].crc_valid&&value.records[1].crc_valid&&
       value.records[0].sequence==value.records[1].sequence&&value.records[0].state!=value.records[1].state)return false;
    *out=value;return true;
}
bool pet_ota_selection_is(const pet_ota_selection_t *s,unsigned slot,uint32_t state)
{
    return s&&slot<2&&s->active>=0&&s->active<2&&s->records[s->active].crc_valid&&
        s->records[s->active].bootable&&s->records[s->active].slot==slot&&s->records[s->active].state==state;
}
int pet_ota_selection_newest(const pet_ota_selection_t *s,unsigned slot)
{
    if(!s||slot>1)return -1;
    int newest=-1;
    for(unsigned i=0;i<2;++i)if(s->records[i].crc_valid&&s->records[i].slot==slot&&
        (newest<0||s->records[i].sequence>s->records[newest].sequence))newest=(int)i;
    return newest;
}
