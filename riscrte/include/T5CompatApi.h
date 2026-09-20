#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T5_APP_ABI_VERSION 1u
#define T5_STORAGE_API_VERSION 1u
#define T5_APP_BUTTON_BACK (1u << 0)
#define T5_APP_BUTTON_CONFIRM (1u << 1)
#define T5_APP_BUTTON_LEFT (1u << 2)
#define T5_APP_BUTTON_RIGHT (1u << 3)
#define T5_APP_BUTTON_UP (1u << 4)
#define T5_APP_BUTTON_DOWN (1u << 5)
#define T5_APP_DIRENT_NAME_MAX 128u

typedef struct {
    uint32_t buttons;
    bool tapped;
    int16_t touch_x;
    int16_t touch_y;
    bool exit_requested;
} t5_app_input_t;

typedef struct {
    char name[T5_APP_DIRENT_NAME_MAX];
    uint64_t size;
    uint8_t is_directory;
} t5_app_dirent_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    int32_t (*screen_width)(void);
    int32_t (*screen_height)(void);
    void (*clear)(void);
    void (*draw_text)(int32_t x, int32_t y, const char *text);
    void (*fill_rect)(int32_t x, int32_t y, int32_t w, int32_t h, bool black);
    void (*present)(bool full_refresh);
    bool (*poll)(t5_app_input_t *input, uint32_t wait_ms);
    uint32_t (*millis)(void);
    bool (*dir_open)(const char *path);
    bool (*dir_next)(t5_app_dirent_t *entry);
    void (*dir_close)(void);
} t5_app_api_prefix_v1;

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    bool (*exists)(const char *path);
    bool (*read_file)(const char *path, void *buffer, size_t capacity, size_t *size_out);
    bool (*write_file_atomic)(const char *path, const void *data, size_t size);
    bool (*remove_file)(const char *path);
} t5_storage_api_prefix_v1;

const t5_app_api_prefix_v1 *t5_app_get_api(uint32_t abi_version);
const t5_storage_api_prefix_v1 *t5_storage_get_api(uint32_t api_version);

#ifdef __cplusplus
}
#endif
