/* Self-contained routines required by the existing emulator which are not
 * exported by RiscRTE's ordinary native-app libc table. No libgcc archive or
 * firmware-private symbols are linked into the application. */
#include <stddef.h>
#include <stdint.h>

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

void *memmove(void *destination, const void *source, size_t count) {
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;
    if ((uintptr_t)dst <= (uintptr_t)src ||
        (uintptr_t)dst - (uintptr_t)src >= count) {
        for (size_t i = 0; i < count; ++i) dst[i] = src[i];
    } else {
        while (count) { --count; dst[count] = src[count]; }
    }
    return destination;
}

/* Source-owned restoring division: linking libgcc directly brings unsupported
 * ELF relocation/section constructs into the constrained native loader. */
static uint64_t divmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    if (!d) { if (rem) *rem = 0; return 0; }
    for (unsigned bit = 0; bit < 64; ++bit) {
        unsigned carry = (unsigned)(r >> 63);
        r = (r << 1) | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (carry || r >= d) { r -= d; q |= 1; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return divmod64(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r = 0;
    (void)divmod64(n, d, &r);
    return r;
}
