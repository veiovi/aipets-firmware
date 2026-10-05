#include "pet_ota_selection.h"
#include "esp_rom_crc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t raw[2][32];
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void record(unsigned i,uint32_t seq,uint32_t state)
{memset(raw[i],0xff,32);put32(raw[i],seq);put32(raw[i]+24,state);put32(raw[i]+28,esp_rom_crc32_le(UINT32_MAX,raw[i],4));}
int main(void)
{
    pet_ota_selection_t s;memset(raw,0xff,sizeof(raw));
    assert(pet_ota_selection_parse(raw,&s)&&s.active==-1&&s.records[0].erased&&s.records[1].erased);
    assert(!pet_ota_selection_is(&s,0,PET_OTA_STATE_VALID)&&pet_ota_selection_newest(&s,1)==-1);
    record(0,1,PET_OTA_STATE_VALID);
    // Fixed zlib CRC fixture, independent of the ROM routine producing records.
    assert(raw[0][28]==0x9a&&raw[0][29]==0x98&&raw[0][30]==0x43&&raw[0][31]==0x47);
    assert(pet_ota_selection_parse(raw,&s)&&pet_ota_selection_is(&s,0,PET_OTA_STATE_VALID));
    record(1,2,PET_OTA_STATE_NEW);assert(pet_ota_selection_parse(raw,&s)&&pet_ota_selection_is(&s,1,PET_OTA_STATE_NEW));
    record(1,2,PET_OTA_STATE_PENDING);assert(pet_ota_selection_parse(raw,&s)&&pet_ota_selection_is(&s,1,PET_OTA_STATE_PENDING));
    record(1,2,PET_OTA_STATE_ABORTED);assert(pet_ota_selection_parse(raw,&s)&&pet_ota_selection_is(&s,0,PET_OTA_STATE_VALID));
    assert(pet_ota_selection_newest(&s,1)==1&&s.records[1].state==PET_OTA_STATE_ABORTED);
    // Every byte prefix of an interrupted record rewrite leaves the previous
    // valid record authoritative or a NEW/PENDING candidate, never an invented
    // VALID target. Missing target metadata is not attempt history.
    uint8_t candidate[32];record(1,2,PET_OTA_STATE_PENDING);memcpy(candidate,raw[1],32);
    for(unsigned n=0;n<=32;++n){memset(raw[1],0xff,32);memcpy(raw[1],candidate,n);
        bool ok=pet_ota_selection_parse(raw,&s);
        if(ok)assert(pet_ota_selection_is(&s,0,PET_OTA_STATE_VALID)||pet_ota_selection_is(&s,1,PET_OTA_STATE_PENDING));
        assert(!pet_ota_selection_is(&s,1,PET_OTA_STATE_VALID));
    }
    record(1,3,PET_OTA_STATE_PENDING);assert(pet_ota_selection_parse(raw,&s));
    assert(s.active==1&&s.records[0].slot==s.records[1].slot&&!pet_ota_selection_is(&s,0,PET_OTA_STATE_VALID));
    record(1,1,PET_OTA_STATE_PENDING);assert(!pet_ota_selection_parse(raw,&s)&&s.active==-1);
    record(1,1,PET_OTA_STATE_VALID);assert(pet_ota_selection_parse(raw,&s)&&s.active==0);
    record(1,0,PET_OTA_STATE_VALID);assert(!pet_ota_selection_parse(raw,&s)&&s.active==-1);
    record(1,2,5);assert(!pet_ota_selection_parse(raw,&s)&&s.active==-1);
    record(1,2,PET_OTA_STATE_UNDEFINED);assert(pet_ota_selection_parse(raw,&s)&&pet_ota_selection_is(&s,1,PET_OTA_STATE_UNDEFINED));
    raw[1][28]^=1;assert(pet_ota_selection_parse(raw,&s)&&pet_ota_selection_is(&s,0,PET_OTA_STATE_VALID)&&!s.records[1].crc_valid);
    record(0,UINT32_MAX,PET_OTA_STATE_VALID);assert(pet_ota_selection_parse(raw,&s)&&s.active==-1);
    // Pinned bootloader selection rules across slots, states, equal sequences
    // and high/wrapping-boundary values. Intentionally reject ambiguous ties.
    const uint32_t seqs[]={1,2,3,4,127,128,0x7fffffff,0x80000000,0xfffffffd,0xfffffffe};
    const uint32_t states[]={0,1,2,3,4,UINT32_MAX};
    for(unsigned a=0;a<10;++a)for(unsigned b=0;b<10;++b)for(unsigned x=0;x<6;++x)for(unsigned y=0;y<6;++y){
        record(0,seqs[a],states[x]);record(1,seqs[b],states[y]);
        bool ok=pet_ota_selection_parse(raw,&s);
        if(seqs[a]==seqs[b]&&states[x]!=states[y]){assert(!ok&&s.active==-1);continue;}
        assert(ok);bool first=states[x]!=3&&states[x]!=4,second=states[y]!=3&&states[y]!=4;
        int expected=first?(second&&seqs[b]>seqs[a]?1:0):(second?1:-1);assert(s.active==expected);
        if(expected>=0)assert(s.records[expected].slot==(seqs[expected?b:a]-1)%2);
    }
    assert(!pet_ota_selection_parse(NULL,&s)&&s.active==-1);assert(!pet_ota_selection_parse(raw,NULL));
    puts("OTA raw selection: pinned CRC, active/newest distinction, duplicates, torn records, state/sequence bounds passed");return 0;
}
