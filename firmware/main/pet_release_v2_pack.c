#include "pet_release_v2_pack.h"
#include <string.h>
#include "pet_face_id.h"
#include "mbedtls/sha256.h"

static bool digest_matches(const char *hex, const uint8_t digest[32])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++)
    {
        if (hex[i * 2] != digits[digest[i] >> 4] || hex[i * 2 + 1] != digits[digest[i] & 15]) return false;
    }
    return true;
}

bool pet_release_v2_validate_source(const pet_release_v2_t *r, const fp_pack_source_t *source,
                                    const fp_pack_info_t *info, void *scratch, uint32_t scratch_bytes)
{
    if (!r || !source || !source->read || !info || !scratch || scratch_bytes < 128u || source->bytes != r->pack.bytes ||
        source->bytes < 128u || source->bytes > PET_REPLACE_MAX_BYTES || !r->requirements.present ||
        r->requirements.bytes != source->bytes || r->requirements.format != 2 ||
        r->requirements.resolution_divisor != 2 || !memchr(r->pack.sha256, 0, sizeof(r->pack.sha256)) ||
        strlen(r->pack.sha256) != 64 || strspn(r->pack.sha256, "0123456789abcdef") != 64 ||
        !memchr(r->face_id, 0, sizeof(r->face_id)) || !r->face_id[0] || !memchr(r->version, 0, sizeof(r->version)) ||
        !r->version[0])
        return false;
    pet_flash_layout_t layout;
    if (!pet_flash_layout_known(r->requirements.layout_id, &layout) || layout.id < PET_LAYOUT_SINGLE_2M ||
        source->bytes > layout.pack_capacity_bytes || !info->approved || info->move || info->format_version != 2 ||
        info->width != 240 || info->height != 240 || info->pack_bytes != source->bytes ||
        !pet_face_id_matches(r->face_id, info->id, info->id_len, r->imported) ||
        info->version_len != strlen(r->version) || memcmp(info->version, r->version, info->version_len))
        return false;
    if (r->promoted_approval &&
        (!memchr(r->reviewed_sha256, 0, sizeof(r->reviewed_sha256)) || strlen(r->reviewed_sha256) != 64 ||
         strspn(r->reviewed_sha256, "0123456789abcdef") != 64))
        return false;
    uint8_t header[128], digest[32], reviewed[32];
    if (!source->read(source->context, 0, header, sizeof(header))) return false;
    mbedtls_sha256_context published, candidate;
    mbedtls_sha256_init(&published);
    mbedtls_sha256_init(&candidate);
    int error = mbedtls_sha256_starts(&published, 0);
    if (!error) error = mbedtls_sha256_update(&published, header, sizeof(header));
    if (r->promoted_approval)
    {
        header[28] &= 0xfe;
        memset(header + 24, 0, 4);
        uint32_t crc = UINT32_MAX;
        for (unsigned i = 0; i < sizeof(header); i++)
        {
            crc ^= header[i];
            for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        crc = ~crc;
        for (unsigned i = 0; i < 4; i++) header[24 + i] = (uint8_t)(crc >> (8 * i));
        if (!error) error = mbedtls_sha256_starts(&candidate, 0);
        if (!error) error = mbedtls_sha256_update(&candidate, header, sizeof(header));
    }
    for (uint32_t at = 128u; !error && at < source->bytes;)
    {
        uint32_t bytes = source->bytes - at;
        if (bytes > scratch_bytes) bytes = scratch_bytes;
        if (bytes > 8192u) bytes = 8192u;
        if (!source->read(source->context, at, scratch, bytes)) error = -1;
        if (!error) error = mbedtls_sha256_update(&published, scratch, bytes);
        if (!error && r->promoted_approval) error = mbedtls_sha256_update(&candidate, scratch, bytes);
        at += bytes;
    }
    if (!error) error = mbedtls_sha256_finish(&published, digest);
    if (!error && r->promoted_approval) error = mbedtls_sha256_finish(&candidate, reviewed);
    mbedtls_sha256_free(&published);
    mbedtls_sha256_free(&candidate);
    return !error && digest_matches(r->pack.sha256, digest) &&
           (!r->promoted_approval || digest_matches(r->reviewed_sha256, reviewed));
}

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
