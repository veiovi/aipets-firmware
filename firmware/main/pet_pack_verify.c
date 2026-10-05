#include "pet_pack_verify.h"

#include <string.h>
#include "cJSON.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"

static bool equal(const cJSON *obj,const char *key,const char *expected)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    return expected&&cJSON_IsString(v)&&!strcmp(v->valuestring,expected);
}
static bool number_is(const cJSON *obj,const char *key,uint32_t expected)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    return cJSON_IsNumber(v)&&v->valuedouble==expected;
}
static bool copy_text(const cJSON *obj,const char *key,char *out,size_t capacity)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=capacity)return false;
    strcpy(out,v->valuestring);return true;
}
static bool hex_text(const cJSON *obj,const char *key,size_t length)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    return cJSON_IsString(v)&&strlen(v->valuestring)==length&&
        strspn(v->valuestring,"0123456789abcdef")==length;
}
static bool raw_signature(const char *encoded,uint8_t raw[64])
{
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if(strlen(encoded)!=86)return false;
    uint32_t bits=0;unsigned count=0,output=0;
    for(unsigned i=0;i<86;++i) {
        const char *p=strchr(alphabet,encoded[i]);if(!p)return false;
        bits=(bits<<6)|(uint32_t)(p-alphabet);count+=6;
        if(count>=8) {
            count-=8;if(output>=64)return false;
            raw[output++]=(uint8_t)(bits>>count);
        }
    }
    return output==64&&!(bits&((1u<<count)-1u));
}

bool pet_release_verify_signature(const char *payload,size_t payload_capacity,
                                  const char *key_id,size_t key_id_capacity,
                                  const char *encoded,size_t signature_capacity,
                                  const pet_pack_trust_key_t *keys,size_t key_count)
{
    if(!payload||!key_id||!encoded||!keys||!key_count||key_count>8||
       !payload_capacity||payload_capacity>7001||!key_id_capacity||key_id_capacity>161||
       !signature_capacity||signature_capacity>87||!memchr(payload,0,payload_capacity)||
       !memchr(key_id,0,key_id_capacity)||!memchr(encoded,0,signature_capacity)||
       !payload[0]||!key_id[0])return false;
    const pet_pack_trust_key_t *key=NULL;
    for(size_t i=0;i<key_count;++i)
        if(keys[i].key_id&&!strcmp(keys[i].key_id,key_id)){if(key)return false;key=&keys[i];}
    if(!key||key->public_point[0]!=4)return false;
    uint8_t signature[64],digest[32];
    if(!raw_signature(encoded,signature)||mbedtls_sha256((const uint8_t *)payload,strlen(payload),digest,0))return false;
    mbedtls_ecp_group group;mbedtls_ecp_point point;mbedtls_mpi r,s;
    mbedtls_ecp_group_init(&group);mbedtls_ecp_point_init(&point);mbedtls_mpi_init(&r);mbedtls_mpi_init(&s);
    bool verified=mbedtls_ecp_group_load(&group,MBEDTLS_ECP_DP_SECP256R1)==0&&
        mbedtls_ecp_point_read_binary(&group,&point,key->public_point,sizeof(key->public_point))==0&&
        mbedtls_ecp_check_pubkey(&group,&point)==0&&
        mbedtls_mpi_read_binary(&r,signature,32)==0&&mbedtls_mpi_read_binary(&s,signature+32,32)==0&&
        mbedtls_ecdsa_verify(&group,digest,sizeof(digest),&point,&r,&s)==0;
    mbedtls_mpi_free(&s);mbedtls_mpi_free(&r);mbedtls_ecp_point_free(&point);mbedtls_ecp_group_free(&group);
    return verified;
}

bool pet_pack_verify_manifest(const pet_control_operation_t *operation,const char *expected_account,
                               const pet_pack_trust_key_t *keys,size_t key_count,
                               pet_pack_verified_manifest_t *manifest)
{
    if(!operation||!expected_account||strlen(expected_account)!=36||!manifest||
       !memchr(operation->build_id,0,sizeof(operation->build_id))||
       !memchr(operation->sha256,0,sizeof(operation->sha256))||
       !pet_release_verify_signature(operation->signed_payload,sizeof(operation->signed_payload),
          operation->key_id,sizeof(operation->key_id),operation->signature,sizeof(operation->signature),keys,key_count))return false;
    cJSON *payload=pet_control_json(operation->signed_payload,strlen(operation->signed_payload),7000);
    const cJSON *compiler=cJSON_GetObjectItemCaseSensitive(payload,"compiler");
    pet_pack_verified_manifest_t parsed={0};char compiler_version[81];
    bool verified=cJSON_IsObject(payload)&&cJSON_GetArraySize(payload)==15&&
        equal(payload,"kind","private-device-release")&&number_is(payload,"version",1)&&
        equal(payload,"accountId",expected_account)&&equal(payload,"buildId",operation->build_id)&&
        equal(payload,"sha256",operation->sha256)&&number_is(payload,"bytes",operation->bytes)&&
        operation->bytes>0&&operation->bytes<=PET_INSTALL_PACK_MAX&&
        number_is(payload,"formatVersion",1)&&equal(payload,"renderer","frame-player-cloud-0.3")&&
        equal(payload,"readiness","device-ready")&&hex_text(payload,"inputHash",64)&&
        hex_text(payload,"characterFingerprint",64)&&
        copy_text(payload,"accountId",parsed.account_id,sizeof(parsed.account_id))&&
        copy_text(payload,"projectId",parsed.project_id,sizeof(parsed.project_id))&&strlen(parsed.project_id)==36&&
        copy_text(payload,"faceId",parsed.face_id,sizeof(parsed.face_id))&&
        copy_text(payload,"packVersion",parsed.version,sizeof(parsed.version))&&
        cJSON_IsObject(compiler)&&cJSON_GetArraySize(compiler)==3&&
        copy_text(compiler,"version",compiler_version,sizeof(compiler_version))&&
        hex_text(compiler,"commit",40)&&hex_text(compiler,"sha256",64);
    cJSON_Delete(payload);
    if(verified){parsed.bytes=operation->bytes;*manifest=parsed;}
    return verified;
}

bool pet_pack_verify_bytes(const void *bytes,size_t length,const char *expected_sha256)
{
    if(!bytes||!length||length>PET_INSTALL_PACK_MAX||!expected_sha256||strlen(expected_sha256)!=64||
       strspn(expected_sha256,"0123456789abcdef")!=64)return false;
    uint8_t digest[32];if(mbedtls_sha256(bytes,length,digest,0))return false;
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i)
        if(expected_sha256[2*i]!=hex[digest[i]>>4]||expected_sha256[2*i+1]!=hex[digest[i]&15])return false;
    return true;
}
