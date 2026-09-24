#pragma once
#include <stdint.h>
#include <stddef.h>
#define T5_STORAGE_API_VERSION 1u
using t5_storage_stream_t = uint32_t;
#define T5_STORAGE_STREAM_INVALID 0u
struct t5_storage_api_v1 {
  uint32_t api_version, struct_size;
  bool (*exists)(const char *);
  bool (*read_file)(const char *, void *, size_t, size_t *);
  bool (*write_file_atomic)(const char *, const void *, size_t);
  bool (*remove_file)(const char *);
  t5_storage_stream_t (*stream_open)(const char *, size_t *);
  size_t (*stream_read)(t5_storage_stream_t, void *, size_t);
  bool (*stream_seek)(t5_storage_stream_t, size_t);
  void (*stream_close)(t5_storage_stream_t);
};
const t5_storage_api_v1 *t5_storage_get_api(uint32_t);
