#pragma once
#include <assert.h>
#include <stdint.h>
#include "mbedtls/md5.h"
typedef mbedtls_md5_context md5_context_t;
static inline void esp_rom_md5_init(md5_context_t *c)
{ mbedtls_md5_init(c); assert(!mbedtls_md5_starts(c)); }
static inline void esp_rom_md5_update(md5_context_t *c,const void *data,uint32_t bytes)
{ assert(!mbedtls_md5_update(c,data,bytes)); }
static inline void esp_rom_md5_final(uint8_t digest[16],md5_context_t *c)
{ assert(!mbedtls_md5_finish(c,digest)); mbedtls_md5_free(c); }
