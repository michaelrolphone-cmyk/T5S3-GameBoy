// Exercise the actual ELF storage adapter with host APIs that reject workers.
#include <Arduino.h>
#include <T5AppApi.h>
#include <T5StorageApi.h>
#include "paperboy_storage.h"
#include "elf_lifecycle.h"
#include "../riscrte/rom_port.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdio>

void paperboy_storage_hid_diagnostic(const char *);
uint32_t millis() { return 0; }
static TaskHandle_t owner;
static std::map<std::string, std::vector<unsigned char>> files;
static std::vector<unsigned char> *opened;
static size_t offset, cursor;
static std::string opened_directory;
static bool fail_write, fail_read;
static unsigned writes, closes, save_writes, hid_writes, serial_writes;
static void authorized() { assert(xTaskGetCurrentTaskHandle() == owner); }
void paperboy_usb_owner_poll() { authorized(); }

static bool is_directory(const char *path) {
  return path && (!strcmp(path, "/sd") ||
      !strcmp(path, GAMEBOY_STATE_ROM_DIRECTORY) ||
      !strcmp(path, GAMEBOY_ROM_MANAGER_DIRECTORY));
}
static bool dir_open(const char *path) {
  authorized();
  assert(opened_directory.empty());
  if (!is_directory(path)) return false;
  opened_directory = path;
  cursor = 0;
  return true;
}
static bool dir_next(t5_app_dirent_t *entry) {
  authorized();
  assert(!opened_directory.empty());
  *entry = {};
  if (opened_directory == "/sd") {
    if (cursor++) return false;
    strcpy(entry->name, "test.gb");
    entry->size = files["/sd/test.gb"].size();
    return true;
  }
  if (opened_directory == GAMEBOY_STATE_ROM_DIRECTORY) {
    switch (cursor++) {
      case 0:
        strcpy(entry->name, "Local.gb");
        entry->size = files[std::string(GAMEBOY_STATE_ROM_DIRECTORY) + "/Local.gb"].size();
        return true;
      case 1:
        strcpy(entry->name, "Local.gb.sav");
        entry->size = 8192;
        return true;
      default:
        return false;
    }
  }
  if (opened_directory == GAMEBOY_ROM_MANAGER_DIRECTORY) {
    switch (cursor++) {
      case 0:
        strcpy(entry->name, "Managed.gb");
        entry->size = files[std::string(GAMEBOY_ROM_MANAGER_DIRECTORY) + "/Managed.gb"].size();
        return true;
      case 1:
        strcpy(entry->name, "Managed.gb.state");
        entry->size = 32768;
        return true;
      default:
        return false;
    }
  }
  return false;
}
static void dir_close() { authorized(); opened_directory.clear(); }
static bool exists(const char *path) {
  authorized();
  return is_directory(path) || files.count(path);
}
static bool read_file(const char *, void *, size_t, size_t *) { authorized(); return false; }
static bool write_file(const char *path, const void *data, size_t size) {
  authorized(); ++writes;
  if (!strcmp(path, "/sd/serial.log")) ++serial_writes;
  else if (!strcmp(path, "/sd/gameboy-hid.log")) ++hid_writes;
  else ++save_writes;
  if (fail_write) return false;
  const auto *bytes = static_cast<const unsigned char *>(data);
  files[path] = std::vector<unsigned char>(bytes, bytes + size); return true;
}
static bool remove_file(const char *) { authorized(); return false; }
static t5_storage_stream_t stream_open(const char *path, size_t *size) {
  authorized(); assert(!opened);
  auto it = files.find(path); if (it == files.end()) return 0;
  opened = &it->second; offset = 0; *size = opened->size(); return 1;
}
static size_t stream_read(t5_storage_stream_t stream, void *buffer, size_t capacity) {
  authorized(); assert(stream == 1 && opened);
  if (fail_read) return 0;
  size_t n = std::min(size_t(73), std::min(capacity, opened->size() - offset));
  memcpy(buffer, opened->data() + offset, n); offset += n; return n;
}
static void stream_close(t5_storage_stream_t stream) { authorized(); assert(stream == 1 && opened); opened = nullptr; ++closes; }
static const t5_app_api_v1 app{sizeof(t5_app_api_v1), dir_open, dir_next, dir_close};
static const t5_storage_api_v1 storage{1, sizeof(t5_storage_api_v1), exists, read_file, write_file, remove_file, stream_open, stream_read, nullptr, stream_close};
const t5_app_api_v1 *t5_app_get_api(uint32_t) { authorized(); return &app; }
const t5_storage_api_v1 *t5_storage_get_api(uint32_t) { authorized(); return &storage; }

int main() {
  owner = xTaskGetCurrentTaskHandle();
  files["/sd/test.gb"] = std::vector<unsigned char>(32768, 0xA5);
  files[std::string(GAMEBOY_STATE_ROM_DIRECTORY) + "/Local.gb"] =
      std::vector<unsigned char>(32768, 0xB4);
  files[std::string(GAMEBOY_ROM_MANAGER_DIRECTORY) + "/Managed.gb"] =
      std::vector<unsigned char>(32768, 0xC3);
  files[std::string(GAMEBOY_ROM_MANAGER_DIRECTORY) + "/Managed.gb.sav"] =
      std::vector<unsigned char>(16, 0x7E);

  paperboy_storage_bind_host(); assert(paperboy_storage_begin());
  assert(paperboy_storage_status().rom_count == 3);

  std::thread worker([] {
    assert(paperboy_storage_begin());
    assert(paperboy_storage_rescan());

    const PaperboyRomInfo *managed = nullptr;
    for (size_t i = 0; i < paperboy_storage_status().rom_count; ++i) {
      const PaperboyRomInfo *candidate = paperboy_storage_rom(i);
      if (candidate && !strcmp(candidate->name, "Managed.gb")) managed = candidate;
    }
    assert(managed);
    assert(!strcmp(managed->path, GAMEBOY_ROM_MANAGER_DIRECTORY "/Managed.gb"));

    PaperboyRomData rom;
    assert(paperboy_storage_load_rom(managed->path, rom));
    assert(rom.size == 32768 && rom.data[32767] == 0xC3);
    paperboy_storage_free_rom(rom);

    char save_path[PAPERBOY_STORAGE_PATH_MAX];
    char state_path[PAPERBOY_STORAGE_PATH_MAX];
    assert(paperboy_storage_make_save_path(managed->path, save_path, sizeof(save_path)));
    assert(paperboy_storage_make_state_path(managed->path, state_path, sizeof(state_path)));
    assert(!strcmp(save_path, GAMEBOY_STATE_ROM_DIRECTORY "/Managed.gb.sav"));
    assert(!strcmp(state_path, GAMEBOY_STATE_ROM_DIRECTORY "/Managed.gb.state"));

    // Existing adjacent saves remain readable as a migration fallback, but all
    // new writes target GameBoy's own RiscRTE application-state directory.
    char legacy_path[PAPERBOY_STORAGE_PATH_MAX];
    assert(paperboy_storage_make_legacy_save_path(managed->path, legacy_path, sizeof(legacy_path)));
    assert(!strcmp(legacy_path, GAMEBOY_ROM_MANAGER_DIRECTORY "/Managed.gb.sav"));

    const char save[] = "battery RAM and RTC";
    for (const char *path : {save_path, state_path}) {
      assert(paperboy_storage_write_blob_atomic(path, save, sizeof(save)));
      assert(paperboy_storage_file_exists(path));
      size_t size = 0; assert(paperboy_storage_file_size(path, size) && size == sizeof(save));
      char readback[64]; assert(paperboy_storage_read_blob(path, readback, sizeof(readback), size));
      assert(size == sizeof(save) && !memcmp(save, readback, size));
    }

    PaperboyStorageConfig config; strcpy(config.last_rom, managed->path); config.audio_engine = 1;
    assert(paperboy_storage_write_config(config));
    PaperboyStorageConfig restored; assert(paperboy_storage_read_config(restored));
    assert(!strcmp(config.last_rom, restored.last_rom) && restored.audio_engine == 1);

    fail_write = true;
    assert(!paperboy_storage_write_blob_atomic(save_path, save, sizeof(save)));
    assert(paperboy_storage_last_error() == PaperboyStorageError::WriteFailed);
    fail_write = false; fail_read = true;
    assert(!paperboy_storage_load_rom(managed->path, rom)); assert(!opened);
    fail_read = false;
    assert(paperboy_storage_rescan());
    assert(paperboy_storage_load_rom(managed->path, rom)); paperboy_storage_free_rom(rom);
    paperboy_storage_owner_note_console_done();
  });

  // An early/unrelated notification must not finish the owner wait.
  xTaskNotifyGive(owner);
  paperboy_storage_owner_wait(); worker.join();
  assert(save_writes >= 4 && serial_writes > 0 && closes > 4 && !opened);
  const unsigned before = writes;
  for (unsigned i = 0; i < 40; ++i) paperboy_storage_hid_diagnostic("Keyboard connection state");
  assert(writes == before); // No SD transaction inside report/poll callbacks.
  paperboy_storage_end(); // Final flush persists the bounded trace in one write.
  assert(hid_writes == 1);
  assert(files["/sd/gameboy-hid.log"].size() < 4096);
  assert(files["/sd/serial.log"].size() <= 16384);
  puts("PASS: owner-only System ROM discovery and GameBoy-state save/state I/O");
}
