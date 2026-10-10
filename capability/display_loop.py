"""Keep logical GameBoy frames independent of physical presentation readiness."""
def adapt(s, once, span):
    s=span(s,'void wait_vsync_frames(', 'void perform_startup_clear()', '')
    s=span(s,'void wait_epd_idle()', 'void clean_panel_white_black_white(', '')
    s=span(s,'void present_shutdown_page()', 'void enter_power_off()', '')
    s=span(s,'void refresh_current_page(', 'void enter_power_off()', '''void refresh_current_page(
    PaperboyPage, bool, const PaperboyBatteryStatus *) {
  if (cap_running()) (void)cap_clean_display();
}''')
    s=once(s,'      full_scene_syncs = 0U;\n      skipped_since_render = 0U;\n      reset_game_frame_pacer(game_frame_pacer);\n      boot_refresh_completed',
           '      full_scene_syncs = kPanelBufferCount;\n      boot_refresh_completed')
    s=once(s,'      const uint32_t vsync_now = epd_video_get_vsync_count();',
           '      const uint32_t vsync_now = epd_video_get_vsync_count();\n      if (!cap_ready()) return;\n      if (!cap_running()) break;')
    s=once(s,'  uint8_t skipped_since_render = 0;',
           '  uint8_t skipped_since_render = 0;\n  bool game_frame_dirty = false;')
    s=once(s,'''      const bool render_due =
          full_scene_syncs > 0U ||
          touch_scene_syncs > 0U ||
          skipped_since_render >= skipped_frames_required;''','''      const bool render_due = skipped_since_render >= skipped_frames_required;''')
    s=once(s,'''      const bool skip_render = !render_due || !epd_video_can_submit() ||
          touch_scene_syncs > 0U;''', '''      // Gameplay image generation keeps its original stride, independently of
      // physical readiness. Only completed visual images are coalesced.
      const bool skip_render = !render_due;''')
    a=s.index('      add_sample(run_timing, frame_stats.run_us);')
    b=s.index('      pace_game_frame(game_frame_pacer);',a)
    s=s[:a]+'''      add_sample(run_timing, frame_stats.run_us);
      ++emulated_frames;
      if (!skip_render) {
        game_frame_dirty = true;
        skipped_since_render = 0U;
        add_sample(draw_timing, frame_stats.draw_us);
      } else if (skipped_since_render < UINT8_MAX) {
        ++skipped_since_render;
      }
      // Scale/compose a full physical scene only when the display can accept it.
      const bool display_ready = epd_video_can_submit();
      if (!cap_ready()) return;
      if (!cap_running()) break;
      if ((game_frame_dirty || full_scene_syncs || touch_scene_syncs) && display_ready) {
        uint8_t *backbuffer = epd_video_get_backbuffer();
        const int64_t compose_started = esp_timer_get_time();
        const bool full_scene = full_scene_syncs > 0U || touch_scene_syncs > 0U;
        if (full_scene) compose_scene(backbuffer, touch_buttons, power_on, page, &battery);
        else rotate_game_to_panel(g_game_frame, backbuffer);
        add_sample(compose_timing,
                   static_cast<uint32_t>(esp_timer_get_time() - compose_started));
        const int64_t flip_started = esp_timer_get_time();
        cap_rom_scene();
        const bool submitted = epd_video_submit(
            full_scene ? 0 : (paperboy_is_landscape()
                ? (paperboy_landscape_fullscreen() ? 0 : PAPERBOY_LANDSCAPE_GAME_Y)
                : kGameDirtyY),
            full_scene ? t5s3_epd::kActiveHeight : (paperboy_is_landscape()
                ? (paperboy_landscape_fullscreen() ? PAPERBOY_LANDSCAPE_FULLSCREEN_HEIGHT : GBEMU_FRAME_HEIGHT)
                : kGameDirtyHeight));
        add_sample(flip_timing, static_cast<uint32_t>(esp_timer_get_time() - flip_started));
        if (submitted) {
          ++rendered_frames;
          game_frame_dirty = false;
          touch_scene_syncs = 0U;
          if (full_scene_syncs) --full_scene_syncs;
        } else ++skipped_frames;
      } else ++skipped_frames;
'''+s[b:]
    s=once(s,'    last_touch_buttons = touch_buttons;\n    last_touch_down = touch_down;\n    const uint64_t now',
           '    cap_service_display();\n    if (!cap_ready()) return;\n    last_touch_buttons = touch_buttons;\n    last_touch_down = touch_down;\n    const uint64_t now')
    s=once(s,'    uint32_t actions = touch_ok ? paperboy_ui_map_actions(&touch, page) : 0U;',
           '    uint32_t actions = touch_ok ? paperboy_ui_map_actions(&touch, page) : 0U;\n    if (!cap_running()) break;')
    return s
