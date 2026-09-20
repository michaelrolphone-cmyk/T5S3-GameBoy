#include <stddef.h>

__attribute__((visibility("hidden"))) int memcmp(const void *lhs, const void *rhs, size_t count) {
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
