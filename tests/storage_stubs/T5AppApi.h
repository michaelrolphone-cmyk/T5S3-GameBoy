#pragma once
#include <stdint.h>
#define T5_APP_ABI_VERSION 1u
struct t5_app_dirent_t { char name[256]; uint32_t size; bool is_directory; };
struct t5_app_api_v1 {
  uint32_t struct_size;
  bool (*dir_open)(const char *);
  bool (*dir_next)(t5_app_dirent_t *);
  void (*dir_close)();
  // The production API has additional members before this optional tail.
  // Keep the tail name so storage-owner builds exercise the size guard.
  void (*fill_rounded_rect_tone)(int32_t, int32_t, int32_t, int32_t, int32_t, uint8_t);
  uint8_t (*backlight_level)();
};
const t5_app_api_v1 *t5_app_get_api(uint32_t);
