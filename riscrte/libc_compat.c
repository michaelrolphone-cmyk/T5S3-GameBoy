/* Emulator-only libc compatibility routines for RiscRTE's constrained ELF
 * export list. The existing host exports snprintf, not vsnprintf/strncmp;
 * keep these adaptations in this application, not in RiscRTE firmware. */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

int strncmp(const char *left, const char *right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (!a[i]) return 0;
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

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (const char *start = haystack; *start; ++start) {
        const char *a = start, *b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return (char *)start;
    }
    return NULL;
}

/* Forward individual type-correct format arguments to the firmware-exported
 * snprintf. This preserves va_list formatting for the original CrankBoy core
 * without importing firmware-private vsnprintf. All writes stay within size.
 * %n is deliberately unsupported: it has no legitimate emulator use. */
#define FORMAT_ARG(value) do { \
    if (width_star && precision_star) \
        written = snprintf(slot, room, spec, star_width, star_precision, (value)); \
    else if (width_star) \
        written = snprintf(slot, room, spec, star_width, (value)); \
    else if (precision_star) \
        written = snprintf(slot, room, spec, star_precision, (value)); \
    else \
        written = snprintf(slot, room, spec, (value)); \
} while (0)

int vsnprintf(char *out, size_t capacity, const char *format, va_list args) {
    if (!format || (!out && capacity)) return -1;
    size_t total = 0;
    const char *cursor = format;
    while (*cursor) {
        if (*cursor != '%') {
            if (out && total + 1 < capacity) out[total] = *cursor;
            if (total >= (size_t)INT_MAX) return -1;
            ++total;
            ++cursor;
            continue;
        }
        ++cursor;
        if (*cursor == '%') {
            if (out && total + 1 < capacity) out[total] = '%';
            if (total >= (size_t)INT_MAX) return -1;
            ++total;
            ++cursor;
            continue;
        }
        char spec[64];
        size_t length = 0;
        spec[length++] = '%';
        bool width_star = false, precision_star = false, dotted = false;
        int star_width = 0, star_precision = 0, length_l = 0;
        bool length_z = false, length_t = false, length_j = false;
        char conversion = 0;
        while (*cursor && length + 2 < sizeof(spec)) {
            char ch = *cursor++;
            spec[length++] = ch;
            if (ch == '.') dotted = true;
            if (ch == '*') {
                if (dotted) {
                    if (precision_star) return -1;
                    precision_star = true;
                    star_precision = va_arg(args, int);
                } else {
                    if (width_star) return -1;
                    width_star = true;
                    star_width = va_arg(args, int);
                }
            }
            if (ch == 'l') ++length_l;
            if (ch == 'z') length_z = true;
            if (ch == 't') length_t = true;
            if (ch == 'j') length_j = true;
            if (strchr("diuoxXcspfeEgGaA", ch)) {
                conversion = ch;
                break;
            }
            if (ch == 'n') return -1;
        }
        if (!conversion) return -1;
        spec[length] = 0;
        char *slot = out && total < capacity ? out + total : NULL;
        size_t room = out && total < capacity ? capacity - total : 0;
        int written = -1;
        switch (conversion) {
            case 'd': case 'i':
                if (length_j) { intmax_t value = va_arg(args, intmax_t); FORMAT_ARG(value); }
                else if (length_l >= 2) { long long value = va_arg(args, long long); FORMAT_ARG(value); }
                else if (length_l) { long value = va_arg(args, long); FORMAT_ARG(value); }
                else if (length_z || length_t) { ptrdiff_t value = va_arg(args, ptrdiff_t); FORMAT_ARG(value); }
                else { int value = va_arg(args, int); FORMAT_ARG(value); }
                break;
            case 'u': case 'o': case 'x': case 'X':
                if (length_j) { uintmax_t value = va_arg(args, uintmax_t); FORMAT_ARG(value); }
                else if (length_l >= 2) { unsigned long long value = va_arg(args, unsigned long long); FORMAT_ARG(value); }
                else if (length_l) { unsigned long value = va_arg(args, unsigned long); FORMAT_ARG(value); }
                else if (length_z || length_t) { size_t value = va_arg(args, size_t); FORMAT_ARG(value); }
                else { unsigned value = va_arg(args, unsigned); FORMAT_ARG(value); }
                break;
            case 's': { const char *value = va_arg(args, const char *); FORMAT_ARG(value ? value : "(null)"); break; }
            case 'c': { int value = va_arg(args, int); FORMAT_ARG(value); break; }
            case 'p': { void *value = va_arg(args, void *); FORMAT_ARG(value); break; }
            case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double value = va_arg(args, double); FORMAT_ARG(value); break;
            }
            default: return -1;
        }
        if (written < 0 || total > (size_t)INT_MAX - (size_t)written)
            return -1;
        total += (size_t)written;
    }
    if (out && capacity) out[total < capacity ? total : capacity - 1u] = 0;
    return (int)total;
}
#undef FORMAT_ARG

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
