#!/usr/bin/env python3
"""Catch accidental regressions to a stripped-down compatibility emulator."""
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'riscrte'))
from prepare_elf import stage

with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp)
    stage(target)
    original = (ROOT / 'src/main.cpp').read_text()
    staged = (target / 'main.cpp').read_text()
    epd_original = (ROOT / 'src/epd_video.cpp').read_text()
    epd_staged = (target / 'epd_video.cpp').read_text()
    for symbol in ('paperboy_storage_begin()', 'paperboy_storage_read_config(',
                   'paperboy_storage_load_rom(', 'launch_sd_rom(', 'save_current_persist(',
                   'gbemu_run_frame(', 'audio_init()', 'audio_service_frame()',
                   'touch_read(', 'snes_mini_controller_buttons()',
                   'snes_mini_controller_take_actions()', 'night_light_init()',
                   'night_light_set_brightness(', 'compose_scene(', 'rotate_game_to_panel(',
                   'refresh_current_page(', 'battery_read_status('):
        assert symbol in original and symbol in staged, symbol
    assert 'run_console(nullptr);' in staged
    assert 'paperboy_elf_console_task' in staged
    assert 'xTaskCreatePinnedToCore(' in staged
    assert '32768' in staged
    # RiscRTE's loopTask services USB at priority 1. A priority-2 console can
    # starve reception whenever emulation uses the whole frame budget.
    assert '1, // Same priority as the RiscRTE owner' in staged
    assert 'paperboy_storage_bind_host();' in staged
    assert '(void)paperboy_storage_begin();' in staged
    assert 'paperboy_storage_owner_wait();' in staged
    assert 'paperboy_storage_owner_note_console_done();' in staged
    assert staged.find('paperboy_storage_bind_host();') < staged.find('(void)paperboy_storage_begin();') < staged.find('xTaskCreatePinnedToCore(')
    app_main = staged.split('void app_main()', 1)[1]
    create = app_main.index('xTaskCreatePinnedToCore(')
    hid_begin = app_main.index('paperboy_usb_owner_begin();')
    worker_start = app_main.index('xTaskNotifyGive(console_task);')
    display_wait = app_main.index('ulTaskNotifyTake(pdTRUE, portMAX_DELAY)')
    assert create < display_wait < hid_begin < worker_start
    assert 'if (s_elf_display_bus_ready) paperboy_usb_owner_begin();' in app_main
    assert 'bool paperboy_elf_prepare_display_bus() { return init_panel_bus(); }' in epd_staged
    assert 'ulTaskNotifyTake(pdTRUE, portMAX_DELAY)' in staged
    assert 'while (!paperboy_elf_exit_requested()) {' in staged
    assert 'paperboy_elf_request_exit();' in staged
    assert 'app_hardware_takeover()' in staged
    assert 'void app_main()' in staged
    assert app_main.index('paperboy_storage_owner_wait();') < app_main.index('night_light_shutdown();') < app_main.index('epd_video_shutdown();')
    assert app_main.index('paperboy_storage_owner_wait();') < app_main.index('paperboy_usb_owner_end();') < app_main.index('night_light_shutdown();')
    assert 'int app_module_init()' in staged
    assert 'void app_module_fini()' in staged
    assert 'if (s_elf_boot_interrupt_attached)' in staged
    assert 'paperboy_elf_note_boot_interrupt_attached();' in staged
    assert 'esp_deep_sleep_start();' in original
    assert '#ifndef PAPERBOY_RISCRTE_ELF' in staged
    # GPIO46 is active-low LoRa CS, not an unconnected dummy LCD pin.
    # Both standalone and staged ELF must keep it inactive throughout scan.
    for phase in ('idle', 'cmd', 'dummy', 'data'):
        setting = f'panel_config.dc_levels.dc_{phase}_level = 1;'
        assert setting in epd_original and setting in epd_staged, phase
    assert 'esp_lcd_panel_io_tx_color' in epd_staged
    assert 'esp_lcd_panel_io_del(g_panel_io)' in epd_staged
    assert 'esp_lcd_del_i80_bus(g_i80_bus)' in epd_staged
    assert 'release_allocations();' in epd_staged
    assert 'kVideoDrivePasses = 3' in epd_staged
    assert original == (ROOT / 'src/main.cpp').read_text()
    assert epd_original == (ROOT / 'src/epd_video.cpp').read_text()
print('Faithful GameBoy ELF source staging and hardware-release checks passed')

# Execute the actual input and rendering decisions with a counted display.
# Controller changes must not invalidate the full scene, even during turbo;
# touch feedback and menu/shortcut routing must still work.
import subprocess
with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp)
    stage(target)
    staged = (target / 'main.cpp').read_text()
    input_block = staged.split('    // Poll on every page', 1)[1].split('    const bool touch_down', 1)[0]
    input_block = input_block[input_block.index('    const uint8_t controller_buttons'):]
    transition_block = '    if (next_page != page) {' + staged.split('    if (next_page != page) {', 1)[1].split('    bool pca_button_pressed', 1)[0]
    render_block = staged.split('        uint8_t *backbuffer = epd_video_get_backbuffer();', 1)[1].split('      pace_game_frame', 1)[0]
    render_block = '        uint8_t *backbuffer = epd_video_get_backbuffer();' + render_block.rsplit('}', 1)[0]
    refresh_functions = staged.split('void wait_epd_idle()', 1)[1].split('void present_shutdown_page()', 1)[0]
    refresh_functions = 'void wait_epd_idle()' + refresh_functions
    refresh_constants = '\n'.join(line for line in staged.splitlines() if line.startswith(
        ('constexpr uint8_t kClearWhiteFrames', 'constexpr uint8_t kClearBlackFrames',
         'constexpr uint32_t kBootDebounceMs')))
    harness = (ROOT / 'tests/controller_frame_harness.cpp').read_text()
    source = target / 'controller_frame.cpp'
    source.write_text(harness.replace('// FRAME_INPUT', input_block)
                      .replace('// REFRESH_CONSTANTS', refresh_constants)
                      .replace('// REFRESH_FUNCTIONS', refresh_functions)
                      .replace('// FRAME_PAGE_TRANSITION', transition_block)
                      .replace('// FRAME_RENDER', render_block))
    binary = target / 'controller_frame'
    subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'src'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('1,200 controller frames: no incidental full-screen repaints; touch, shortcuts and hardware-equivalent redraw: PASS')

# Execute the real frame pacer with an overdue emulated frame. The ELF must
# give blocked owner/USB work and idle tasks an opportunity to run even when
# no frame time remains; standalone has its own higher-priority USB tasks.
import subprocess
with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp)
    stage(target)
    staged = (target / 'main.cpp').read_text()
    constants = original[original.index('constexpr uint32_t kDmgClockHz'):]
    constants = constants[:constants.index(';', constants.index('constexpr int64_t kGameFramePeriodCeilingUs')) + 1]
    frame = original[original.index('struct GameFramePacer {'):].split('};', 1)[0] + '};'
    for name, code, must_yield in [('elf', staged, True), ('standalone', original, False)]:
        functions = code[code.index('void reset_game_frame_pacer('):code.index('void compose_scene(')]
        source = target / f'{name}_pacer.cpp'
        source.write_text('''#include <cstdint>
static int64_t now = 100000;
static unsigned delays;
static int64_t esp_timer_get_time() { return now; }
static void vTaskDelay(unsigned ticks) { delays += ticks; now += ticks * 1000; }
static void delayMicroseconds(unsigned us) { now += us; }
''' + constants + frame + functions + '\nint main() { GameFramePacer p{}; pace_game_frame(p); return ' +
                          ('delays == 0' if must_yield else 'delays != 0') + '; }\n')
        binary = target / f'{name}_pacer'
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
print('Overdue ELF frames yield; standalone frame pacing remains unchanged: PASS')

# Execute the staged worker itself against a deterministic scheduler shim.
# A provider can finish after the old 15-second timeout, with or without a HID
# device. The worker must still be alive when its owner sends the start signal.
with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp)
    stage(target)
    staged = (target / 'main.cpp').read_text()
    worker = staged.split('extern "C" void paperboy_elf_console_task', 1)[1]
    worker = 'extern "C" void paperboy_elf_console_task' + worker.split(
        'extern "C" __attribute__((visibility("default"))) void app_main()', 1)[0]
    harness = (ROOT / 'tests/elf_start_gate_harness.cpp').read_text()
    source = target / 'worker_test.cpp'
    source.write_text(harness.replace('// STAGED_WORKER', worker))
    binary = target / 'worker_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)

# Execute the actual staged LCD setup: the shared SPI device must be
# deselected before the first display transfer, while USB ELFs load from SD.
with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp)
    stage(target)
    staged = (target / 'epd_video.cpp').read_text()
    bus = 'bool init_panel_bus() {' + staged.split('bool init_panel_bus() {', 1)[1].split('\nvoid row_control_start()', 1)[0]
    source = target / 'display_test.cpp'
    source.write_text((ROOT / 'tests/elf_display_bus_harness.cpp').read_text().replace('// STAGED_BUS', bus))
    binary = target / 'display_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('Staged LCD bus keeps LoRa CS high before provider SD reads: PASS')

# ELF telemetry must bind without calling the standalone charger/OTG writers.
with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp)
    battery = (ROOT / 'src/battery_power.cpp').read_text()
    functions = []
    for name in ('bool configure_charger()', 'bool start_host_boost()', 'void battery_service()'):
        start = battery.index(name + ' {')
        end = battery.index('{', start) + 1
        depth = 1
        while depth:
            depth += (battery[end] == '{') - (battery[end] == '}')
            end += 1
        functions.append(battery[start:end])
    source = target / 'power_test.cpp'
    source.write_text((ROOT / 'tests/elf_power_harness.cpp').read_text().replace('// ELF_POWER_FUNCTIONS', '\n'.join(functions)))
    binary = target / 'power_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('ELF charger telemetry leaves USB provider power control intact: PASS')
