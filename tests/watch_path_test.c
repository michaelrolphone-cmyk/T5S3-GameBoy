#include "../riscrte/watch_path.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(int cond, const char *message) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", message);
        failures++;
    }
}

static void test_display(void) {
    expect(watch_display_is_twatch(240, 240, WATCH_RGB565_BIT), "240 rgb565 is the watch");
    expect(!watch_display_is_twatch(960, 540, 1u), "paper mono is not the watch");
    expect(!watch_display_is_twatch(240, 240, 1u), "240 without rgb565 is not the watch");
    expect(!watch_display_is_twatch(241, 240, WATCH_RGB565_BIT), "width must be 240");
}

static void test_catalog(void) {
    watch_catalog catalog;
    char path[192];
    memset(&catalog, 0, sizeof(catalog));
    expect(!watch_catalog_add(&catalog, "notes.txt"), "reject non-rom");
    expect(!watch_catalog_add(&catalog, "../Zelda.gb"), "reject slash");
    expect(!watch_catalog_add(&catalog, "/sd/Zelda.gb"), "reject sd path as a name");
    expect(watch_catalog_add(&catalog, "Zelda.GB"), "accept gb");
    expect(watch_catalog_add(&catalog, "a.gbc"), "accept gbc");
    expect(!watch_rom_path("Zelda.GB", path, sizeof(path)) || strstr(path, "/sd") != NULL ||
           strcmp(path, "/bootfs/gameboy/roms/Zelda.GB") == 0, "rom path is bootfs");
    expect(watch_rom_path("Zelda.GB", path, sizeof(path)), "rom path ok");
    expect(strcmp(path, "/bootfs/gameboy/roms/Zelda.GB") == 0, path);
    expect(watch_save_path("Zelda.GB", path, sizeof(path)), "save path ok");
    expect(strcmp(path, "/bootfs/gameboy/saves/Zelda.pbsv") == 0, path);
    expect(strstr(path, "/sd") == NULL, "save is not on sd");
    watch_catalog_sort(&catalog);
    expect(strcmp(catalog.roms[0].name, "a.gbc") == 0, "sort");
    expect(strcmp(catalog.roms[1].name, "Zelda.GB") == 0, "sort second");
}

static void test_picker_crown(void) {
    watch_picker_state state;
    watch_input input;
    watch_picker_reset(&state);
    memset(&input, 0, sizeof(input));
    input.buttons = WATCH_NAV_BACK;
    input.pressed = WATCH_NAV_BACK;
    expect(watch_picker_step(&state, &input, 1000) == WATCH_UI_NONE, "press does not move yet");
    input.pressed = 0;
    expect(watch_picker_step(&state, &input, 1100) == WATCH_UI_NONE, "still held");
    input.buttons = 0;
    expect(watch_picker_step(&state, &input, 1200) == WATCH_UI_NEXT, "short crown moves");
    input.buttons = WATCH_NAV_BACK;
    input.pressed = WATCH_NAV_BACK;
    expect(watch_picker_step(&state, &input, 2000) == WATCH_UI_NONE, "second press");
    input.pressed = 0;
    input.buttons = 0;
    expect(watch_picker_step(&state, &input, 2800) == WATCH_UI_LAUNCH, "700ms crown launches");
    input.buttons = WATCH_NAV_BACK;
    input.pressed = WATCH_NAV_BACK;
    expect(watch_picker_step(&state, &input, 3000) == WATCH_UI_NONE, "third press");
    input.pressed = 0;
    expect(watch_picker_step(&state, &input, 5000) == WATCH_UI_EXIT_APP, "2s crown leaves the app");
}

static void test_picker_pad(void) {
    watch_picker_state state;
    watch_input input;
    watch_picker_reset(&state);
    memset(&input, 0, sizeof(input));
    input.pressed = WATCH_NAV_DOWN;
    input.buttons = WATCH_NAV_DOWN;
    expect(watch_picker_step(&state, &input, 10) == WATCH_UI_NEXT, "down moves");
    input.pressed = WATCH_NAV_CONFIRM;
    input.buttons = WATCH_NAV_CONFIRM;
    expect(watch_picker_step(&state, &input, 20) == WATCH_UI_LAUNCH, "confirm launches");
    input.pressed = WATCH_NAV_BACK;
    input.buttons = WATCH_NAV_BACK;
    expect(watch_picker_step(&state, &input, 30) == WATCH_UI_EXIT_APP, "rich back leaves picker");
    expect(state.rich == 1, "pad is rich navigation");
}

static void test_play_input(void) {
    watch_input input;
    memset(&input, 0, sizeof(input));
    input.hid = WATCH_GB_A;
    input.buttons = WATCH_NAV_BACK;
    expect(watch_play_buttons(&input, 0) == WATCH_GB_A, "crown-only does not invent B");
    expect(watch_play_leave(&input, 800, 0), "crown hold leaves play");
    expect(!watch_play_leave(&input, 100, 0), "short crown does not leave play");
    input.buttons = WATCH_NAV_UP | WATCH_NAV_CONFIRM | WATCH_NAV_BACK;
    expect(watch_play_buttons(&input, 1) == (WATCH_GB_A | WATCH_GB_UP | WATCH_GB_B | WATCH_GB_A),
           "rich nav maps onto the hid mask");
    input.pressed = WATCH_NAV_HOME;
    expect(watch_play_leave(&input, 0, 1), "home leaves play");
    input.pressed = 0;
    expect(!watch_play_leave(&input, 5000, 1), "rich back is B, not leave");
}

int main(void) {
    test_display();
    test_catalog();
    test_picker_crown();
    test_picker_pad();
    test_play_input();
    if (failures) {
        fprintf(stderr, "%d watch path checks failed\n", failures);
        return 1;
    }
    puts("watch path checks passed");
    return 0;
}
