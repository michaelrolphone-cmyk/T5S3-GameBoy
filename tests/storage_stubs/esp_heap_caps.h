#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
inline void *heap_caps_malloc(size_t n, unsigned) { return malloc(n); }
inline void heap_caps_free(void *p) { free(p); }
