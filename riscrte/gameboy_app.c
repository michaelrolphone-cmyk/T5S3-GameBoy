/* Independently loadable GameBoy front end for RiscRTE. The original gbemu.c,
 * CrankBoy CPU/PPU, MiniGB APU and built-in demo are linked into this ELF.
 * Hardware is accessed only through RiscRTE's already-exported app/storage APIs.
 * The e-paper screen is refreshed at a much lower rate than CPU emulation. */
#define _POSIX_C_SOURCE 200809L
#include <T5AppApi.h>
#include <T5StorageApi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/gbemu.h"
#include "../src/audio.h"

#define ROM_LIMIT 48u
#define ROM_PATH_CAP 192u
#define STATE_LIMIT (1024u * 1024u)
#define FRAME_REFRESH_MS 350u
#define BUTTON_HOLD_MS 900u

extern const uint8_t *riscrte_gameboy_demo_data(void);
extern size_t riscrte_gameboy_demo_size(void);

/* The host ABI is append-only: check the final field used, not just version. */
#define HAS_API_FIELD(table, type, field) \
    ((table) != NULL && (table)->struct_size >= \
      offsetof(type, field) + sizeof(((type *)0)->field) && (table)->field != NULL)

typedef struct {
    char name[T5_APP_DIRENT_NAME_MAX];
    char path[ROM_PATH_CAP];
    size_t size;
} rom_item_t;

static const t5_app_api_v1 *s_app;
static const t5_storage_api_v1 *s_storage;
static rom_item_t s_roms[ROM_LIMIT];
static uint32_t s_rom_count;
static bool s_exit;
static bool s_menu_requested;
static bool s_save_requested;
static bool s_load_requested;
static uint32_t s_buttons;
static uint8_t s_touch_mask;
static uint32_t s_touch_expires;
static uint32_t s_menu_hold_started;
static char s_notice[72];

/* Referenced by the ESP timer compatibility header used in original gbemu.c. */
int64_t riscrte_gameboy_timer_us(void) {
    return HAS_API_FIELD(s_app, t5_app_api_v1, millis)
        ? (int64_t)(uint64_t)s_app->millis() * 1000LL : 0;
}

static uint32_t milliseconds(void) { return s_app->millis(); }
static bool elapsed(uint32_t now, uint32_t then, uint32_t duration) {
    return (uint32_t)(now - then) >= duration;
}

static void copy_name(char *dst, size_t cap, const char *src) {
    size_t index = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    while (src[index] && index + 1 < cap) {
        dst[index] = src[index];
        ++index;
    }
    dst[index] = 0;
}

static char lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static bool has_extension(const char *name, const char *extension) {
    size_t n = strlen(name), e = strlen(extension);
    if (n <= e) return false;
    for (size_t i = 0; i < e; ++i)
        if (lower(name[n - e + i]) != extension[i]) return false;
    return true;
}

static void scan_directory(const char *folder) {
    t5_app_dirent_t entry;
    if (!s_app->dir_open(folder)) return;
    while (s_rom_count < ROM_LIMIT && s_app->dir_next(&entry)) {
        entry.name[sizeof(entry.name) - 1] = 0;
        if (entry.is_directory || entry.size < 0x150u ||
            entry.size > GBEMU_MAX_ROM_BYTES ||
            (!has_extension(entry.name, ".gb") &&
             !has_extension(entry.name, ".gbc"))) continue;
        /* getName() supplies a basename. Never concatenate path separators,
         * dot segments or unbounded names returned from removable media. */
        if (!entry.name[0] || entry.name[0] == '.' ||
            strchr(entry.name, '/') || strchr(entry.name, '\\') ||
            strstr(entry.name, "..")) continue;
        size_t folder_len = strlen(folder), name_len = strlen(entry.name);
        if (folder_len + 1u + name_len + 1u > ROM_PATH_CAP) continue;
        rom_item_t *item = &s_roms[s_rom_count++];
        memcpy(item->path, folder, folder_len);
        item->path[folder_len] = '/';
        memcpy(item->path + folder_len + 1u, entry.name, name_len + 1u);
        copy_name(item->name, sizeof(item->name), entry.name);
        item->size = (size_t)entry.size;
    }
    s_app->dir_close();
}

static void discover_roms(void) {
    s_rom_count = 1;
    memset(s_roms, 0, sizeof(s_roms));
    copy_name(s_roms[0].name, sizeof(s_roms[0].name),
              "Built-in homebrew demo");
    s_roms[0].size = riscrte_gameboy_demo_size();
    scan_directory("/sd/ROMs");
    scan_directory("/sd/roms");
    scan_directory("/sd/GameBoy/ROMs");
}

static void message_screen(const char *title, const char *detail) {
    s_app->clear();
    s_app->draw_text(22, 52, title);
    if (detail) s_app->draw_text(22, 95, detail);
    s_app->draw_text(22, 155, "Power/Home exits to RiscRTE");
    s_app->present(true);
}

static void render_menu(uint32_t selected) {
    int32_t height = s_app->screen_height();
    uint32_t first = selected > 5u ? selected - 5u : 0u;
    s_app->clear();
    s_app->draw_text(18, 33, "GAMEBOY / ROM LIBRARY");
    s_app->draw_text(18, 66, "Up/Down choose; Confirm loads");
    for (uint32_t index = first, row = 0;
         index < s_rom_count && row < 9u; ++index, ++row) {
        int32_t y = 122 + (int32_t)row * 42;
        char short_name[43];
        copy_name(short_name, sizeof(short_name), s_roms[index].name);
        if (index == selected) s_app->fill_rect(8, y - 5, 7, 28, true);
        s_app->draw_text(23, y, short_name);
    }
    s_app->draw_text(18, height - 98, "ROMs: /sd/ROMs/*.gb or *.gbc");
    s_app->draw_text(18, height - 61, "Back exits; Power/Home returns");
    s_app->present(true);
}

static bool sidecar_path(const rom_item_t *item, const char *suffix,
                         char *output, size_t capacity) {
    const char *base = item->path[0] ? item->path : "/sd/GameBoy/builtin.gb";
    size_t n = strlen(base), s = strlen(suffix);
    if (n + s + 1u > capacity) return false;
    memcpy(output, base, n);
    memcpy(output + n, suffix, s + 1u);
    return true;
}

static uint32_t wall_seconds(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec <= 0 ||
        (uint64_t)ts.tv_sec > UINT32_MAX) return 0u;
    return (uint32_t)ts.tv_sec;
}

/* Retain existing PBSV cartridge RAM/RTC format, so standalone save files
 * remain usable. Never mark RAM clean if a write fails. */
static bool save_cartridge(gbemu_t *emu, const rom_item_t *item) {
    if (!gbemu_persist_is_dirty(emu)) return true;
    size_t size = gbemu_get_persist_size(emu);
    char path[ROM_PATH_CAP + 16];
    if (!size || size > STATE_LIMIT ||
        !sidecar_path(item, ".sav", path, sizeof(path))) return false;
    uint8_t *buffer = (uint8_t *)malloc(size);
    if (!buffer) return false;
    bool ok = gbemu_export_persist(emu, buffer, size, wall_seconds()) &&
              s_storage->write_file_atomic(path, buffer, size);
    free(buffer);
    if (ok) gbemu_mark_persist_clean(emu);
    return ok;
}

static bool read_blob(const char *path, uint8_t **bytes, size_t *size) {
    *bytes = NULL;
    *size = 0;
    size_t actual = 0;
    if (!s_storage->read_file(path, NULL, 0, &actual) ||
        actual == 0 || actual > STATE_LIMIT) return false;
    uint8_t *buffer = (uint8_t *)malloc(actual);
    if (!buffer) return false;
    size_t read = 0;
    bool ok = s_storage->read_file(path, buffer, actual, &read) &&
              read == actual;
    if (!ok) { free(buffer); return false; }
    *bytes = buffer;
    *size = actual;
    return true;
}

static void restore_cartridge(gbemu_t *emu, const rom_item_t *item) {
    char path[ROM_PATH_CAP + 16];
    uint8_t *buffer = NULL;
    size_t size = 0;
    if (!sidecar_path(item, ".sav", path, sizeof(path)) ||
        !read_blob(path, &buffer, &size)) return;
    if (!gbemu_import_persist(emu, buffer, size, wall_seconds()))
        copy_name(s_notice, sizeof(s_notice), "Cartridge save ignored");
    free(buffer);
}

static bool quick_state(gbemu_t *emu, const rom_item_t *item, bool load) {
    char path[ROM_PATH_CAP + 16];
    if (!sidecar_path(item, ".state", path, sizeof(path))) return false;
    if (load) {
        uint8_t *bytes = NULL;
        size_t size = 0;
        if (!read_blob(path, &bytes, &size)) return false;
        bool ok = gbemu_load_state(emu, bytes, size);
        free(bytes);
        return ok;
    }
    size_t size = gbemu_get_state_size(emu);
    if (!size || size > STATE_LIMIT) return false;
    uint8_t *bytes = (uint8_t *)malloc(size);
    if (!bytes) return false;
    bool ok = gbemu_save_state(emu, bytes, size) &&
              s_storage->write_file_atomic(path, bytes, size);
    free(bytes);
    return ok;
}

/* Stream ROM in bounded pieces, rather than the firmware read_file bridge,
 * which materializes the entire file into a temporary Arduino String. */
static uint8_t *load_rom(const rom_item_t *item, size_t *size_out) {
    *size_out = 0;
    if (!item->path[0]) {
        *size_out = riscrte_gameboy_demo_size();
        return (uint8_t *)riscrte_gameboy_demo_data();
    }
    size_t size = 0;
    t5_storage_stream_t stream = s_storage->stream_open(item->path, &size);
    if (stream == T5_STORAGE_STREAM_INVALID) return NULL;
    uint8_t *buffer = NULL;
    if (size >= 0x150u && size <= GBEMU_MAX_ROM_BYTES && size == item->size)
        buffer = (uint8_t *)malloc(size);
    if (!buffer) { s_storage->stream_close(stream); return NULL; }
    size_t done = 0;
    while (done < size && !s_exit) {
        size_t chunk = size - done;
        if (chunk > 4096u) chunk = 4096u;
        size_t got = s_storage->stream_read(stream, buffer + done, chunk);
        if (!got || got > chunk) break;
        done += got;
        if ((done & 0x3fffu) == 0u || done == size) {
            t5_app_input_t input = {0};
            if (!s_app->poll(&input, 1u) || input.exit_requested) s_exit = true;
        }
    }
    s_storage->stream_close(stream);
    if (done != size || s_exit) { free(buffer); return NULL; }
    *size_out = size;
    return buffer;
}

static void process_touch(const t5_app_input_t *input) {
    if (!input->tapped) return;
    int32_t w = s_app->screen_width(), h = s_app->screen_height();
    int32_t x = input->touch_x, y = input->touch_y;
    if (y < 105) {
        if (x < w / 3) s_menu_requested = true;
        else if (x > (2 * w) / 3) s_save_requested = true;
        else s_load_requested = true;
        return;
    }
    if (y < h - 205) return;
    s_touch_expires = milliseconds();
    if (y < h - 105) {
        if (x < w / 4) s_touch_mask = GBEMU_INPUT_LEFT;
        else if (x < w / 2) s_touch_mask = GBEMU_INPUT_RIGHT;
        else if (x < (3 * w) / 4) s_touch_mask = GBEMU_INPUT_B;
        else s_touch_mask = GBEMU_INPUT_A;
    } else {
        if (x < w / 4) s_touch_mask = GBEMU_INPUT_UP;
        else if (x < w / 2) s_touch_mask = GBEMU_INPUT_DOWN;
        else if (x < (3 * w) / 4) s_touch_mask = GBEMU_INPUT_SELECT;
        else s_touch_mask = GBEMU_INPUT_START;
    }
}

static bool poll_game(uint32_t wait_ms) {
    t5_app_input_t input = {0};
    if (!s_app->poll(&input, wait_ms) || input.exit_requested) {
        s_exit = true;
        return false;
    }
    s_buttons = input.buttons;
    process_touch(&input);
    const uint32_t menu_combo = T5_APP_BUTTON_BACK | T5_APP_BUTTON_CONFIRM |
                                T5_APP_BUTTON_UP;
    if ((input.buttons & menu_combo) == menu_combo) {
        if (!s_menu_hold_started) s_menu_hold_started = milliseconds();
        else if (elapsed(milliseconds(), s_menu_hold_started, BUTTON_HOLD_MS))
            s_menu_requested = true;
    } else s_menu_hold_started = 0;
    return !s_menu_requested;
}

static uint8_t game_input_mask(void) {
    uint8_t mask = 0;
    if (s_buttons & T5_APP_BUTTON_CONFIRM) mask |= GBEMU_INPUT_A;
    if (s_buttons & T5_APP_BUTTON_BACK) mask |= GBEMU_INPUT_B;
    if (s_buttons & T5_APP_BUTTON_LEFT) mask |= GBEMU_INPUT_LEFT;
    if (s_buttons & T5_APP_BUTTON_RIGHT) mask |= GBEMU_INPUT_RIGHT;
    if (s_buttons & T5_APP_BUTTON_UP) mask |= GBEMU_INPUT_UP;
    if (s_buttons & T5_APP_BUTTON_DOWN) mask |= GBEMU_INPUT_DOWN;
    if ((s_buttons & (T5_APP_BUTTON_CONFIRM | T5_APP_BUTTON_RIGHT)) ==
        (T5_APP_BUTTON_CONFIRM | T5_APP_BUTTON_RIGHT)) mask |= GBEMU_INPUT_START;
    if ((s_buttons & (T5_APP_BUTTON_BACK | T5_APP_BUTTON_LEFT)) ==
        (T5_APP_BUTTON_BACK | T5_APP_BUTTON_LEFT)) mask |= GBEMU_INPUT_SELECT;
    if (!elapsed(milliseconds(), s_touch_expires, 160u)) mask |= s_touch_mask;
    return mask;
}

/* The existing RiscRTE app API has fill_rect but no pixel/framebuffer upload.
 * FAST_MONO makes every original source pixel a uniform 3x3 block. Draw each
 * black horizontal run as one rectangle; present only after the whole frame.
 * This is intentionally slow but does not require changing firmware. */
static bool display_frame(const uint8_t *pixels, uint32_t draw_count,
                          const char *title) {
    int32_t w = s_app->screen_width(), h = s_app->screen_height();
    int32_t x0 = (w - (int32_t)GBEMU_FRAME_WIDTH) / 2;
    int32_t y0 = (h - (int32_t)GBEMU_FRAME_HEIGHT) / 2 - 20;
    if (x0 < 0 || y0 < 115 || y0 + GBEMU_FRAME_HEIGHT > h - 205)
        return false;
    s_app->clear();
    s_app->draw_text(12, 26, "MENU          LOAD          SAVE");
    s_app->draw_text(12, 65, title);
    for (uint16_t y = 0; y < GBEMU_SOURCE_HEIGHT; ++y) {
        if ((y % 12u) == 0u && !poll_game(1u)) return false;
        const uint8_t *row = pixels +
            (size_t)y * GBEMU_SCALE * GBEMU_FRAME_PITCH_BYTES;
        uint16_t x = 0;
        while (x < GBEMU_SOURCE_WIDTH) {
            uint16_t bit = (uint16_t)(x * GBEMU_SCALE);
            bool black = (row[bit >> 3u] & (0x80u >> (bit & 7u))) == 0;
            uint16_t start = x++;
            while (x < GBEMU_SOURCE_WIDTH) {
                bit = (uint16_t)(x * GBEMU_SCALE);
                bool next_black =
                    (row[bit >> 3u] & (0x80u >> (bit & 7u))) == 0;
                if (next_black != black) break;
                ++x;
            }
            if (black)
                s_app->fill_rect(x0 + (int32_t)start * GBEMU_SCALE,
                                 y0 + (int32_t)y * GBEMU_SCALE,
                                 (int32_t)(x - start) * GBEMU_SCALE,
                                 GBEMU_SCALE, true);
        }
    }
    s_app->draw_text(12, h - 191, "LEFT      RIGHT       B       A");
    s_app->draw_text(12, h - 92, "UP       DOWN     SELECT    START");
    if (s_notice[0]) s_app->draw_text(12, h - 43, s_notice);
    s_app->present(draw_count == 0u || (draw_count % 8u) == 0u);
    return !s_exit && !s_menu_requested;
}

/* False requests termination of the entire ELF; true returns to ROM menu. */
static bool run_game(const rom_item_t *item) {
    size_t rom_size = 0;
    message_screen("GAMEBOY", "Loading cartridge...");
    uint8_t *rom = load_rom(item, &rom_size);
    if (!rom) {
        if (!s_exit) message_screen("ROM LOAD FAILED", item->name);
        return !s_exit;
    }
    audio_set_engine(AUDIO_ENGINE_MUTE);
    audio_init();
    gbemu_t *emu = gbemu_create();
    uint8_t *pixels = (uint8_t *)malloc(GBEMU_FRAMEBUFFER_SIZE);
    if (!emu || !pixels) {
        message_screen("OUT OF MEMORY", "Cannot allocate emulator buffers");
        if (emu) gbemu_destroy(emu);
        free(pixels);
        if (item->path[0]) free(rom);
        audio_deinit();
        return !s_exit;
    }
    gbemu_status_t status = gbemu_init(emu, rom, rom_size);
    if (status != GBEMU_STATUS_OK) {
        message_screen("CARTRIDGE REJECTED", gbemu_status_string(status));
        gbemu_destroy(emu);
        free(pixels);
        if (item->path[0]) free(rom);
        audio_deinit();
        return !s_exit;
    }
    restore_cartridge(emu, item);
    s_menu_requested = s_save_requested = s_load_requested = false;
    s_menu_hold_started = s_touch_expires = s_touch_mask = s_buttons = 0u;
    uint32_t last_paint = 0u, last_save = milliseconds(), drawings = 0u;
    bool first_frame = true;
    copy_name(s_notice, sizeof(s_notice), "Touch buttons below screen");
    while (!s_exit && !s_menu_requested && poll_game(16u)) {
        if (s_save_requested) {
            s_save_requested = false;
            bool ok = quick_state(emu, item, false);
            copy_name(s_notice, sizeof(s_notice),
                      ok ? "Quick state saved" : "Quick save failed");
        }
        if (s_load_requested) {
            s_load_requested = false;
            bool ok = quick_state(emu, item, true);
            copy_name(s_notice, sizeof(s_notice),
                      ok ? "Quick state loaded" : "No valid quick state");
        }
        uint32_t now = milliseconds();
        bool draw = first_frame || elapsed(now, last_paint, FRAME_REFRESH_MS);
        if (!gbemu_run_frame(emu, pixels, GBEMU_FRAMEBUFFER_SIZE,
                             game_input_mask(), !draw, NULL)) {
            copy_name(s_notice, sizeof(s_notice), gbemu_get_last_error_string(emu));
            break;
        }
        audio_service_frame();
        if (draw) {
            first_frame = false;
            if (!display_frame(pixels, drawings++, gbemu_get_rom_title(emu)))
                break;
            last_paint = milliseconds();
        }
        if (elapsed(milliseconds(), last_save, 60000u)) {
            if (!save_cartridge(emu, item))
                copy_name(s_notice, sizeof(s_notice), "Cartridge save failed");
            last_save = milliseconds();
        }
    }
    if (!save_cartridge(emu, item))
        message_screen("WARNING: SAVE FAILED", "Cartridge RAM not written to SD");
    gbemu_destroy(emu);
    free(pixels);
    if (item->path[0]) free(rom);
    audio_deinit();
    return !s_exit;
}

/* Exported dlsym entry point; returning lets RiscRTE release the UI session. */
__attribute__((visibility("default"))) void app_main(void) {
    s_app = t5_app_get_api(T5_APP_ABI_VERSION);
    s_storage = t5_storage_get_api(T5_STORAGE_API_VERSION);
    if (!HAS_API_FIELD(s_app, t5_app_api_v1, set_back_exits_app) ||
        !HAS_API_FIELD(s_storage, t5_storage_api_v1, stream_close) ||
        !HAS_API_FIELD(s_storage, t5_storage_api_v1, write_file_atomic) ||
        !s_app->screen_width || !s_app->screen_height || !s_app->clear ||
        !s_app->draw_text || !s_app->fill_rect || !s_app->present ||
        !s_app->poll || !s_app->millis || !s_app->dir_open ||
        !s_app->dir_next || !s_app->dir_close || !s_storage->read_file ||
        !s_storage->stream_open || !s_storage->stream_read) return;
    s_app->set_back_exits_app(false);
    discover_roms();
    s_exit = false;
    uint32_t selected = 0, last_buttons = 0;
    bool needs_redraw = true;
    while (!s_exit) {
        if (needs_redraw) { render_menu(selected); needs_redraw = false; }
        t5_app_input_t input = {0};
        if (!s_app->poll(&input, 25u) || input.exit_requested) break;
        uint32_t pressed = input.buttons & ~last_buttons;
        last_buttons = input.buttons;
        if (pressed & T5_APP_BUTTON_BACK) break;
        if (pressed & (T5_APP_BUTTON_UP | T5_APP_BUTTON_LEFT)) {
            selected = selected ? selected - 1u : s_rom_count - 1u;
            needs_redraw = true;
        }
        if (pressed & (T5_APP_BUTTON_DOWN | T5_APP_BUTTON_RIGHT)) {
            selected = (selected + 1u) % s_rom_count;
            needs_redraw = true;
        }
        if (input.tapped) {
            uint32_t first = selected > 5u ? selected - 5u : 0u;
            int32_t row = ((int32_t)input.touch_y - 117) / 42;
            if (input.touch_y >= 117 && row >= 0 && row < 9 &&
                first + (uint32_t)row < s_rom_count) {
                selected = first + (uint32_t)row;
                pressed |= T5_APP_BUTTON_CONFIRM;
            }
        }
        if (pressed & T5_APP_BUTTON_CONFIRM) {
            if (!run_game(&s_roms[selected])) break;
            discover_roms();
            if (selected >= s_rom_count) selected = 0;
            last_buttons = 0;
            needs_redraw = true;
        }
    }
    s_app->set_back_exits_app(true);
    s_app = NULL;
    s_storage = NULL;
}
