#include "watch_path.h"
#include <string.h>

bool watch_display_is_twatch(uint32_t width, uint32_t height,
                             uint32_t supported_formats) {
    return width == 240u && height == 240u &&
           (supported_formats & WATCH_RGB565_BIT) != 0u;
}

static char lower_ascii(char value) {
    return (value >= 'A' && value <= 'Z') ? (char)(value - 'A' + 'a') : value;
}

static int ends_with_ci(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    size_t i;
    if (name_len < suffix_len) return 0;
    for (i = 0; i < suffix_len; ++i) {
        if (lower_ascii(name[name_len - suffix_len + i]) != suffix[i]) return 0;
    }
    return 1;
}

bool watch_rom_name_ok(const char *name) {
    size_t i;
    size_t length;
    if (!name || !name[0]) return false;
    length = strlen(name);
    if (length >= WATCH_NAME_MAX) return false;
    if (!(ends_with_ci(name, ".gb") || ends_with_ci(name, ".gbc"))) return false;
    for (i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)name[i];
        if (value < 0x20u || value == '/' || value == '\\' || value == ':') return false;
    }
    if (name[0] == '.' ) return false;
    return true;
}

bool watch_catalog_add(watch_catalog *catalog, const char *name) {
    if (!catalog || !watch_rom_name_ok(name)) return false;
    if (catalog->count >= WATCH_ROM_MAX) {
        catalog->truncated = true;
        return false;
    }
    memcpy(catalog->roms[catalog->count].name, name, strlen(name) + 1u);
    catalog->count++;
    return true;
}

static int name_less(const char *left, const char *right) {
    while (*left && *right) {
        char a = lower_ascii(*left++);
        char b = lower_ascii(*right++);
        if (a != b) return a < b;
    }
    return *left == '\0' && *right != '\0';
}

void watch_catalog_sort(watch_catalog *catalog) {
    size_t i, j;
    if (!catalog) return;
    for (i = 1; i < catalog->count; ++i) {
        watch_rom_entry key = catalog->roms[i];
        j = i;
        while (j > 0 && name_less(key.name, catalog->roms[j - 1].name)) {
            catalog->roms[j] = catalog->roms[j - 1];
            --j;
        }
        catalog->roms[j] = key;
    }
}

bool watch_join_dir(const char *dir, const char *name, char *out, size_t out_size) {
    size_t dir_len;
    size_t name_len;
    if (!dir || !name || !out || out_size < 2u) return false;
    if (strncmp(dir, "/bootfs/", 8) != 0) return false;
    if (strstr(dir, "/sd") != NULL) return false;
    dir_len = strlen(dir);
    name_len = strlen(name);
    if (dir_len + 1u + name_len + 1u > out_size) return false;
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1u, name, name_len + 1u);
    return true;
}

bool watch_rom_path(const char *name, char *out, size_t out_size) {
    if (!watch_rom_name_ok(name)) return false;
    return watch_join_dir(WATCH_ROM_DIR, name, out, out_size);
}

bool watch_save_path(const char *name, char *out, size_t out_size) {
    char stem[WATCH_NAME_MAX];
    size_t length;
    size_t stem_len;
    char *dot;
    if (!watch_rom_name_ok(name)) return false;
    length = strlen(name);
    memcpy(stem, name, length + 1u);
    dot = strrchr(stem, '.');
    if (!dot) return false;
    *dot = '\0';
    stem_len = strlen(stem);
    if (stem_len + 5u >= WATCH_NAME_MAX) return false;
    memcpy(stem + stem_len, ".pbsv", 6u);
    return watch_join_dir(WATCH_SAVE_DIR, stem, out, out_size);
}

void watch_picker_reset(watch_picker_state *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

static int note_rich(watch_picker_state *state, const watch_input *input) {
    if ((input->buttons | input->pressed) & WATCH_NAV_RICH) state->rich = 1;
    return state->rich;
}

watch_ui_action watch_picker_step(watch_picker_state *state, const watch_input *input,
                                  uint32_t now_ms) {
    uint32_t held;
    if (!state || !input) return WATCH_UI_NONE;
    if (note_rich(state, input)) {
        if (input->pressed & WATCH_NAV_HOME) return WATCH_UI_EXIT_APP;
        if (input->pressed & WATCH_NAV_UP) return WATCH_UI_PREVIOUS;
        if (input->pressed & WATCH_NAV_DOWN) return WATCH_UI_NEXT;
        if (input->pressed & WATCH_NAV_CONFIRM) return WATCH_UI_LAUNCH;
        if (input->pressed & WATCH_NAV_BACK) return WATCH_UI_EXIT_APP;
        return WATCH_UI_NONE;
    }
    if (input->buttons & WATCH_NAV_BACK) {
        if (!state->back_down) {
            state->back_down = 1;
            state->back_since_ms = now_ms;
            state->hold_launch_sent = 0;
            state->hold_exit_sent = 0;
        }
        held = now_ms - state->back_since_ms;
        if (!state->hold_exit_sent && held >= 2000u) {
            state->hold_exit_sent = 1;
            return WATCH_UI_EXIT_APP;
        }
        return WATCH_UI_NONE;
    }
    if (!state->back_down) return WATCH_UI_NONE;
    held = now_ms - state->back_since_ms;
    state->back_down = 0;
    if (state->hold_exit_sent) return WATCH_UI_NONE;
    if (held >= 700u) return WATCH_UI_LAUNCH;
    return WATCH_UI_NEXT;
}

uint8_t watch_play_buttons(const watch_input *input, int rich_nav) {
    uint8_t buttons;
    uint32_t nav;
    if (!input) return 0;
    buttons = input->hid;
    if (!rich_nav) return buttons;
    nav = input->buttons;
    if (nav & WATCH_NAV_UP) buttons |= WATCH_GB_UP;
    if (nav & WATCH_NAV_DOWN) buttons |= WATCH_GB_DOWN;
    if (nav & WATCH_NAV_LEFT) buttons |= WATCH_GB_LEFT;
    if (nav & WATCH_NAV_RIGHT) buttons |= WATCH_GB_RIGHT;
    if (nav & WATCH_NAV_CONFIRM) buttons |= WATCH_GB_A;
    if (nav & WATCH_NAV_BACK) buttons |= WATCH_GB_B;
    if (nav & WATCH_NAV_PAGE_FORWARD) buttons |= WATCH_GB_START;
    if (nav & WATCH_NAV_PAGE_BACK) buttons |= WATCH_GB_SELECT;
    return buttons;
}

bool watch_play_leave(const watch_input *input, uint32_t back_held_ms, int rich_nav) {
    if (!input) return false;
    if (input->pressed & WATCH_NAV_HOME) return true;
    if (rich_nav) return false;
    return (input->buttons & WATCH_NAV_BACK) && back_held_ms >= 800u;
}
