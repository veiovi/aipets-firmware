/* AI Pet freestanding tinfl port. See README for pinned source and hardening.
 * No allocator, libc imports, unaligned access, or platform-dependent bitbuf. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MINIZ_EXPORT
/* Avoid interposing ESP-IDF/ROM's independent miniz symbols. */
#define tinfl_decompress fp_tinfl_decompress
#define tinfl_decompress_mem_to_heap fp_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem fp_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback fp_tinfl_decompress_mem_to_callback
#define MINIZ_NO_MALLOC
#define MINIZ_HAS_64BIT_REGISTERS 0
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_LITTLE_ENDIAN 0
#define MZ_MACRO_END while (0)
typedef uint8_t mz_uint8;
typedef int16_t mz_int16;
typedef uint16_t mz_uint16;
typedef uint32_t mz_uint32;
typedef uint32_t mz_uint;
typedef uint64_t mz_uint64;
#define MZ_MIN(a,b) ((a)<(b)?(a):(b))
#define MZ_MAX(a,b) ((a)>(b)?(a):(b))
#define MZ_ASSERT(x) ((void)0)
#define MZ_MALLOC(n) ((void)(n), (void *)0)
#define MZ_REALLOC(p,n) ((void)(p), (void)(n), (void *)0)
#define MZ_FREE(p) ((void)(p))
static inline void *fp_miniz_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static inline void *fp_miniz_set(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}
#define MZ_CLEAR_ARR(a) fp_miniz_set((a), 0, sizeof(a))
#define MZ_READ_LE16(p) ((mz_uint32)((const mz_uint8 *)(p))[0] | ((mz_uint32)((const mz_uint8 *)(p))[1] << 8))
#define MZ_READ_LE32(p) (MZ_READ_LE16(p) | (MZ_READ_LE16((const mz_uint8 *)(p)+2) << 16))
