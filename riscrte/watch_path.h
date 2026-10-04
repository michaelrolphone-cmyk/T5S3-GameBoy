#pragma once
/* T-Watch launch decisions for the same Game Boy ELF. No SD paths. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WATCH_ROM_DIR "/bootfs/gameboy/roms"
#define WATCH_SAVE_DIR "/bootfs/gameboy/saves"
#define WATCH_ROM_MAX 32u
#define WATCH_NAME_MAX 64u

/* Matches RiscRTE-T-Watch-S3 PR #8 risc_input_navigation_api_v1 bits. */
#define WATCH_NAV_BACK (1u << 0)
#define WATCH_NAV_CONFIRM (1u << 1)
#define WATCH_NAV_LEFT (1u << 2)
#define WATCH_NAV_RIGHT (1u << 3)
#define WATCH_NAV_UP (1u << 4)
#define WATCH_NAV_DOWN (1u << 5)
#define WATCH_NAV_PAGE_BACK (1u << 6)
#define WATCH_NAV_PAGE_FORWARD (1u << 7)
#define WATCH_NAV_HOME (1u << 8)
#define WATCH_NAV_RICH (WATCH_NAV_CONFIRM | WATCH_NAV_LEFT | WATCH_NAV_RIGHT | \
                        WATCH_NAV_UP | WATCH_NAV_DOWN | WATCH_NAV_PAGE_BACK | \
                        WATCH_NAV_PAGE_FORWARD | WATCH_NAV_HOME)

/* GBEMU_INPUT_* values. Repeated so this file stays freestanding. */
#define WATCH_GB_A 0x01u
#define WATCH_GB_B 0x02u
#define WATCH_GB_SELECT 0x04u
#define WATCH_GB_START 0x08u
#define WATCH_GB_RIGHT 0x10u
#define WATCH_GB_LEFT 0x20u
#define WATCH_GB_UP 0x40u
#define WATCH_GB_DOWN 0x80u

#define WATCH_RGB565_BIT (1u << 4)

typedef struct {
    char name[WATCH_NAME_MAX];
} watch_rom_entry;

typedef struct {
    watch_rom_entry roms[WATCH_ROM_MAX];
    size_t count;
    bool truncated;
} watch_catalog;

typedef struct {
    uint32_t buttons;
    uint32_t pressed;
    uint8_t hid;
} watch_input;

typedef struct {
    int rich;
    int back_down;
    uint32_t back_since_ms;
    int hold_launch_sent;
    int hold_exit_sent;
} watch_picker_state;

typedef enum {
    WATCH_UI_NONE = 0,
    WATCH_UI_PREVIOUS,
    WATCH_UI_NEXT,
    WATCH_UI_LAUNCH,
    WATCH_UI_EXIT_APP,
    WATCH_UI_BACK_TO_PICKER
} watch_ui_action;

/* True only for the PR #8 panel: 240x240 with display.output RGB565. */
bool watch_display_is_twatch(uint32_t width, uint32_t height,
                             uint32_t supported_formats);

bool watch_rom_name_ok(const char *name);
bool watch_catalog_add(watch_catalog *catalog, const char *name);
void watch_catalog_sort(watch_catalog *catalog);
bool watch_join_dir(const char *dir, const char *name, char *out, size_t out_size);
bool watch_rom_path(const char *name, char *out, size_t out_size);
bool watch_save_path(const char *name, char *out, size_t out_size);

void watch_picker_reset(watch_picker_state *state);
/* Crown-only input.navigation publishes RISC_NAV_BACK and nothing else.
 * A short BACK edge moves to the next ROM. Holding BACK for 700ms launches.
 * Holding BACK for 2000ms leaves the app. Direction, Confirm, and Home bits
 * switch the picker to that pad instead of the crown hold gestures. */
watch_ui_action watch_picker_step(watch_picker_state *state, const watch_input *input,
                                  uint32_t now_ms);

/* HID mask is usb.hid.gamepad, usb.xinput.gamepad, and usb.hid.keyboard.
 * Rich input.navigation bits are OR-ed on. Crown-only BACK is not B. */
uint8_t watch_play_buttons(const watch_input *input, int rich_nav);
bool watch_play_leave(const watch_input *input, uint32_t back_held_ms, int rich_nav);
