/* Host-only state comparison. Original snapshots embed gb_s process pointers.
 * Clear named pointer slots only; retain all scalar/core/RAM/RTC/APU bytes. */
void lifecycle_normalize_state(void *buffer, size_t size) {
  const size_t offset=sizeof(gbemu_state_header_t)+sizeof(struct StateHeader);
  if(size<offset+sizeof(gb_s))abort();
  uint8_t *core=(uint8_t *)buffer+offset;
#define CLEAR_POINTER(field) memset(core+offsetof(gb_s,field),0,sizeof(((gb_s *)0)->field))
  CLEAR_POINTER(gb_rom);CLEAR_POINTER(gb_cart_ram);CLEAR_POINTER(gb_error);
  CLEAR_POINTER(gb_serial_tx);CLEAR_POINTER(gb_serial_rx);CLEAR_POINTER(ram_base);
  CLEAR_POINTER(vram_base);CLEAR_POINTER(selected_cart_bank_addr);
  CLEAR_POINTER(wram);CLEAR_POINTER(vram);CLEAR_POINTER(lcd);
  CLEAR_POINTER(display.bg_map_base);CLEAR_POINTER(display.window_map_base);
  CLEAR_POINTER(direct.oam_ghost_buffer);CLEAR_POINTER(direct.priv);
  CLEAR_POINTER(breakpoints);CLEAR_POINTER(xram);CLEAR_POINTER(audio.audio_mem);
#undef CLEAR_POINTER
}
