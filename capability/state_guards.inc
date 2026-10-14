/* Validate serialized memory geometry before the legacy loader copies it into
 * the live core or constructs bank pointers. Never follow serialized pointers.
 * Existing snapshot bytes and valid old state versions remain compatible. */
static bool cap_state_geometry_valid(const gbemu_t *emu,const void *data,size_t size) {
 if(size<sizeof(struct StateHeader))return false;
 struct StateHeader header;memcpy(&header,data,sizeof(header));
 if(header.version!=PGB_VERSION)return true; /* legacy upgrade owns old layouts */
 if(size<sizeof(header)+sizeof(gb_s) || header.gb_s_size!=sizeof(gb_s))return false;
 gb_s saved;memcpy(&saved,(const uint8_t*)data+sizeof(header),sizeof(saved));
 const gb_s *live=&emu->core;
 if(saved.gb_rom_size!=live->gb_rom_size || saved.gb_cart_ram_size!=live->gb_cart_ram_size ||
    saved.num_rom_banks_mask!=live->num_rom_banks_mask || saved.num_ram_banks!=live->num_ram_banks ||
    saved.mbc!=live->mbc || saved.is_mbc1m!=live->is_mbc1m || saved.is_cgb_mode!=live->is_cgb_mode ||
    saved.cart_ram!=live->cart_ram || saved.cart_battery!=live->cart_battery)return false;
 if(saved.zero_bank_base>emu->rom_size || emu->rom_size-saved.zero_bank_base<ROM_BANK_SIZE ||
    saved.zero_bank_base%ROM_BANK_SIZE)return false;
 const size_t bank=(size_t)(saved.selected_rom_bank & saved.num_rom_banks_mask)*ROM_BANK_SIZE;
 if(bank>emu->rom_size || emu->rom_size-bank<ROM_BANK_SIZE)return false;
 const size_t required=sizeof(header)+sizeof(gb_s)+ROM_HEADER_SIZE+WRAM_SIZE_CGB+VRAM_SIZE_CGB+XRAM_SIZE+
     emu->cart_ram_size+MAX_BREAKPOINTS*sizeof(gb_breakpoint);
 return size==required;
}
