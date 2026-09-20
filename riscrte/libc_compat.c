#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((visibility("hidden"))) int memcmp(const void *lhs, const void *rhs, size_t count) {
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

__attribute__((visibility("hidden"))) int strncmp(const char *a, const char *b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ca = (unsigned char)a[i];
        const unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
    }
    return 0;
}

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} fmt_out_t;

static void emit_char(fmt_out_t *out, char ch) {
    if (out->buffer && out->capacity && out->length + 1u < out->capacity) {
        out->buffer[out->length] = ch;
    }
    ++out->length;
}

static void emit_string(fmt_out_t *out, const char *text) {
    if (!text) text = "(null)";
    while (*text) emit_char(out, *text++);
}

static void emit_uint(fmt_out_t *out, uint64_t value, unsigned base, int uppercase) {
    char digits[24];
    size_t used = 0;
    const char *alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[used++] = alphabet[value % base];
        value /= base;
    } while (value && used < sizeof(digits));
    while (used) emit_char(out, digits[--used]);
}

/* Small source-owned formatter for CrankBoy's diagnostic aprintf paths. */
__attribute__((visibility("hidden"))) int vsnprintf(char *buffer, size_t capacity, const char *format, va_list args) {
    fmt_out_t out = {buffer, capacity, 0};
    if (!format) return -1;

    while (*format) {
        if (*format != '%') {
            emit_char(&out, *format++);
            continue;
        }
        ++format;
        if (*format == '%') {
            emit_char(&out, '%');
            ++format;
            continue;
        }

        /* Ignore common width/zero-padding syntax; diagnostics do not rely on alignment. */
        while (*format == '0' || (*format >= '1' && *format <= '9')) ++format;
        int long_count = 0;
        while (*format == 'l') { ++long_count; ++format; }

        switch (*format) {
            case 's': emit_string(&out, va_arg(args, const char *)); break;
            case 'c': emit_char(&out, (char)va_arg(args, int)); break;
            case 'd':
            case 'i': {
                int64_t value = long_count >= 2 ? va_arg(args, int64_t) :
                                long_count == 1 ? va_arg(args, long) : va_arg(args, int);
                uint64_t mag = (uint64_t)value;
                if (value < 0) { emit_char(&out, '-'); mag = ~mag + 1u; }
                emit_uint(&out, mag, 10u, 0);
                break;
            }
            case 'u': {
                uint64_t value = long_count >= 2 ? va_arg(args, uint64_t) :
                                 long_count == 1 ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                emit_uint(&out, value, 10u, 0);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t value = long_count >= 2 ? va_arg(args, uint64_t) :
                                 long_count == 1 ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                emit_uint(&out, value, 16u, *format == 'X');
                break;
            }
            case 'p':
                emit_string(&out, "0x");
                emit_uint(&out, (uintptr_t)va_arg(args, void *), 16u, 0);
                break;
            default:
                emit_char(&out, '%');
                if (*format) emit_char(&out, *format);
                break;
        }
        if (*format) ++format;
    }

    if (buffer && capacity) {
        size_t pos = out.length < capacity ? out.length : capacity - 1u;
        buffer[pos] = '\0';
    }
    return (int)out.length;
}
