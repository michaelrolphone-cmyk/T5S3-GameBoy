#include "rom_port.h"

#include <stdlib.h>
#include <string.h>

/* Keep the ASCII comparator and insertion semantics from paperboy_storage.cpp.
 * The /sd prefix is a VFS mount translation, not a new ROM directory. */
static unsigned char ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static int compare_ci(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        unsigned char a = ascii_lower((unsigned char)*left++);
        unsigned char b = ascii_lower((unsigned char)*right++);
        if (a != b) return a < b ? -1 : 1;
    }
    if (*left == *right) return 0;
    return *left == '\0' ? -1 : 1;
}

static int compare_roms(const gameboy_rom_info_t *a, const gameboy_rom_info_t *b)
{
    int result = compare_ci(a->name, b->name);
    if (result == 0) result = compare_ci(a->path, b->path);
    if (result == 0) result = strcmp(a->path, b->path);
    return result;
}

static size_t bounded_length(const char *value, size_t limit)
{
    size_t length = 0;
    while (length < limit && value[length] != '\0') ++length;
    return length;
}

static bool valid_name(const char *name)
{
    if (!name || name[0] == '\0' || name[0] == '.') return false;
    for (const char *p = name; *p; ++p) {
        if (*p == '/' || *p == '\\' || (unsigned char)*p < 0x20u) return false;
    }
    return true;
}

static bool has_rom_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (compare_ci(dot, ".gb") == 0 || compare_ci(dot, ".gbc") == 0);
}

static bool path_join(const char *dir, const char *name, char out[GAMEBOY_ROM_PATH_MAX])
{
    size_t d = strlen(dir);
    size_t n = strlen(name);
    if (!valid_name(name) || d + 1u + n >= GAMEBOY_ROM_PATH_MAX) return false;
    memcpy(out, dir, d);
    out[d] = '/';
    memcpy(out + d + 1u, name, n + 1u);
    return true;
}

static void insert_rom(gameboy_rom_catalog_t *catalog, const gameboy_rom_info_t *candidate)
{
    size_t count = catalog->count;
    if (count == GAMEBOY_ROM_MAX) {
        catalog->truncated = true;
        if (compare_roms(candidate, &catalog->roms[count - 1u]) >= 0) return;
        --count;
        catalog->count = count;
    }
    size_t position = count;
    while (position > 0u && compare_roms(candidate, &catalog->roms[position - 1u]) < 0) {
        catalog->roms[position] = catalog->roms[position - 1u];
        --position;
    }
    catalog->roms[position] = *candidate;
    catalog->count = count + 1u;
}

/* Host API permits one open directory per app: collect first-level folder
 * names during root enumeration, close root, THEN visit child directories. */
typedef struct {
    char (*paths)[GAMEBOY_ROM_PATH_MAX];
    size_t count;
    size_t capacity;
} folder_list_t;

static gameboy_rom_result_t remember_folder(folder_list_t *folders, const char *path)
{
    if (folders->count == folders->capacity) {
        size_t cap = folders->capacity ? folders->capacity * 2u : 8u;
        if (cap < folders->capacity || cap > SIZE_MAX / sizeof(*folders->paths))
            return GAMEBOY_ROM_OUT_OF_MEMORY;
        void *new_paths = realloc(folders->paths, cap * sizeof(*folders->paths));
        if (!new_paths) return GAMEBOY_ROM_OUT_OF_MEMORY;
        folders->paths = new_paths;
        folders->capacity = cap;
    }
    memcpy(folders->paths[folders->count++], path, strlen(path) + 1u);
    return GAMEBOY_ROM_OK;
}

static gameboy_rom_result_t scan_one(const gameboy_rom_host_t *host,
                                     gameboy_rom_catalog_t *catalog,
                                     const char *dir, folder_list_t *folders)
{
    if (!host->dir_open(dir)) return GAMEBOY_ROM_DIRECTORY_ERROR;
    gameboy_rom_result_t result = GAMEBOY_ROM_OK;
    gameboy_dirent_t entry;
    while (host->dir_next(&entry)) {
        /* Host fills an exact-size struct. Never treat a nonterminated name as C text. */
        size_t len = bounded_length(entry.name, sizeof(entry.name));
        if (len == sizeof(entry.name)) {
            result = GAMEBOY_ROM_PATH_TOO_LONG;
            continue;
        }
        if (entry.name[0] == '.') continue;
        char path[GAMEBOY_ROM_PATH_MAX];
        if (!path_join(dir, entry.name, path)) {
            result = GAMEBOY_ROM_PATH_TOO_LONG;
            continue;
        }
        if (entry.is_directory) {
            if (folders) {
                gameboy_rom_result_t remembered = remember_folder(folders, path);
                if (remembered != GAMEBOY_ROM_OK) result = remembered;
            }
            continue;
        }
        if (!has_rom_extension(entry.name) || entry.size == 0u ||
            entry.size > GAMEBOY_ROM_BYTES_MAX) continue;
        gameboy_rom_info_t info = {0};
        memcpy(info.name, entry.name, len + 1u);
        memcpy(info.path, path, strlen(path) + 1u);
        info.size_bytes = (uint32_t)entry.size;
        insert_rom(catalog, &info);
    }
    host->dir_close();
    return result;
}

static gameboy_rom_result_t scan_tree(const gameboy_rom_host_t *host,
                                           gameboy_rom_catalog_t *catalog,
                                           const char *root)
{
    folder_list_t folders = {0};
    gameboy_rom_result_t result = scan_one(host, catalog, root, &folders);
    if (result == GAMEBOY_ROM_OK) {
        for (size_t i = 0; i < folders.count; ++i) {
            gameboy_rom_result_t child = scan_one(host, catalog, folders.paths[i], NULL);
            if (child != GAMEBOY_ROM_OK && result == GAMEBOY_ROM_OK) result = child;
        }
    }
    free(folders.paths);
    return result;
}

gameboy_rom_result_t gameboy_rom_scan(const gameboy_rom_host_t *host,
                                     gameboy_rom_catalog_t *catalog)
{
    if (!host || !catalog || !host->dir_open || !host->dir_next || !host->dir_close)
        return GAMEBOY_ROM_BAD_ARGUMENT;
    memset(catalog, 0, sizeof(*catalog));

    gameboy_rom_result_t result = scan_tree(host, catalog, "/sd");
    if (result != GAMEBOY_ROM_OK) return result;

    static const char *const riscrte_roots[] = {
        GAMEBOY_STATE_ROM_DIRECTORY,
        GAMEBOY_ROM_MANAGER_DIRECTORY,
    };
    if (host->path_exists) {
        for (size_t i = 0; i < sizeof(riscrte_roots) / sizeof(riscrte_roots[0]); ++i) {
            if (!host->path_exists(riscrte_roots[i])) continue;
            gameboy_rom_result_t extra = scan_tree(host, catalog, riscrte_roots[i]);
            if (extra != GAMEBOY_ROM_OK && result == GAMEBOY_ROM_OK) result = extra;
        }
    }
    return result;
}

gameboy_rom_result_t gameboy_rom_read_exact(const gameboy_rom_host_t *host,
                                           const gameboy_rom_info_t *rom,
                                           uint8_t *buffer, size_t capacity)
{
    if (!host || !rom || !buffer || rom->size_bytes == 0u ||
        rom->size_bytes > GAMEBOY_ROM_BYTES_MAX || capacity < rom->size_bytes ||
        rom->path[0] != '/' || bounded_length(rom->path, sizeof(rom->path)) == sizeof(rom->path))
        return GAMEBOY_ROM_BAD_ARGUMENT;

    if (host->stream_open && host->stream_read && host->stream_close) {
        size_t actual_size = 0;
        gameboy_stream_t stream = host->stream_open(rom->path, &actual_size);
        if (stream == GAMEBOY_INVALID_STREAM) return GAMEBOY_ROM_OPEN_FAILED;
        gameboy_rom_result_t result = GAMEBOY_ROM_OK;
        if (actual_size != rom->size_bytes) result = GAMEBOY_ROM_SIZE_MISMATCH;
        size_t offset = 0;
        while (result == GAMEBOY_ROM_OK && offset < actual_size) {
            size_t remaining = actual_size - offset;
            size_t chunk = remaining < GAMEBOY_ROM_READ_CHUNK ? remaining : GAMEBOY_ROM_READ_CHUNK;
            size_t read_count = host->stream_read(stream, buffer + offset, chunk);
            if (read_count == 0u || read_count > chunk) {
                result = GAMEBOY_ROM_READ_FAILED;
                break;
            }
            offset += read_count;
        }
        host->stream_close(stream);
        return result;
    }
    if (!host->read_file) return GAMEBOY_ROM_UNSUPPORTED_HOST;
    size_t actual_size = 0;
    if (!host->read_file(rom->path, buffer, rom->size_bytes, &actual_size))
        return GAMEBOY_ROM_READ_FAILED;
    return actual_size == rom->size_bytes ? GAMEBOY_ROM_OK : GAMEBOY_ROM_SIZE_MISMATCH;
}
