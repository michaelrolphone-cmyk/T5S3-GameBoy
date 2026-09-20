#pragma once
/* The standalone core uses ESP-IDF heap capability calls. A native app only
 * imports the ordinary allocator from RiscRTE's public libc export table.
 * These wrappers deliberately do not import privileged ESP-IDF symbols. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 0x0001u
#define MALLOC_CAP_SPIRAM 0x0002u
#define MALLOC_CAP_8BIT 0x0004u
static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}
static inline void *heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(count, size);
}
static inline void heap_caps_free(void *ptr) {
    free(ptr);
}
