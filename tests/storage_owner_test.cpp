// Exercise the actual ELF storage adapter with host APIs that reject workers.
#include <Arduino.h>
#include <T5AppApi.h>
#include <T5StorageApi.h>
#include "paperboy_storage.h"
#include "elf_lifecycle.h"
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
static bool directory, fail_write, fail_read;
static unsigned writes, closes;
static void authorized() { assert(xTaskGetCurrentTaskHandle() == owner); }
static bool dir_open(const char *path) { authorized(); assert(!directory); directory = !strcmp(path, "/sd"); cursor = 0; return directory; }
static bool dir_next(t5_app_dirent_t *entry) {
  authorized(); assert(directory);
  if (cursor++) return false;
  *entry = {}; strcpy(entry->name, "test.gb"); entry->size = files["/sd/test.gb"].size(); return true;
}
static void dir_close() { authorized(); directory = false; }
static bool exists(const char *path) { authorized(); return files.count(path); }
static bool read_file(const char *, void *, size_t, size_t *) { authorized(); return false; }
static bool write_file(const char *path, const void *data, size_t size) {
  authorized(); ++writes;
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
  paperboy_storage_bind_host(); assert(paperboy_storage_begin());
  std::thread worker([] {
    assert(paperboy_storage_begin());
    assert(paperboy_storage_rescan());
    PaperboyRomData rom;
    assert(paperboy_storage_load_rom(size_t(0), rom));
    assert(rom.size == 32768 && rom.data[32767] == 0xA5);
    paperboy_storage_free_rom(rom);
    const char save[] = "battery RAM and RTC";
    for (const char *path : {"/test.gb.sav", "/test.gb.state"}) {
      assert(paperboy_storage_write_blob_atomic(path, save, sizeof(save)));
      assert(paperboy_storage_file_exists(path));
      size_t size = 0; assert(paperboy_storage_file_size(path, size) && size == sizeof(save));
      char readback[64]; assert(paperboy_storage_read_blob(path, readback, sizeof(readback), size));
      assert(size == sizeof(save) && !memcmp(save, readback, size));
    }
    PaperboyStorageConfig config; strcpy(config.last_rom, "/test.gb"); config.audio_engine = 1;
    assert(paperboy_storage_write_config(config));
    PaperboyStorageConfig restored; assert(paperboy_storage_read_config(restored));
    assert(!strcmp(config.last_rom, restored.last_rom) && restored.audio_engine == 1);
    fail_write = true;
    assert(!paperboy_storage_write_blob_atomic("/test.gb.sav", save, sizeof(save)));
    assert(paperboy_storage_last_error() == PaperboyStorageError::WriteFailed);
    fail_write = false; fail_read = true;
    assert(!paperboy_storage_load_rom(size_t(0), rom)); assert(!opened);
    fail_read = false;
    assert(paperboy_storage_rescan());
    assert(paperboy_storage_load_rom(size_t(0), rom)); paperboy_storage_free_rom(rom);
    paperboy_storage_owner_note_console_done();
  });
  // An early/unrelated notification must not finish the owner wait.
  xTaskNotifyGive(owner);
  paperboy_storage_owner_wait(); worker.join();
  assert(writes == 4 && closes > 4 && !opened);
  for (unsigned i = 0; i < 20; ++i) paperboy_storage_hid_diagnostic("Keyboard connection state");
  assert(writes == 16); // Four saves plus at most twelve bounded trace writes.
  assert(files["/sd/gameboy-hid.log"].size() < 2048);
  paperboy_storage_end();
  puts("PASS: owner-only ROM/rescan/config/save/state I/O and failure cleanup");
}
