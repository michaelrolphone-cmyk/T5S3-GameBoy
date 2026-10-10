"""Reconstruct the 1.3.23 header snapshot boundary on the recovered 1.3.22 adapter.

Original CPU/snapshot format, save destinations, browser and UI remain unchanged.
This module is new source, not a byte-exact recovery of the missing revision.
"""
def adapt(s,once,span):
    s=span(s,'bool read_blob_allocated(', 'bool save_current_persist(','''bool read_blob_allocated(const char *path, uint8_t *&data, size_t &size) {
  data=nullptr;size=0;
  if(!cap_ready())return false;
  CapLoadTrace total("save-blob-load",path);
  CapLoadTrace sizing("save-blob-size",path);
  const bool sized=paperboy_storage_file_size(path,size);
  sizing.end(sized && size!=0);
  if(!cap_ready() || !sized || size==0)return false;
  CapLoadTrace allocation("save-blob-allocation");
  data=allocate_buffer(size,false);allocation.end(data!=nullptr);
  if(!cap_ready() || !data)return false;
  size_t read_size=0;
  CapLoadTrace reading("save-blob-read",path);
  const bool read=paperboy_storage_read_blob(path,data,size,read_size);
  reading.end(read && read_size==size);
  if(!cap_ready())return false;
  if(!read || read_size!=size){cap_free(data);data=nullptr;size=0;return false;}
  total.end(true);return true;
}''')
    s=once(s,'bool load_current_state() {\n  bool loaded = false;', '''bool load_current_state() {
  if(!cap_ready())return false;
  CapLoadTrace header("header-state-load");
  // Snapshot presence may have changed since ROM selection or a prior error.
  refresh_current_snapshot_availability();
  if(!cap_ready())return false;
  char availability[80];snprintf(availability,sizeof(availability),"memory=%u disk=%u storage=%u",unsigned(g_memory_quicksave_valid),unsigned(g_current_disk_snapshot_available),unsigned(g_storage_ready));
  cap_log("header-state-availability","ok",availability);
  bool loaded = false;''')
    s=once(s,'    loaded = gbemu_load_state(g_emu, g_quicksave, g_quicksave_size);','    cap_log("header-state-source","memory",nullptr);\n    loaded = gbemu_load_state(g_emu, g_quicksave, g_quicksave_size);')
    s=once(s,'    loaded = load_state_into(g_emu, g_current_rom_path);','    cap_log("header-state-source","disk",nullptr);\n    loaded = load_state_into(g_emu, g_current_rom_path);\n    if(!cap_ready())return false;')
    s=once(s,'  return loaded;\n}\n\nvoid apply_audio_engine','  header.end(loaded);\n  return loaded;\n}\n\nvoid apply_audio_engine')
    s=once(s,'bool save_current_session() {','bool save_current_session() {\n  if(!cap_ready())return false;\n  CapLoadTrace header("header-state-save");')
    s=once(s,'  return state_saved && config_saved;','  if(!cap_ready())return false;\n  header.end(state_saved && config_saved);\n  return state_saved && config_saved;')
    s=once(s,'    if ((actions & PAPERBOY_ACTION_LOAD) != 0U) {','    if(!cap_ready())return;\n    if ((actions & PAPERBOY_ACTION_LOAD) != 0U) {')
    # Source helpers must not execute a later emulator action after lost custody.
    for anchor in ['  const bool loaded = gbemu_load_state(emu, state_data, state_size);',
                   '  const bool loaded = gbemu_import_persist(']:
        if anchor in s:s=once(s,anchor,'  if(!cap_ready())return false;\n'+anchor)
    return s
