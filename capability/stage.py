#!/usr/bin/env python3
"""Adapt only the hardware/lifetime boundaries of the original application.

UI, console state transitions, save/load, ROM library and emulator stay sourced
from their original files. Every replacement requires its original anchor.
"""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
def once(text,old,new):
    if text.count(old)!=1: raise ValueError('Original application anchor changed: '+old[:100])
    return text.replace(old,new,1)
def span(text,start,end,new):
    a=text.index(start); b=text.index(end,a+len(start))
    return text[:a]+new+'\n\n'+text[b:]
def stage(out):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    s=(ROOT/'src/main.cpp').read_text()
    s=span(s,'#include <Arduino.h>','#include <stdio.h>','#include "platform.hpp"\n#include "load_trace.hpp"')
    s=once(s,'#include "pca9535_min.h"','')
    s=once(s,'Pca9535Min g_expander;','')
    s=span(s,'uint8_t decode_bcd(','void add_sample(', 'uint32_t read_rtc_timestamp() { return cap_epoch(); }')
    s=span(s,'uint8_t *allocate_buffer(','bool find_existing_sidecar(',
        'uint8_t *allocate_buffer(size_t size, bool) { return static_cast<uint8_t *>(cap_alloc(size)); }')
    s=span(s,'void scan_i2c_bus()','void submit_clear_frame(',
        'bool init_display() { return cap_display_init(); }\n'
        'void wait_vsync_frames(uint8_t count) { cap_wait_display(count); }')
    # The original waveform pulse train is panel-specific. A typed CLEAN request
    # asks the selected physical driver to implement its qualified clean policy.
    s=span(s,'void perform_startup_clear()','bool allocate_runtime()',
        'void perform_startup_clear() { /* First complete UI uses selected fast presentation. */ }')
    s=span(s,'void on_shutdown()','void draw_shutdown_page()',
        'bool read_expander_button(bool &pressed) { pressed=false; return cap_ready(); }')
    s=span(s,'void clean_panel_white_black_white(','void refresh_current_page(',
        'void clean_panel_white_black_white(const char *reason) {\n'
        '  cap_log("display-clean", "begin", reason);\n'
        '  if (cap_ready()) (void)cap_clean_display();\n}')
    s=span(s,'[[noreturn]] void enter_power_off()','void run_console(',
        'void enter_power_off() { cap_exit(); }')
    s=once(s,'  while (true) {\n    battery_service();','  while (cap_running()) {\n    cap_yield();\n    if (!cap_running()) break;\n    battery_service();')
    s=s.replace('digitalRead(t5s3_epd::kBootButton) == LOW','false /* Physical Home is handled by the navigation capability. */')
    s=once(s,'  while (epd_video_submit_pending()) {','  while (cap_running() && epd_video_submit_pending()) {')
    s=s.replace('while (!epd_video_submit(0, t5s3_epd::kActiveHeight)) {',
        'while (cap_running() && !epd_video_submit(0, t5s3_epd::kActiveHeight)) {')
    s=once(s,'  while (true) {\n    const int64_t now = esp_timer_get_time();',
        '  while (cap_running()) {\n    const int64_t now = esp_timer_get_time();')
    s=s.replace('(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)', '0u /* Native heap telemetry is reported by Runtime. */').replace('(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)', '0u')
    s=once(s,'  vTaskDelete(nullptr);','  // Return on the invocation owner task.')
    s=once(s,'void setup() {\n  Serial.begin(115200);\n  delay(1500);','void cap_console_setup() {')
    s=span(s,'  ESP_LOGI(\n      kTag,\n      "memory internal=', '  if (!init_display()) {','')
    s=span(s,'  pinMode(t5s3_epd::kBootButton, INPUT_PULLUP);','  paperboy_ui_init();','')
    s=span(s,'  const BaseType_t task_result = xTaskCreatePinnedToCore(','\nvoid loop() {','  run_console(nullptr);\n}')
    s=s[:s.index('\nvoid loop() {')]
    # Keep errors/actions, not an unchanged battery sample or performance window
    # every second. The app's diagnostic sink remains Runtime's bounded ring.
    s=span(s,'      ESP_LOGI(\n          kTag,\n          "battery poll=',
        '      if (indicator_changed', '      (void)ok;')
    s=once(s,'  (void)save_current_persist(true);\n  audio_deinit();\n  // Return',
        '  if (cap_ready()) (void)save_current_persist(true);\n  audio_deinit();\n  // Return')
    s=once(s,'  prepare_quicksave();\n\n  ESP_LOGI(',
        '  prepare_quicksave();\n'
        '  char requested[PAPERBOY_STORAGE_PATH_MAX]={0};\n'
        '  if (cap_file_source(requested,sizeof(requested))) {\n'
        '    if (launch_sd_rom(requested,false,false)) g_initial_page=PaperboyPage::Game;\n'
        '    else g_initial_page=PaperboyPage::SdCard;\n'
        '  }\n\n  ESP_LOGI(')
    # Retained custody ends application work immediately, including local error
    # rendering. A failed provider close can occur during any storage helper.
    for anchor in [
        'void cap_console_setup() {',
        'void present_error(const char *headline, const char *detail) {',
        'void enter_idle(const char *reason, const char *headline, const char *detail) {',
    ]:
        s=once(s,anchor,anchor+'\n  if (!cap_ready()) return;')
    for anchor in [
        '  const bool storage_scan_ok = paperboy_storage_begin();',
        '  refresh_last_snapshot_availability();\n  if (g_storage_ready',
        '  const uint32_t rtc_timestamp = read_rtc_timestamp();',
        '  prepare_quicksave();\n  char requested',
    ]:
        first,separator,rest=anchor.partition('\n')
        s=once(s,anchor,first+'\n  if (!cap_ready()) return;'+(separator+rest if separator else ''))
    s=once(s,'  if (g_storage_ready && !paperboy_storage_read_config(g_storage_config)) {',
        '  const bool config_ok = !g_storage_ready || paperboy_storage_read_config(g_storage_config);\n'
        '  if (!cap_ready()) return;\n  if (!config_ok) {')
    s=once(s,'  if (cap_file_source(requested,sizeof(requested))) {',
        '  const bool have_request=cap_file_source(requested,sizeof(requested));\n'
        '  if (!cap_ready()) return;\n  if (have_request) {')
    s=once(s,'    else g_initial_page=PaperboyPage::SdCard;\n  }',
        '    else g_initial_page=PaperboyPage::SdCard;\n    if (!cap_ready()) return;\n  }')
    s=once(s,'    PaperboyPage next_page = page;',
        '    if (!cap_ready()) return;\n    PaperboyPage next_page = page;')
    s=once(s,'    if ((actions & PAPERBOY_ACTION_SETTINGS) != 0U) {',
        '    if (!cap_ready()) return;\n    if ((actions & PAPERBOY_ACTION_SETTINGS) != 0U) {')
    # Selected-ROM operation timing lives only in the capability frontend;
    # standalone behavior/UI and directory discovery remain unchanged.
    a=s.index('bool prepare_sd_candidate(');b=s.index('bool capture_current_state_for_rom_swap(',a)
    part=s[a:b]
    part=once(part,'  candidate_emu = gbemu_create();','  CapLoadTrace allocation_trace("emulator-allocation");\n  candidate_emu = gbemu_create();\n  allocation_trace.end(candidate_emu!=nullptr);')
    part=once(part,'  const gbemu_status_t init_status = gbemu_init(', '  CapLoadTrace init_trace("rom-header-emulator-init");\n  const gbemu_status_t init_status = gbemu_init(')
    part=once(part,'      candidate_emu, candidate_rom.data, candidate_rom.size);', '      candidate_emu, candidate_rom.data, candidate_rom.size);\n  init_trace.end(init_status==GBEMU_STATUS_OK,int(init_status),gbemu_status_string(init_status));')
    part=once(part,'  persist_loaded = load_persist_into(candidate_emu, rom_path);','  CapLoadTrace persist_trace("save-ram-restore");\n  persist_loaded = load_persist_into(candidate_emu, rom_path);\n  persist_trace.end(persist_loaded);')
    part=once(part,'  if (restore_snapshot && !load_state_into(candidate_emu, rom_path)) {',
        '  bool snapshot_loaded=true;\n  if (restore_snapshot) { CapLoadTrace state_trace("save-state-restore");\n    snapshot_loaded=load_state_into(candidate_emu,rom_path);state_trace.end(snapshot_loaded); }\n'
        '  else cap_log("save-state-restore","skipped","normal Play request");\n  if (!snapshot_loaded) {')
    s=s[:a]+part+s[b:]
    a=s.index('bool launch_sd_rom(');b=s.index('bool rescan_storage()',a);part=s[a:b]
    part=once(part,'  if (!g_storage_ready || rom_path == nullptr',
        '  cap_rom_begin(rom_path);\n  CapLoadTrace launch_trace("selected-rom-load");\n  if (!g_storage_ready || rom_path == nullptr')
    part=once(part,'  if (!save_current_persist(true)) {',
        '  CapLoadTrace save_trace("previous-cartridge-save");\n  const bool saved=save_current_persist(true);save_trace.end(saved);\n  if (!saved) {')
    part=once(part,'  (void)write_current_config();',
        '  CapLoadTrace config_trace("selected-rom-config-save");\n  const bool config_saved=write_current_config();config_trace.end(config_saved);')
    part=once(part,'  return true;\n}', '  launch_trace.end(true);cap_rom_ready();\n  return true;\n}')
    s=s[:a]+part+s[b:]
    s=once(s,'      if (!skip_render) {\n        draw_game_low_battery_overlay',
        '      cap_rom_frame(!skip_render);\n      if (!skip_render) {\n        draw_game_low_battery_overlay')
    s=once(s,'t5s3_epd::kBoardName, kFirmwareVersion','cap_device_label(), kFirmwareVersion')
    s=once(s,'      audio_engine_name(audio_get_engine()),\n      rtc_timestamp',
        '      "silent capability backend",\n      rtc_timestamp')
    import importlib.util
    spec=importlib.util.spec_from_file_location('gameboy_display_loop',ROOT/'capability/display_loop.py')
    display_loop=importlib.util.module_from_spec(spec);spec.loader.exec_module(display_loop)
    s=display_loop.adapt(s,once,span)
    spec=importlib.util.spec_from_file_location('gameboy_header_load',ROOT/'capability/header_load.py')
    header=importlib.util.module_from_spec(spec);spec.loader.exec_module(header)
    s=header.adapt(s,once,span)
    # Hardware-free UI uses the existing frame and all application state.
    s+='''\nvoid cap_console_cleanup() {
  if (cap_retained()) return;
  night_light_shutdown();
  if (!cap_ready()) return;
  paperboy_storage_end();
  if (cap_retained()) return;
  audio_deinit();
  if (g_emu) { gbemu_destroy(g_emu); g_emu=nullptr; }
  paperboy_storage_free_rom(g_sd_rom);
  release_quicksave();
  cap_free(g_background); g_background=nullptr;
  cap_free(g_scene); g_scene=nullptr;
  cap_free(g_game_frame); g_game_frame=nullptr;
}
'''
    (out/'main.cpp').write_text(s)
    ui=(ROOT/'src/paperboy_ui.cpp').read_text()
    ui=once(ui,'"DEVICE", "LILYGO T5S3 PRO"','"DEVICE", cap_device_label()')
    ui=once(ui,'"DISPLAY", "4.7 IN 960x540 EPD"','"DISPLAY", cap_display_label()')
    ui=once(ui,'snprintf(value, sizeof(value), "%u MB", static_cast<unsigned>(ESP.getFlashChipSize() / (1024U * 1024U)));','snprintf(value, sizeof(value), "HOST MANAGED");')
    ui=once(ui,'snprintf(value, sizeof(value), "%u MB", static_cast<unsigned>(ESP.getPsramSize() / (1024U * 1024U)));','snprintf(value, sizeof(value), "HOST MANAGED");')
    (out/'paperboy_ui.cpp').write_text(ui)
    clock=(ROOT/'src/paperboy_game_clock.cpp').read_text()
    clock=once(clock,'#include "paperboy_game_clock.h"','#include "paperboy_game_clock.h"\n#include "platform.hpp"')
    clock=span(clock,'  const uint32_t minutes_today =','  return true;',
        '  cap_clock_text(epoch_seconds,g_clock_label,sizeof(g_clock_label));')
    (out/'paperboy_game_clock.cpp').write_text(clock)
    # rom_port owns a temporary folder list. Its original scan ordering and
    # path logic remain unchanged; only allocator custody is adapted.
    rom=(ROOT/'riscrte/rom_port.c').read_text()
    rom=once(rom,'#include "rom_port.h"',
        '#include "rom_port.h"\nvoid *cap_core_realloc(void *,size_t);\nvoid cap_core_free(void *);')
    rom=once(rom,'realloc(folders->paths, cap * sizeof(*folders->paths))',
        'cap_core_realloc(folders->paths, cap * sizeof(*folders->paths))')
    rom=once(rom,'free(folders.paths);','cap_core_free(folders.paths);')
    (out/'rom_port.c').write_text(rom)
    return out/'main.cpp'
if __name__=='__main__':
    import sys
    stage(Path(sys.argv[1]))
