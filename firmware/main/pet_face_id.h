#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A pack's embedded ID names its face. A signed imported release names it
 * `<embedded ID>-<8 lowercase hex>` instead: the cloud's per-project
 * importedFaceId, never a legacy face such as "pablo". derived_ok accepts
 * only that form, exactly: the same prefix, one '-', eight lowercase hex. */
static inline bool pet_face_id_matches(const char *face_id,const uint8_t *id,size_t id_len,bool derived_ok)
{
    if(!face_id||!id||!id_len)return false;
    const size_t length=strlen(face_id);
    if(length==id_len)return !memcmp(face_id,id,id_len);
    if(!derived_ok||length!=id_len+9||memcmp(face_id,id,id_len)||face_id[id_len]!='-')return false;
    for(size_t i=id_len+1;i<length;++i)
        if(!((face_id[i]>='0'&&face_id[i]<='9')||(face_id[i]>='a'&&face_id[i]<='f')))return false;
    return true;
}
