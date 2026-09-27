/* Minimal runtime for the freestanding ARM build: the compiler may emit calls to these for struct
 * copies and large zero-fills. The bridge itself has no libc dependency. */
#include <stddef.h>
#include <stdint.h>

__attribute__((visibility("hidden"))) void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((visibility("hidden"))) void *memset(void *dst, int value, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)value;
    return dst;
}

__attribute__((visibility("hidden"))) void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

/* libgcc's default division-by-zero hooks call raise(); the bridge never divides by zero on purpose,
 * so return 0 instead of pulling in libc. */
__attribute__((visibility("hidden"))) int __aeabi_idiv0(int result) { (void)result; return 0; }
__attribute__((visibility("hidden"))) long long __aeabi_ldiv0(long long result) { (void)result; return 0; }
