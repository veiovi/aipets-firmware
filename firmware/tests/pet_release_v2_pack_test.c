#include "pet_release_v2_pack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "pet_face_id.h"

/* The cloud's imported face ID: `<pack ID>-<first 8 hex of the project ID>`. */
static void face_id_checks(void)
{
    const uint8_t *id=(const uint8_t *)"pablo";
    assert(pet_face_id_matches("pablo",id,5,false)&&pet_face_id_matches("pablo",id,5,true));
    assert(pet_face_id_matches("pablo-7c4253d1",id,5,true)&&!pet_face_id_matches("pablo-7c4253d1",id,5,false));
    const char *refused[]={"pabla-7c4253d1","luna-7c4253d1","pablo-7c4253d","pablo-7c4253d10","pablo-7C4253D1","pablo-7c4253g1",
        "pablo_7c4253d1","pablo-7c4253d1-x","pablo7c4253d1","pablo-","pabl","pablox","-7c4253d1",""};
    for(size_t i=0;i<sizeof(refused)/sizeof(refused[0]);++i)assert(!pet_face_id_matches(refused[i],id,5,true));
    assert(!pet_face_id_matches(NULL,id,5,true)&&!pet_face_id_matches("pablo",NULL,5,true)&&!pet_face_id_matches("-7c4253d1",id,0,true));
}
int main(int argc,char **argv)
{
    face_id_checks();
    assert(argc==4||argc==5);uint8_t pack[128000];size_t bytes=fread(pack,1,sizeof(pack),stdin);
    pet_release_v2_t release={.requirements={.present=true,.layout_id=PET_LAYOUT_SINGLE_2M,.format=2,.resolution_divisor=2}};
    release.pack.bytes=release.requirements.bytes=(uint32_t)bytes;
    strcpy(release.face_id,"big-sal");strcpy(release.version,"fixture-240-v2");
    release.requirements.codecs=(uint8_t)strtoul(argv[3],NULL,10);
    if(argc==5){release.promoted_approval=true;assert(strlen(argv[4])==64);strcpy(release.reviewed_sha256,argv[4]);}
    uint8_t digest[32];assert(!mbedtls_sha256(pack,bytes,digest,0));
    const char *hex="0123456789abcdef";
    for(unsigned i=0;i<32;++i){release.pack.sha256[2*i]=hex[digest[i]>>4];release.pack.sha256[2*i+1]=hex[digest[i]&15];}
    uint32_t workspace_bytes=fp_validation_workspace_size();void *workspace=malloc(workspace_bytes+8);assert(workspace);
    fp_pack_info_t info={0};fp_error_t code=fp_validate_with_workspace(pack,(uint32_t)bytes,&info,workspace,workspace_bytes);
    assert((code==FP_OK)==!strcmp(argv[2],"structural"));
    bool valid=pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,&info);
    assert(valid==!strcmp(argv[1],"valid"));
    if(valid){
        assert(!pet_release_v2_validate_pack(&release,pack,bytes-1,workspace,workspace_bytes,NULL));
        assert(!pet_release_v2_validate_pack(&release,pack,bytes+1,workspace,workspace_bytes,NULL));
        release.pack.sha256[0]=release.pack.sha256[0]=='a'?'b':'a';
        assert(!pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));
        release.pack.sha256[0]=hex[digest[0]>>4];
        /* The pack's own codecs decide, through the validator. Metadata that
         * lists other codecs cannot reject bytes the SHA-256 already pins. */
        release.requirements.codecs^=1;
        assert(pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));
        release.requirements.codecs^=1;
        release.face_id[0]='x';assert(!pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));release.face_id[0]='b';
        /* A signed imported release names the face `<pack ID>-<8 hex>`. */
        strcpy(release.face_id,"big-sal-7c4253d1");assert(!pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));
        release.imported=true;assert(pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));
        const char *wrong[]={"big-sa-7c4253d1","big-sal-7C4253D1","big-sal-7c4253d","big-sal-7c4253d1a","big-sal-7c4253d1-2"};
        for(size_t i=0;i<sizeof(wrong)/sizeof(wrong[0]);++i){
            strcpy(release.face_id,wrong[i]);assert(!pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));
        }
        strcpy(release.face_id,"big-sal");assert(pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));
        release.imported=false;
        release.version[0]='x';assert(!pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes,NULL));release.version[0]='f';
        if(info.codecs&0xe0)assert(!pet_release_v2_validate_pack(&release,pack,bytes,NULL,0,NULL));
        assert(!pet_release_v2_validate_pack(&release,pack,bytes,(uint8_t *)workspace+1,workspace_bytes,NULL));
        assert(!pet_release_v2_validate_pack(&release,pack,bytes,workspace,workspace_bytes-16,NULL));
    }
    free(workspace);return 0;
}
