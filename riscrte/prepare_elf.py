#!/usr/bin/env python3
"""Stage the *original* Paperboy sources with minimal ELF-only lifecycle changes.

The standalone firmware remains untouched. Every patch has an exact one-match
assertion: upstream changes must be reviewed instead of silently dropping behavior.
"""
from pathlib import Path
import argparse

ROOT = Path(__file__).resolve().parents[1]


def patch_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected exactly one source anchor, found {count}')
    return text.replace(old, new, 1)


def stage(destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    main = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    main = patch_once(main, '#include "touch_gt911.h"',
                      '#include "touch_gt911.h"\n#include "elf_lifecycle.h"', 'lifecycle header')
    # Whole-device shutdown is correct for standalone firmware but an ELF must
    # return control to its host. Keep the shutdown code out of the ELF entirely.
    main = patch_once(main, '[[noreturn]] void enter_power_off() {',
                      '#ifndef PAPERBOY_RISCRTE_ELF\n[[noreturn]] void enter_power_off() {',
                      'power-off guard start')
    main = patch_once(main, '\nvoid run_console(void *unused) {',
                      '\n#endif  // !PAPERBOY_RISCRTE_ELF\n\nvoid run_console(void *unused) {',
                      'power-off guard end')
    main = patch_once(main, '  while (true) {\n    battery_service();',
                      '  while (!paperboy_elf_exit_requested()) {\n    battery_service();',
                      'console lifetime')
    main = patch_once(main, '        enter_power_off();',
                      '        paperboy_elf_request_exit();\n        continue;',
                      'long-press exit')
    # run_console executes synchronously in the native-launch owner task.
    # Deleting that task would kill RiscRTE instead of returning to the launcher.
    main = patch_once(main,
                      '  (void)save_current_persist(true);\n  audio_deinit();\n  vTaskDelete(nullptr);\n}',
                      '  (void)save_current_persist(true);\n  audio_deinit();\n}',
                      'console return')
    main = patch_once(main, '  Serial.begin(115200);\n  delay(1500);',
                      '  // Serial belongs to RiscRTE and must not be reinitialized.',
                      'serial ownership')
    main = patch_once(main, '  (void)esp_register_shutdown_handler(on_shutdown);',
                      '  // An ELF must not register a shutdown callback pointing into unloadable code.',
                      'shutdown callback')
    main = patch_once(
        main,
        '''  attachInterrupt(
      digitalPinToInterrupt(t5s3_epd::kBootButton),
      on_boot_button_falling,
      FALLING);''',
        '''  attachInterrupt(
      digitalPinToInterrupt(t5s3_epd::kBootButton),
      on_boot_button_falling,
      FALLING);
  paperboy_elf_note_boot_interrupt_attached();''',
        'boot interrupt ownership')
    task_block = '''  const BaseType_t task_result = xTaskCreatePinnedToCore(
      run_console,
      "gameboy_console",
      14336,
      nullptr,
      2,
      nullptr,
      0);
  if (task_result != pdPASS) {
    audio_deinit();
    paperboy_storage_end();
    enter_idle("console task creation failed", "TASK ERROR", "CHECK INTERNAL RAM");
  }'''
    main = patch_once(main, task_block,
                      '  // The ELF launcher must regain control when the player exits.\n'
                      '  run_console(nullptr);', 'synchronous console')
    main += '''\n\n// ELF-only entry points. Original UI, emulator, audio, storage, touch and
// display sources remain in the module rather than being replaced by facades.
#ifdef PAPERBOY_RISCRTE_ELF
namespace {
volatile bool s_elf_exit_requested = false;
bool s_elf_boot_interrupt_attached = false;
}
void paperboy_elf_request_exit() { s_elf_exit_requested = true; }
bool paperboy_elf_exit_requested() { return s_elf_exit_requested; }
void paperboy_elf_note_boot_interrupt_attached() { s_elf_boot_interrupt_attached = true; }

extern "C" __attribute__((visibility("default"))) uint32_t app_hardware_takeover() {
  return T5_HARDWARE_TAKEOVER_DISPLAY;
}

using PaperboyInitFunction = void (*)();
extern "C" PaperboyInitFunction __app_init_array_start[];
extern "C" PaperboyInitFunction __app_init_array_end[];
extern "C" PaperboyInitFunction __app_ctors_start[];
extern "C" PaperboyInitFunction __app_ctors_end[];
extern "C" PaperboyInitFunction __app_fini_array_start[];
extern "C" PaperboyInitFunction __app_fini_array_end[];
extern "C" PaperboyInitFunction __app_dtors_start[];
extern "C" PaperboyInitFunction __app_dtors_end[];

extern "C" __attribute__((visibility("default"))) int app_module_init() {
  for (PaperboyInitFunction *fn = __app_init_array_start; fn != __app_init_array_end; ++fn) {
    if (*fn != nullptr) (*fn)();
  }
  // GCC's legacy .ctors ABI executes entries in reverse link order.
  for (PaperboyInitFunction *fn = __app_ctors_end; fn != __app_ctors_start;) {
    --fn;
    if (*fn != nullptr) (*fn)();
  }
  return 0;
}

extern "C" __attribute__((visibility("default"))) void app_module_fini() {
  for (PaperboyInitFunction *fn = __app_dtors_start; fn != __app_dtors_end; ++fn) {
    if (*fn != nullptr) (*fn)();
  }
  for (PaperboyInitFunction *fn = __app_fini_array_end; fn != __app_fini_array_start;) {
    --fn;
    if (*fn != nullptr) (*fn)();
  }
}

extern "C" __attribute__((visibility("default"))) void app_main() {
  s_elf_exit_requested = false;
  s_elf_boot_interrupt_attached = false;
  setup();  // Includes the complete, synchronously executed original console.
  if (s_elf_boot_interrupt_attached) {
    detachInterrupt(digitalPinToInterrupt(t5s3_epd::kBootButton));
    s_elf_boot_interrupt_attached = false;
  }
  audio_deinit();
  paperboy_storage_end();
  epd_video_shutdown();  // Joins scan task and deletes LCD DMA/bus handles.
  if (g_emu != nullptr) { gbemu_destroy(g_emu); g_emu = nullptr; }
  paperboy_storage_free_rom(g_sd_rom);
  release_quicksave();
  if (g_background != nullptr) { heap_caps_free(g_background); g_background = nullptr; }
  if (g_scene != nullptr) { heap_caps_free(g_scene); g_scene = nullptr; }
  if (g_game_frame != nullptr) { heap_caps_free(g_game_frame); g_game_frame = nullptr; }
  g_idle_reason = nullptr;
  g_storage_ready = false;
  g_current_rom_path[0] = '\\0';
  // The host still owns the I2C peripheral; do not call Wire.end().
}
#endif
'''
    (destination / 'main.cpp').write_text(main, encoding='utf-8')

    epd = (ROOT / 'src/epd_video.cpp').read_text(encoding='utf-8')
    original_wait = '''  for (uint8_t i = 0; i < 20 && g_scan_task != nullptr; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  wait_for_dma();'''
    replacement_wait = '''  // A returning ELF must not leave a scan task executing its unloaded text.
  // If a scan is stuck, fail closed instead of returning stale DMA callbacks
  // to the firmware's newly initialized display backend.
  for (uint16_t i = 0; i < 200 && g_scan_task != nullptr; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (g_scan_task != nullptr) {
    ESP_LOGE(kTag, "scan task did not stop; refusing unsafe ELF unload");
    abort();
  }
  vTaskDelay(1);  // Allow scan_task's final vTaskDelete(nullptr) to complete.
  wait_for_dma();'''
    epd = patch_once(epd, original_wait, replacement_wait, 'scan-task join')
    original_tail = '''  if (g_expander != nullptr) {
    g_expander->safeShutdownOutputs();
  }
}'''
    replacement_tail = '''  if (g_expander != nullptr) {
    g_expander->safeShutdownOutputs();
  }
#ifdef PAPERBOY_RISCRTE_ELF
  // The original standalone firmware never needs to release its LCD driver.
  // RiscRTE does: otherwise esp_lcd_new_i80_bus() fails during host resume.
  if (g_panel_io != nullptr) {
    const esp_err_t rc = esp_lcd_panel_io_del(g_panel_io);
    if (rc != ESP_OK) { ESP_LOGE(kTag, "panel IO release: %s", esp_err_to_name(rc)); abort(); }
    g_panel_io = nullptr;
  }
  if (g_i80_bus != nullptr) {
    const esp_err_t rc = esp_lcd_del_i80_bus(g_i80_bus);
    if (rc != ESP_OK) { ESP_LOGE(kTag, "i80 bus release: %s", esp_err_to_name(rc)); abort(); }
    g_i80_bus = nullptr;
  }
  release_allocations();
  g_expander = nullptr;
  g_dma_done = true;
  g_flip_req = false;
  g_drive_pending = false;
#endif
}'''
    epd = patch_once(epd, original_tail, replacement_tail, 'LCD/DMA teardown')
    (destination / 'epd_video.cpp').write_text(epd, encoding='utf-8')
    print(f'Staged complete GameBoy application and original display driver in {destination}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/riscrte/src')
    args = parser.parse_args()
    stage(args.output)
