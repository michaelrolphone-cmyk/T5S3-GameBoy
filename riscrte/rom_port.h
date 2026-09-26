#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The original paperboy_storage implementation is the behavior reference.
 * This narrowly typed bridge is implemented by RiscRTE host APIs, not by a
 * second SD driver. Paths here are absolute RiscRTE VFS paths (/sd/...). */
#define GAMEBOY_ROM_MAX 64u
#define GAMEBOY_ROM_NAME_MAX 128u
#define GAMEBOY_ROM_PATH_MAX 256u
#define GAMEBOY_ROM_BYTES_MAX (4u * 1024u * 1024u)
#define GAMEBOY_ROM_READ_CHUNK (16u * 1024u)
#define GAMEBOY_STATE_ROM_DIRECTORY "/sd/System/State/Applications/gameboy"
#define GAMEBOY_ROM_MANAGER_DIRECTORY "/sd/System/State/Applications/Rom Manager"

typedef struct {
    char name[GAMEBOY_ROM_NAME_MAX];
    uint64_t size;
    uint8_t is_directory;
} gameboy_dirent_t;

typedef struct {
    char name[GAMEBOY_ROM_NAME_MAX];
    char path[GAMEBOY_ROM_PATH_MAX];
    uint32_t size_bytes;
} gameboy_rom_info_t;

typedef struct {
    gameboy_rom_info_t roms[GAMEBOY_ROM_MAX];
    size_t count;
    bool truncated;
} gameboy_rom_catalog_t;

typedef uint32_t gameboy_stream_t;
#define GAMEBOY_INVALID_STREAM 0u

typedef struct {
    bool (*dir_open)(const char *path);
    bool (*dir_next)(gameboy_dirent_t *entry);
    void (*dir_close)(void);
    gameboy_stream_t (*stream_open)(const char *path, size_t *size_out);
    size_t (*stream_read)(gameboy_stream_t stream, void *buffer, size_t capacity);
    void (*stream_close)(gameboy_stream_t stream);
    /* Used only for hosts preceding the append-only streaming API. */
    bool (*read_file)(const char *path, void *buffer, size_t capacity, size_t *size_out);
    /* Optional existence probe used to avoid treating absent RiscRTE state
     * directories as scan failures. */
    bool (*path_exists)(const char *path);
} gameboy_rom_host_t;

typedef enum {
    GAMEBOY_ROM_OK = 0,
    GAMEBOY_ROM_BAD_ARGUMENT,
    GAMEBOY_ROM_DIRECTORY_ERROR,
    GAMEBOY_ROM_PATH_TOO_LONG,
    GAMEBOY_ROM_OUT_OF_MEMORY,
    GAMEBOY_ROM_OPEN_FAILED,
    GAMEBOY_ROM_SIZE_MISMATCH,
    GAMEBOY_ROM_READ_FAILED,
    GAMEBOY_ROM_UNSUPPORTED_HOST
} gameboy_rom_result_t;

/* Preserve legacy SD-root + first-level discovery, then also scan the
 * RiscRTE GameBoy application-state and Rom Manager state roots (plus one
 * child-directory level) when path_exists reports that they are present.
 * Save/state files are ignored by this ROM catalog. No auto-launch. */
gameboy_rom_result_t gameboy_rom_scan(const gameboy_rom_host_t *host,
                                     gameboy_rom_catalog_t *catalog);
/* Caller allocates exactly the chosen catalog entry's size_bytes and decides
 * when to launch. Do not truncate an image on a short read. */
gameboy_rom_result_t gameboy_rom_read_exact(const gameboy_rom_host_t *host,
                                           const gameboy_rom_info_t *rom,
                                           uint8_t *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif
