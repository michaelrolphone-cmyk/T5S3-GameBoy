#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "T5CompatApi.h"
#include "gbemu.h"

#define ROM_DIR "/sd/Games"
#define MAX_ROM_BYTES GBEMU_MAX_ROM_BYTES
#define FRAME_INTERVAL_MS 120u

static const t5_app_api_prefix_v1 *g_app;
static const t5_storage_api_prefix_v1 *g_storage;

static bool ends_with_gb(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 3) return false;
    char a = name[n - 3], b = name[n - 2], c = name[n - 1];
    if (a == '.') {
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        return b == 'g' && c == 'b';
    }
    return false;
}

static bool make_rom_path(char *path, size_t cap, const char *name) {
    const size_t dir_len = sizeof(ROM_DIR) - 1u;
    const size_t name_len = strlen(name);
    if (dir_len + 1u + name_len + 1u > cap) return false;
    memcpy(path, ROM_DIR, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1u, name, name_len + 1u);
    return true;
}

static bool find_first_rom(char *path, size_t cap, size_t *size_out) {
    t5_app_dirent_t ent;
    if (!g_app->dir_open || !g_app->dir_next || !g_app->dir_close) return false;
    if (!g_app->dir_open(ROM_DIR)) return false;
    bool found = false;
    while (g_app->dir_next(&ent)) {
        if (!ent.is_directory && ent.size >= 0x150u && ent.size <= MAX_ROM_BYTES &&
            ends_with_gb(ent.name) && make_rom_path(path, cap, ent.name)) {
            *size_out = (size_t)ent.size;
            found = true;
            break;
        }
    }
    g_app->dir_close();
    return found;
}

static void message(const char *line1, const char *line2) {
    g_app->clear();
    if (line1) g_app->draw_text(18, 26, line1);
    if (line2) g_app->draw_text(18, 52, line2);
    g_app->present(true);
}

static uint8_t input_mask(const t5_app_input_t *in) {
    uint8_t mask = 0;
    if (in->buttons & T5_APP_BUTTON_LEFT) mask |= GBEMU_INPUT_LEFT;
    if (in->buttons & T5_APP_BUTTON_RIGHT) mask |= GBEMU_INPUT_RIGHT;
    if (in->buttons & T5_APP_BUTTON_UP) mask |= GBEMU_INPUT_UP;
    if (in->buttons & T5_APP_BUTTON_DOWN) mask |= GBEMU_INPUT_DOWN;
    if (in->buttons & T5_APP_BUTTON_CONFIRM) mask |= GBEMU_INPUT_A;
    if (in->tapped) {
        int32_t w = g_app->screen_width();
        int32_t h = g_app->screen_height();
        if (in->touch_y > (h * 3) / 4) {
            if (in->touch_x < w / 3) mask |= GBEMU_INPUT_B;
            else if (in->touch_x < (w * 2) / 3) mask |= GBEMU_INPUT_SELECT;
            else mask |= GBEMU_INPUT_START;
        }
    }
    return mask;
}

static void render_framebuffer(const uint8_t *fb) {
    const int32_t sw = g_app->screen_width();
    const int32_t sh = g_app->screen_height();
    const int32_t ox = (sw - (int32_t)GBEMU_FRAME_WIDTH) / 2;
    const int32_t oy = (sh - (int32_t)GBEMU_FRAME_HEIGHT) / 2;
    g_app->clear();

    for (uint16_t y = 0; y < GBEMU_FRAME_HEIGHT; ++y) {
        const uint8_t *row = fb + (size_t)y * GBEMU_FRAME_PITCH_BYTES;
        uint16_t x = 0;
        while (x < GBEMU_FRAME_WIDTH) {
            bool white = (row[x >> 3] & (uint8_t)(0x80u >> (x & 7u))) != 0;
            if (white) { ++x; continue; }
            uint16_t start = x++;
            while (x < GBEMU_FRAME_WIDTH &&
                   (row[x >> 3] & (uint8_t)(0x80u >> (x & 7u))) == 0) ++x;
            g_app->fill_rect(ox + start, oy + y, x - start, 1, true);
        }
    }
    g_app->present(false);
}

__attribute__((visibility("default"))) void app_main(void) {
    g_app = t5_app_get_api(T5_APP_ABI_VERSION);
    g_storage = t5_storage_get_api(T5_STORAGE_API_VERSION);
    if (!g_app || !g_storage || !g_app->poll || !g_app->clear || !g_app->present ||
        !g_app->draw_text || !g_app->fill_rect || !g_app->screen_width || !g_app->screen_height ||
        !g_storage->read_file) return;

    char rom_path[256];
    size_t rom_size = 0;
    if (!find_first_rom(rom_path, sizeof(rom_path), &rom_size)) {
        message("GameBoy ELF", "Put a .gb ROM in /Games on SD");
        t5_app_input_t in;
        while (g_app->poll(&in, 50) && !in.exit_requested) {}
        return;
    }

    uint8_t *rom = (uint8_t *)malloc(rom_size);
    uint8_t *frame = (uint8_t *)malloc(GBEMU_FRAMEBUFFER_SIZE);
    gbemu_t *emu = gbemu_create();
    if (!rom || !frame || !emu) {
        message("GameBoy ELF", "Out of memory");
        goto done;
    }

    size_t got = 0;
    if (!g_storage->read_file(rom_path, rom, rom_size, &got) || got != rom_size) {
        message("GameBoy ELF", "ROM read failed");
        goto done;
    }
    if (gbemu_init(emu, rom, rom_size) != GBEMU_STATUS_OK) {
        message("GameBoy ELF", gbemu_status_string(gbemu_get_status(emu)));
        goto done;
    }

    g_app->clear();
    g_app->draw_text(8, 8, "GameBoy ELF - Confirm=A; bottom touch B/Select/Start");
    g_app->present(true);

    uint32_t last_present = 0;
    t5_app_input_t in;
    memset(&in, 0, sizeof(in));
    while (g_app->poll(&in, 12) && !in.exit_requested) {
        uint32_t now = g_app->millis ? g_app->millis() : 0;
        bool render = (uint32_t)(now - last_present) >= FRAME_INTERVAL_MS;
        gbemu_frame_stats_t stats;
        if (!gbemu_run_frame(emu, frame, GBEMU_FRAMEBUFFER_SIZE, input_mask(&in), !render, &stats)) break;
        if (render) {
            render_framebuffer(frame);
            last_present = now;
        }
    }

done:
    if (emu) gbemu_destroy(emu);
    free(frame);
    free(rom);
}
