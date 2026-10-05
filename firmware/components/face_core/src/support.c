/* Freestanding libcall backstop. -mbulk-memory lowers most memcpy/memset
 * intrinsics to native wasm instructions, but the compiler is always entitled
 * to emit libcalls; these definitions keep the link honest (we link without
 * --allow-undefined so anything else missing still fails loudly). */
#include <stddef.h>
#include <stdint.h>

void *fc_memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void *fc_memset(void *dest, int value, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)value;
    return dest;
}

void *fc_memmove(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

/* Freestanding wasm may still lower compiler intrinsics to libcalls. Keep the
 * backstop local to wasm so the ESP-IDF component never exports these names. */
#if defined(__wasm__)
void *memcpy(void *dest, const void *src, size_t n) { return fc_memcpy(dest, src, n); }
void *memset(void *dest, int value, size_t n) { return fc_memset(dest, value, n); }
void *memmove(void *dest, const void *src, size_t n) { return fc_memmove(dest, src, n); }
#endif
