#pragma once
#ifdef __cplusplus
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
extern "C" {
#define TEST_NOEXCEPT noexcept
#else
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#define TEST_NOEXCEPT
#endif
void *lifecycle_malloc(size_t) TEST_NOEXCEPT;
void *lifecycle_calloc(size_t,size_t) TEST_NOEXCEPT;
void *lifecycle_realloc(void *,size_t) TEST_NOEXCEPT;
void lifecycle_free(void *) TEST_NOEXCEPT;
int cap_test_vsnprintf(char *,size_t,const char *,va_list);
char *cap_test_strtok_r(char *,const char *,char **);
int cap_test_toupper(int);
#ifdef __cplusplus
}
#endif
#define malloc lifecycle_malloc
#define calloc lifecycle_calloc
#define realloc lifecycle_realloc
#define free lifecycle_free
#define vsnprintf cap_test_vsnprintf
#define strtok_r cap_test_strtok_r
#define toupper cap_test_toupper
