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
                   'touch_read(', 'compose_scene(', 'rotate_game_to_panel(',
                   'refresh_current_page(', 'battery_read_status('):
        assert symbol in original and symbol in staged, symbol
    assert 'run_console(nullptr);' in staged
    assert 'while (!paperboy_elf_exit_requested()) {' in staged
    assert 'paperboy_elf_request_exit();' in staged
    assert 'app_hardware_takeover()' in staged
    assert 'void app_main()' in staged
    assert 'esp_deep_sleep_start();' in original
    assert '#ifndef PAPERBOY_RISCRTE_ELF' in staged
    assert 'esp_lcd_panel_io_tx_color' in epd_staged
    assert 'esp_lcd_panel_io_del(g_panel_io)' in epd_staged
    assert 'esp_lcd_del_i80_bus(g_i80_bus)' in epd_staged
    assert 'release_allocations();' in epd_staged
    assert 'kVideoDrivePasses = 3' in epd_staged
    assert original == (ROOT / 'src/main.cpp').read_text()
    assert epd_original == (ROOT / 'src/epd_video.cpp').read_text()
print('Faithful GameBoy ELF source staging and hardware-release checks passed')
