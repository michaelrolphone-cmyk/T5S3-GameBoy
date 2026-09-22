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
    assert 'paperboy_storage_bind_host();' in staged
    assert '(void)paperboy_storage_begin();' in staged
    assert 'paperboy_storage_owner_wait();' in staged
    assert 'paperboy_storage_owner_note_console_done();' in staged
    assert staged.find('paperboy_storage_bind_host();') < staged.find('(void)paperboy_storage_begin();') < staged.find('xTaskCreatePinnedToCore(')
    app_main = staged.split('void app_main()', 1)[1]
    create = app_main.index('xTaskCreatePinnedToCore(')
    hid_begin = app_main.index('paperboy_usb_owner_begin();')
    worker_start = app_main.index('xTaskNotifyGive(console_task);')
    assert create < hid_begin < worker_start
    assert 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000))' in staged
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
