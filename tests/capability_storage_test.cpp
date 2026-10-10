#include "platform.hpp"
#include "paperboy_storage.h"
#include "rom_port.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

void paperboy_storage_hid_diagnostic(const char *message);
#include "capability_storage_fixture.hpp"
using namespace capability_storage_fixture;
namespace {
void begin() { assert(paperboy_storage_begin()); assert(handles.empty() && dirs.empty()); }
void assert_retained_fence() {
  assert(held);
  const unsigned before = calls;
  char bytes[16]{}; size_t size = 0;
  PaperboyRomData rom{}; PaperboyStorageConfig config{};
  assert(!paperboy_storage_begin()); assert(!paperboy_storage_rescan());
  assert(!paperboy_storage_load_rom("/sd/a.gb", rom));
  assert(!paperboy_storage_read_blob("/sd/a.sav", bytes, sizeof(bytes), size));
  assert(!paperboy_storage_write_blob_atomic("/sd/a.sav", bytes, sizeof(bytes)));
  assert(!paperboy_storage_read_config(config)); assert(!paperboy_storage_write_config(config));
  assert(!paperboy_storage_file_exists("/sd/a.gb")); assert(!paperboy_storage_file_size("/sd/a.gb", size));
  paperboy_storage_free_rom(rom); paperboy_storage_end();
  paperboy_serial_log("retained"); paperboy_storage_hid_diagnostic("retained");
  assert(calls == before);
}
} // namespace

bool cap_ready() { return !held; }
bool cap_retained() { return held; }
bool cap_running() { return !held; }
void cap_hold(const char *) { assert(!held); ++hold_count; held = true; }
void cap_log(const char *, const char *, const char *) { assert(!held); ++logs; }
uint32_t cap_millis(){assert(!held);return calls+yields;}
void cap_yield() { assert(!held); ++yields; if (terminal_yield && yields == terminal_yield) held = true; }
void *cap_alloc(size_t size) {
  assert(!held);
  if (fail_alloc) return nullptr;
  void *result = std::malloc(size); if (result) allocations.insert(result); return result;
}
void cap_free(void *memory) { assert(!held); assert(allocations.erase(memory) == 1); std::free(memory); }
const risc_storage_volume_api_v1 *cap_storage() { assert(!held); return &api.base; }

int main(int argc, char **argv) {
  assert(argc >= 2); setup();
  const std::string scenario = argv[1];
  const std::string save = "/System/State/Applications/gameboy/A.gb.sav";
  const std::string public_save = "/sd" + save;
  if (scenario == "catalog") {
    add_file("/z.gb", 1); add_file("/A.GBC", 2); add_file("/games/b.gb", 3);
    add_file("/games/deeper/ignored.gb", 4); add_file("/.hidden.gb", 1);
    add_file("/zero.gb", 0); add_file("/large.gb", PAPERBOY_STORAGE_MAX_ROM_BYTES + 1);
    add_file("/readme.txt", 1); add_file("/System/State/Applications/gameboy/c.gb", 5);
    add_file("/System/State/Applications/Rom Manager/Pack/d.GBC", 6);
    add_file("/System/State/Applications/gameboy/A.gb.sav", 1);
    begin(); assert(paperboy_storage_status().rom_count == 5);
    assert(std::strcmp(paperboy_storage_rom(0)->path, "/sd/A.GBC") == 0);
    assert(std::strcmp(paperboy_storage_rom(1)->path, "/sd/games/b.gb") == 0);
    assert(std::strcmp(paperboy_storage_rom(2)->name, "c.gb") == 0);
    assert(std::strcmp(paperboy_storage_rom(3)->name, "d.GBC") == 0);
    assert(std::strcmp(paperboy_storage_rom(4)->name, "z.gb") == 0);
    char path[256]; assert(paperboy_storage_make_save_path("/sd/games/A.gb", path, sizeof(path)));
    assert(path == public_save);
    assert(paperboy_storage_make_state_path("/A.gb", path, sizeof(path)));
    assert(std::strcmp(path, "/sd/System/State/Applications/gameboy/A.gb.state") == 0);
    assert(paperboy_storage_make_legacy_save_path("/sd/A.gb", path, sizeof(path)));
    assert(std::strcmp(path, "/sd/A.sav") == 0);
    add_file("/A.gb.sav", 1);
    assert(paperboy_storage_make_legacy_save_path("/sd/A.gb", path, sizeof(path)));
    assert(std::strcmp(path, "/sd/A.gb.sav") == 0);
    const unsigned before = calls;
    assert(!paperboy_storage_file_exists("/sd/../secret"));
    assert(!paperboy_storage_file_exists("/sd//a.gb"));
    assert(!paperboy_storage_file_exists("/sd/./a.gb"));
    assert(!paperboy_storage_make_save_path("/sd/a.txt", path, sizeof(path)));
    assert(calls == before);
    unsigned previous_logs=logs;paperboy_serial_log("hello"); paperboy_storage_hid_diagnostic("report");
    assert(logs == previous_logs+2 && calls == before && !files.count("/serial.log") && !files.count("/gameboy-hid.log"));
  } else if (scenario == "truncation") {
    for (unsigned i = 0; i < 70; ++i) { char path[32]; std::snprintf(path, sizeof(path), "/%02u.gb", i); add_file(path, 1); }
    begin(); assert(paperboy_storage_status().rom_count == 64 && paperboy_storage_status().roms_truncated);
    assert(std::strcmp(paperboy_storage_rom(0)->name, "00.gb") == 0);
    assert(std::strcmp(paperboy_storage_rom(63)->name, "63.gb") == 0);
  } else if (scenario == "abi") {
    assert(argc == 3); const int variant = std::atoi(argv[2]);
    if (variant == 0) api.base.api_version = 2;
    if (variant == 1) api.base.struct_size = sizeof(api.base);
    if (variant == 2) api.dir_close_checked = nullptr;
    if (variant == 3) api.handle_error = nullptr;
    if (variant == 4) api.rename = nullptr;
    assert(!paperboy_storage_begin()); assert(calls == 0);
    assert(paperboy_storage_last_error() == PaperboyStorageError::MountFailed);
  } else if (scenario == "media") {
    media_ready = false; assert(!paperboy_storage_begin()); assert(calls == 2);
    assert(paperboy_storage_last_error() == PaperboyStorageError::CardUnavailable);
  } else if (scenario == "scan_error") {
    directory_error = true; assert(!paperboy_storage_begin()); assert(!held && dirs.empty());
    assert(paperboy_storage_last_error() == PaperboyStorageError::ScanFailed);
  } else if (scenario == "dir_close") {
    fail_dir_close = true; assert(!paperboy_storage_begin()); assert(hold_count == 1 && dirs.size() == 1);
    assert_retained_fence();
  } else if (scenario == "read" || scenario == "read_close" || scenario == "read_error" || scenario == "read_count" || scenario == "alloc") {
    add_file("/a.gb", 20000); begin();
    partial = 733;
    if (scenario == "read_close") fail_file_close = true;
    if (scenario == "read_error") fail_read = true;
    if (scenario == "read_count") over_read = true;
    if (scenario == "alloc") fail_alloc = true;
    PaperboyRomData rom{};
    const bool result = paperboy_storage_load_rom("/a.gb", rom);
    if (scenario == "read") {
      assert(result && rom.size == 20000 && std::memcmp(rom.data, files.at("/a.gb").data(), rom.size) == 0);
      assert(max_read == 4096 && yields > 20); paperboy_storage_free_rom(rom); assert(allocations.empty());
      char small[2]; size_t size = 0;
      assert(!paperboy_storage_read_blob("/sd/a.gb", small, sizeof(small), size));
      assert(size == 20000 && paperboy_storage_last_error() == PaperboyStorageError::BufferTooSmall);
    } else {
      assert(!result);
      if (held) { assert_retained_fence(); assert(!handles.empty()); }
      else assert(handles.empty() && allocations.empty());
    }
  } else if (scenario == "config") {
    begin(); PaperboyStorageConfig config{}; assert(paperboy_storage_read_config(config));
    std::strcpy(config.last_rom, "/sd/games/A.gb"); config.audio_engine = 2;
    assert(paperboy_storage_write_config(config));
    const std::string content(files.at("/paperboy.cfg").begin(), files.at("/paperboy.cfg").end());
    assert(content == "last_rom=/sd/games/A.gb\naudio_engine=2\n");
    PaperboyStorageConfig loaded{}; assert(paperboy_storage_read_config(loaded));
    assert(std::strcmp(config.last_rom, loaded.last_rom) == 0 && loaded.audio_engine == 2);
    const std::string bad = "audio_engine=3\n"; files["/paperboy.cfg"] = {bad.begin(), bad.end()};
    assert(!paperboy_storage_read_config(loaded));
    assert(paperboy_storage_last_error() == PaperboyStorageError::ConfigInvalid);
  } else if (scenario == "recover") {
    add_file(save + ".bak", 3, 0x41); add_file(save + ".tmp", 5, 0x42); begin();
    size_t size = 0; uint8_t bytes[16]{};
    assert(paperboy_storage_read_blob(public_save.c_str(), bytes, sizeof(bytes), size));
    assert(size == 3 && bytes[0] == 0x41 && !files.count(save + ".tmp") && !files.count(save + ".bak"));
    add_file(save + ".bak", 4); assert(paperboy_storage_file_exists(public_save.c_str()));
    assert(!files.count(save + ".bak"));
  } else if (scenario == "empty") {
    begin(); assert(paperboy_storage_write_blob_atomic(public_save.c_str(), nullptr, 0));
    size_t size = 7; assert(paperboy_storage_read_blob(public_save.c_str(), nullptr, 0, size)); assert(size == 0);
  } else {
    add_file(save, 7, 0x11); begin();
    const auto old = files.at(save);
    std::vector<uint8_t> bytes(12003, 0x32);
    partial = 777;
    if (scenario == "write_error") fail_write = true;
    if (scenario == "write_count") over_write = true;
    if (scenario == "sync") fail_sync = true;
    if (scenario == "write_close") fail_file_close = true;
    if (scenario == "verify") corrupt_verify = true;
    if (scenario == "rename_old") fail_rename_calls = {1};
    if (scenario == "rollback") fail_rename_calls = {2};
    if (scenario == "rollback_hold") fail_rename_calls = {2, 3};
    if (scenario == "backup_remove") fail_remove = true;
    if (scenario == "terminal") { assert(argc == 3); terminal_at = calls + static_cast<unsigned>(std::atoi(argv[2])); }
    if (scenario == "yield_hold") terminal_yield = yields + 1;
    const unsigned start = calls;
    const bool result = paperboy_storage_write_blob_atomic(public_save.c_str(), bytes.data(), bytes.size());
    if (scenario == "write") {
      assert(result && files.at(save) == bytes && max_write == 4096 && max_read <= 4096);
      assert(!files.count(save + ".tmp") && !files.count(save + ".bak"));
      std::printf("atomic_calls=%u\n", calls - start);
    } else if (scenario == "terminal" && !held) {
      assert(result && files.at(save) == bytes); std::printf("terminal_not_reached\n");
    } else {
      assert(!result);
      if (held) assert_retained_fence();
      if (scenario == "rollback_hold") {
        assert(!files.count(save) && files.at(save + ".bak") == old && files.at(save + ".tmp") == bytes);
        assert(hold_count == 1);
      } else if (scenario == "backup_remove") {
        assert(files.at(save) == bytes && files.at(save + ".bak") == old);
      } else if (scenario != "terminal") assert(files.at(save) == old);
      if (!held) assert(handles.empty() && dirs.empty());
    }
  }
  if (!held) { paperboy_storage_end(); assert(handles.empty() && dirs.empty() && allocations.empty()); }
  // Terminal custody intentionally survives app teardown. Release only test
  // allocator memory after proving no production call crosses the fence.
  for (void *memory : allocations) std::free(memory);
  std::printf("PASS %s calls=%u held=%u\n", argv[1], calls, held ? 1 : 0);
  return 0;
}
