#include "pet_release_v2_pack.h"
#include <string.h>
#include "pet_face_id.h"
#include "mbedtls/sha256.h"

static bool reviewed_bytes_match(const pet_release_v2_t *r,const uint8_t *pack,size_t bytes)
{
    if(!r->promoted_approval)return true;
    if(bytes<128||!memchr(r->reviewed_sha256,0,sizeof(r->reviewed_sha256))||
       strlen(r->reviewed_sha256)!=64||strspn(r->reviewed_sha256,"0123456789abcdef")!=64)return false;
    uint8_t header[128],digest[32];memcpy(header,pack,sizeof(header));
    header[28]&=0xfe;memset(header+24,0,4);
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<sizeof(header);++i){crc^=header[i];
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
    crc=~crc;for(unsigned i=0;i<4;++i)header[24+i]=(uint8_t)(crc>>(8*i));
    mbedtls_sha256_context context;mbedtls_sha256_init(&context);
    int error=mbedtls_sha256_starts(&context,0);
    if(!error)error=mbedtls_sha256_update(&context,header,sizeof(header));
    if(!error)error=mbedtls_sha256_update(&context,pack+128,bytes-128);
    if(!error)error=mbedtls_sha256_finish(&context,digest);
    mbedtls_sha256_free(&context);if(error)return false;
    const char *hex="0123456789abcdef";
    for(unsigned i=0;i<32;++i)if(r->reviewed_sha256[i*2]!=hex[digest[i]>>4]||
        r->reviewed_sha256[i*2+1]!=hex[digest[i]&15])return false;
    return true;
}

bool pet_release_v2_validate_pack(const pet_release_v2_t *r,const void *pack,size_t bytes,
                                 void *workspace,uint32_t workspace_bytes,fp_pack_info_t *info)
{
    if(!r||!pack||!bytes||bytes>PET_REPLACE_MAX_BYTES||bytes!=r->pack.bytes||
       !r->requirements.present||r->requirements.bytes!=bytes||r->requirements.format!=2||r->requirements.resolution_divisor!=2||
       !memchr(r->pack.sha256,0,sizeof(r->pack.sha256))||strlen(r->pack.sha256)!=64||strspn(r->pack.sha256,"0123456789abcdef")!=64||
       !memchr(r->face_id,0,sizeof(r->face_id))||!r->face_id[0]||!memchr(r->version,0,sizeof(r->version))||!r->version[0])return false;
    pet_flash_layout_t layout;
    if(!pet_flash_layout_known(r->requirements.layout_id,&layout)||layout.id<PET_LAYOUT_SINGLE_2M||bytes>layout.pack_capacity_bytes)return false;
    uint8_t digest[32];
    if(mbedtls_sha256(pack,bytes,digest,0))return false;
    const char *hex="0123456789abcdef";
    for(unsigned i=0;i<32;++i)if(r->pack.sha256[i*2]!=hex[digest[i]>>4]||r->pack.sha256[i*2+1]!=hex[digest[i]&15])return false;
    fp_pack_info_t parsed={0};
    if(fp_validate_with_workspace(pack,(uint32_t)bytes,&parsed,workspace,workspace_bytes)!=FP_OK||
       !parsed.approved||parsed.move||parsed.format_version!=2||parsed.width!=240||parsed.height!=240||parsed.pack_bytes!=bytes||
       !pet_face_id_matches(r->face_id,parsed.id,parsed.id_len,r->imported)||
       parsed.version_len!=strlen(r->version)||memcmp(parsed.version,r->version,parsed.version_len))return false;
    if(!reviewed_bytes_match(r,pack,bytes))return false;
    if(info)*info=parsed;
    return true;
}
