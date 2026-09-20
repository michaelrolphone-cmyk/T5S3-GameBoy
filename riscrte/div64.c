#include <stdint.h>

static uint64_t divide_u64(uint64_t n, uint64_t d, uint64_t *r_out) {
    if (d == 0) { if (r_out) *r_out = 0; return 0; }
    uint64_t q = 0, r = 0;
    for (unsigned bit = 0; bit < 64; ++bit) {
        unsigned carry = (unsigned)(r >> 63);
        r = (r << 1) | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (carry || r >= d) { r -= d; q |= 1; }
    }
    if (r_out) *r_out = r;
    return q;
}

static uint64_t magnitude_i64(int64_t value) {
    uint64_t bits = (uint64_t)value;
    return value < 0 ? (~bits + 1u) : bits;
}

__attribute__((visibility("hidden"))) uint64_t __udivdi3(uint64_t n, uint64_t d) {
    return divide_u64(n, d, 0);
}

__attribute__((visibility("hidden"))) uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r = 0;
    (void)divide_u64(n, d, &r);
    return r;
}

__attribute__((visibility("hidden"))) int64_t __divdi3(int64_t n, int64_t d) {
    if (d == 0) return 0;
    const int negative = (n < 0) != (d < 0);
    uint64_t q = divide_u64(magnitude_i64(n), magnitude_i64(d), 0);
    return negative ? (int64_t)(~q + 1u) : (int64_t)q;
}

__attribute__((visibility("hidden"))) int64_t __moddi3(int64_t n, int64_t d) {
    if (d == 0) return 0;
    uint64_t r = 0;
    (void)divide_u64(magnitude_i64(n), magnitude_i64(d), &r);
    return n < 0 ? (int64_t)(~r + 1u) : (int64_t)r;
}
