#include "../src/builtin_demo_rom.h"

extern "C" const uint8_t *riscrte_gameboy_demo_data(void) {
    return builtin_demo_rom_data();
}
extern "C" size_t riscrte_gameboy_demo_size(void) {
    return builtin_demo_rom_size();
}
