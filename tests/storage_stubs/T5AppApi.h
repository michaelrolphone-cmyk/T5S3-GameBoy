#pragma once
#include <stdint.h>
#define T5_APP_ABI_VERSION 1u
struct t5_app_dirent_t { char name[256]; uint32_t size; bool is_directory; };
struct t5_app_api_v1 {
  uint32_t struct_size;
  bool (*dir_open)(const char *);
  bool (*dir_next)(t5_app_dirent_t *);
  void (*dir_close)();
};
const t5_app_api_v1 *t5_app_get_api(uint32_t);
