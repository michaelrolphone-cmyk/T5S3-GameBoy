#include "../riscrte/rom_port.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *opened_directory;
static size_t directory_cursor;
static bool overflow_mode;
static bool directory_open_during_other;
static uint32_t closed_directories;
static uint32_t closed_streams;
static size_t stream_cursor;
static size_t stream_reads;
static bool stream_short_size;
static bool stream_fail_midway;
static uint8_t rom_bytes[48u * 1024u];

static bool dir_open(const char *path)
{
    if (opened_directory != NULL) directory_open_during_other = true;
    if (strcmp(path, "/sd") && strcmp(path, "/sd/Games")) return false;
    opened_directory = path;
    directory_cursor = 0;
    return true;
}

static bool dir_next(gameboy_dirent_t *entry)
{
    if (opened_directory == NULL) return false;
    memset(entry, 0, sizeof(*entry));
    if (overflow_mode) {
        if (directory_cursor >= 70) return false;
        (void)snprintf(entry->name, sizeof(entry->name), "R%02u.gb", (unsigned)directory_cursor++);
        entry->size = 32768;
        return true;
    }
    if (!strcmp(opened_directory, "/sd")) {
        switch (directory_cursor++) {
        case 0: strcpy(entry->name, "Zelda.gb"); entry->size = 49152; return true;
        case 1: strcpy(entry->name, "Games"); entry->is_directory = 1; return true;
        case 2: strcpy(entry->name, ".hidden"); entry->is_directory = 1; return true;
        case 3: strcpy(entry->name, "readme.txt"); entry->size = 123; return true;
        default: return false;
        }
    }
    switch (directory_cursor++) {
    case 0: strcpy(entry->name, "Pokemon.GBC"); entry->size = 49152; return true;
    case 1: strcpy(entry->name, "alpha.gb"); entry->size = 49152; return true;
    case 2: strcpy(entry->name, "deeper"); entry->is_directory = 1; return true;
    case 3: strcpy(entry->name, "broken.gb"); entry->size = 0; return true;
    default: return false;
    }
}

static void dir_close(void)
{
    assert(opened_directory != NULL);
    opened_directory = NULL;
    ++closed_directories;
}

static gameboy_stream_t stream_open(const char *path, size_t *size)
{
    assert(!strcmp(path, "/sd/Games/Pokemon.GBC"));
    *size = sizeof(rom_bytes) - (stream_short_size ? 1u : 0u);
    stream_cursor = 0;
    stream_reads = 0;
    return 1u;
}

static size_t stream_read(gameboy_stream_t handle, void *buffer, size_t cap)
{
    assert(handle == 1u);
    ++stream_reads;
    if (stream_fail_midway && stream_reads == 4u) return 0u;
    size_t chunk = cap > 1024u ? 1024u : cap;
    if (chunk > sizeof(rom_bytes) - stream_cursor) chunk = sizeof(rom_bytes) - stream_cursor;
    memcpy(buffer, rom_bytes + stream_cursor, chunk);
    stream_cursor += chunk;
    return chunk;
}

static void stream_close(gameboy_stream_t handle)
{
    assert(handle == 1u);
    ++closed_streams;
}

static bool truncated_fallback(const char *path, void *buffer, size_t cap, size_t *size)
{
    (void)path;
    memset(buffer, 0, cap);
    *size = cap - 1u;
    return true;
}

int main(void)
{
    memset(rom_bytes, 0xA5, sizeof(rom_bytes));
    gameboy_rom_host_t host = {0};
    host.dir_open = dir_open;
    host.dir_next = dir_next;
    host.dir_close = dir_close;
    host.stream_open = stream_open;
    host.stream_read = stream_read;
    host.stream_close = stream_close;

    gameboy_rom_catalog_t *catalog = malloc(sizeof(*catalog));
    assert(catalog);
    assert(gameboy_rom_scan(&host, catalog) == GAMEBOY_ROM_OK);
    assert(catalog->count == 3u && !catalog->truncated);
    assert(!strcmp(catalog->roms[0].name, "alpha.gb"));
    assert(!strcmp(catalog->roms[1].name, "Pokemon.GBC"));
    assert(!strcmp(catalog->roms[1].path, "/sd/Games/Pokemon.GBC"));
    assert(!strcmp(catalog->roms[2].name, "Zelda.gb"));
    assert(!directory_open_during_other && closed_directories == 2u);

    uint8_t *readback = malloc(sizeof(rom_bytes));
    assert(readback);
    assert(gameboy_rom_read_exact(&host, &catalog->roms[1], readback, sizeof(rom_bytes)) == GAMEBOY_ROM_OK);
    assert(stream_reads == sizeof(rom_bytes) / 1024u);
    assert(memcmp(rom_bytes, readback, sizeof(rom_bytes)) == 0);
    assert(closed_streams == 1u);

    stream_short_size = true;
    assert(gameboy_rom_read_exact(&host, &catalog->roms[1], readback, sizeof(rom_bytes)) == GAMEBOY_ROM_SIZE_MISMATCH);
    assert(closed_streams == 2u);
    stream_short_size = false;
    stream_fail_midway = true;
    assert(gameboy_rom_read_exact(&host, &catalog->roms[1], readback, sizeof(rom_bytes)) == GAMEBOY_ROM_READ_FAILED);
    assert(closed_streams == 3u);
    stream_fail_midway = false;

    host.stream_open = NULL;
    host.stream_read = NULL;
    host.stream_close = NULL;
    host.read_file = truncated_fallback;
    assert(gameboy_rom_read_exact(&host, &catalog->roms[1], readback, sizeof(rom_bytes)) == GAMEBOY_ROM_SIZE_MISMATCH);

    overflow_mode = true;
    assert(gameboy_rom_scan(&host, catalog) == GAMEBOY_ROM_OK);
    assert(catalog->count == GAMEBOY_ROM_MAX && catalog->truncated);
    assert(!strcmp(catalog->roms[0].name, "R00.gb"));
    assert(!strcmp(catalog->roms[63].name, "R63.gb"));

    free(readback);
    free(catalog);
    puts("PASS: root/first-level ROM browsing, .gb/.gbc, sorting, catalog cap, exact streamed reads and failure cleanup");
    return 0;
}
