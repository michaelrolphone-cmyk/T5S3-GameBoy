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
        '''  attachInterrupt(\n      digitalPinToInterrupt(t5s3_epd::kBootButton),\n      on_boot_button_falling,\n      FALLING);''',
        '''  attachInterrupt(\n      digitalPinToInterrupt(t5s3_epd::kBootButton),\n      on_boot_button_falling,\n      FALLING);\n  paperboy_elf_note_boot_interrupt_attached();''',
        'boot interrupt ownership')
    task_block = '''  const BaseType_t task_result = xTaskCreatePinnedToCore(\n      run_console,\n      "gameboy_console",\n      14336,\n      nullptr,\n      2,\n      nullptr,\n      0);\n  if (task_result != pdPASS) {\n    audio_deinit();\n    paperboy_storage_end();\n    enter_idle("console task creation failed", "TASK ERROR", "CHECK INTERNAL RAM");\n  }'''
    main = patch_once(main, task_block,
                      '  // Console work stays on the dedicated ELF worker created by app_main.\n'
                      '  run_console(nullptr);', 'synchronous console')
    main += '''\n\n#ifdef PAPERBOY_RISCRTE_ELF\nnamespace {\nvolatile bool s_elf_exit_requested = false;\nbool s_elf_boot_interrupt_attached = false;\nTaskHandle_t s_elf_owner_task = nullptr;\n}\nvoid paperboy_elf_request_exit() { s_elf_exit_requested = true; }\nbool paperboy_elf_exit_requested() { return s_elf_exit_requested; }\nvoid paperboy_elf_note_boot_interrupt_attached() { s_elf_boot_interrupt_attached = true; }\n\nextern "C" __attribute__((visibility("default"))) uint32_t app_hardware_takeover() {\n  return T5_HARDWARE_TAKEOVER_DISPLAY;\n}\n\nusing PaperboyInitFunction = void (*)();\nextern "C" PaperboyInitFunction __app_init_array_start[];\nextern "C" PaperboyInitFunction __app_init_array_end[];\nextern "C" PaperboyInitFunction __app_ctors_start[];\nextern "C" PaperboyInitFunction __app_ctors_end[];\nextern "C" PaperboyInitFunction __app_fini_array_start[];\nextern "C" PaperboyInitFunction __app_fini_array_end[];\nextern "C" PaperboyInitFunction __app_dtors_start[];\nextern "C" PaperboyInitFunction __app_dtors_end[];\n\nextern "C" __attribute__((visibility("default"))) int app_module_init() {\n  for (PaperboyInitFunction *fn = __app_init_array_start; fn != __app_init_array_end; ++fn) {\n    if (*fn != nullptr) (*fn)();\n  }\n  for (PaperboyInitFunction *fn = __app_ctors_end; fn != __app_ctors_start;) {\n    --fn;\n    if (*fn != nullptr) (*fn)();\n  }\n  return 0;\n}\n\nextern "C" __attribute__((visibility("default"))) void app_module_fini() {\n  for (PaperboyInitFunction *fn = __app_dtors_start; fn != __app_dtors_end; ++fn) {\n    if (*fn != nullptr) (*fn)();\n  }\n  for (PaperboyInitFunction *fn = __app_fini_array_end; fn != __app_fini_array_start;) {\n    --fn;\n    if (*fn != nullptr) (*fn)();\n  }\n}\n\nextern "C" void paperboy_elf_console_task(void *unused) {\n  (void)unused;\n  setup();\n  TaskHandle_t owner = s_elf_owner_task;\n  s_elf_owner_task = nullptr;\n  paperboy_storage_owner_note_console_done();\n  if (owner != nullptr) {\n    xTaskNotifyGive(owner);\n  }\n  vTaskDelete(nullptr);\n}\n\nextern "C" __attribute__((visibility("default"))) void app_main() {\n  s_elf_exit_requested = false;\n  s_elf_boot_interrupt_attached = false;\n  paperboy_storage_bind_host();\n  (void)paperboy_storage_begin();\n  s_elf_owner_task = xTaskGetCurrentTaskHandle();\n  TaskHandle_t console_task = nullptr;\n  const BaseType_t task_result = xTaskCreatePinnedToCore(\n      paperboy_elf_console_task,\n      "gameboy_console",\n      32768,\n      nullptr,\n      2,\n      &console_task,\n      0);\n  if (task_result != pdPASS) {\n    s_elf_owner_task = nullptr;\n    ESP_LOGE(kTag, "ELF console task creation failed");\n  } else {\n    paperboy_storage_owner_wait();\n  }\n  if (s_elf_boot_interrupt_attached) {\n    detachInterrupt(digitalPinToInterrupt(t5s3_epd::kBootButton));\n    s_elf_boot_interrupt_attached = false;\n  }\n  night_light_shutdown();\n  audio_deinit();\n  paperboy_storage_end();\n  epd_video_shutdown();\n  if (g_emu != nullptr) { gbemu_destroy(g_emu); g_emu = nullptr; }\n  paperboy_storage_free_rom(g_sd_rom);\n  release_quicksave();\n  if (g_background != nullptr) { heap_caps_free(g_background); g_background = nullptr; }\n  if (g_scene != nullptr) { heap_caps_free(g_scene); g_scene = nullptr; }\n  if (g_game_frame != nullptr) { heap_caps_free(g_game_frame); g_game_frame = nullptr; }\n  g_idle_reason = nullptr;\n  g_storage_ready = false;\n  g_current_rom_path[0] = '\\0';\n}\n#endif\n'''
    main = patch_once(
        main,
        'extern "C" void paperboy_elf_console_task(void *unused) {\n  (void)unused;\n  setup();',
        'extern "C" void paperboy_elf_console_task(void *unused) {\n'
        '  (void)unused;\n'
        '  const uint32_t start_signal = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));\n'
        '  if (start_signal != 0) {\n'
        '    setup();\n'
        '  } else {\n'
        '    ESP_LOGE(kTag, "ELF console start timed out");\n'
        '  }',
        'console waits for optional providers')
    main = patch_once(
        main,
        '  } else {\n    paperboy_storage_owner_wait();\n  }\n'
        '  if (s_elf_boot_interrupt_attached)',
        '  } else {\n'
        '    // Reserve the critical worker stack before loading optional HID providers.\n'
        '    paperboy_usb_owner_begin();\n'
        '    xTaskNotifyGive(console_task);\n'
        '    paperboy_storage_owner_wait();\n'
        '  }\n'
        '  paperboy_usb_owner_end();\n'
        '  if (s_elf_boot_interrupt_attached)',
        'HID starts after worker allocation')
    (destination / 'main.cpp').write_text(main, encoding='utf-8')

    epd = (ROOT / 'src/epd_video.cpp').read_text(encoding='utf-8')
    original_wait = '''  for (uint8_t i = 0; i < 20 && g_scan_task != nullptr; ++i) {\n    vTaskDelay(pdMS_TO_TICKS(10));\n  }\n\n  wait_for_dma();'''
    replacement_wait = '''  for (uint16_t i = 0; i < 200 && g_scan_task != nullptr; ++i) {\n    vTaskDelay(pdMS_TO_TICKS(10));\n  }\n  if (g_scan_task != nullptr) {\n    ESP_LOGE(kTag, "scan task did not stop; refusing unsafe ELF unload");\n    abort();\n  }\n  vTaskDelay(1);\n  wait_for_dma();'''
    epd = patch_once(epd, original_wait, replacement_wait, 'scan-task join')
    original_tail = '''  if (g_expander != nullptr) {\n    g_expander->safeShutdownOutputs();\n  }\n}'''
    replacement_tail = '''  if (g_expander != nullptr) {\n    g_expander->safeShutdownOutputs();\n  }\n#ifdef PAPERBOY_RISCRTE_ELF\n  if (g_panel_io != nullptr) {\n    const esp_err_t rc = esp_lcd_panel_io_del(g_panel_io);\n    if (rc != ESP_OK) { ESP_LOGE(kTag, "panel IO release: %s", esp_err_to_name(rc)); abort(); }\n    g_panel_io = nullptr;\n  }\n  if (g_i80_bus != nullptr) {\n    const esp_err_t rc = esp_lcd_del_i80_bus(g_i80_bus);\n    if (rc != ESP_OK) { ESP_LOGE(kTag, "i80 bus release: %s", esp_err_to_name(rc)); abort(); }\n    g_i80_bus = nullptr;\n  }\n  release_allocations();\n  g_expander = nullptr;\n  g_dma_done = true;\n  g_flip_req = false;\n  g_drive_pending = false;\n#endif\n}'''
    epd = patch_once(epd, original_tail, replacement_tail, 'LCD/DMA teardown')
    (destination / 'epd_video.cpp').write_text(epd, encoding='utf-8')
    print(f'Staged complete GameBoy application and original display driver in {destination}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/riscrte/src')
    args = parser.parse_args()
    stage(args.output)
