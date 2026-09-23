#include "paperboy_storage.h"
#include "elf_lifecycle.h"

#include <Arduino.h>
#include <T5AppApi.h>
#include <T5StorageApi.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom_port.h"

namespace {
constexpr char kTag[] = "storage";
constexpr char kConfigPath[] = "/sd/paperboy.cfg";
constexpr size_t kConfigMaxBytes = 1024U;

const t5_app_api_v1 *g_app = nullptr;
const t5_storage_api_v1 *g_storage = nullptr;
PaperboyRomInfo g_roms[PAPERBOY_STORAGE_MAX_ROMS];
PaperboyStorageStatus g_status;
bool g_scan_ok = false;
gameboy_rom_catalog_t g_scan_catalog;

// Requests retain the caller's buffers until the owner has completed the call.
// Only the owner executes host APIs; cached API pointers do not grant a worker
// the host session's task-owned authorization.
struct OwnerRequest {
  void (*execute)(void *);
  void *context;
  bool done;
};
TaskHandle_t g_owner_task = nullptr;
OwnerRequest *g_owner_request = nullptr;
bool g_console_done = false;
constexpr size_t kSerialLogBytes = 16384;
char g_serial_log[kSerialLogBytes] = {};
size_t g_serial_used = 0;
bool g_serial_dirty = false;
uint32_t g_serial_last_flush = 0;
char g_hid_trace[4096] = {};
size_t g_hid_used = 0;
unsigned g_hid_entries = 0;
bool g_hid_dirty = false;

void serial_append(const char *message) {
  if (!message || !g_status.mounted || !g_storage) return;
  char line[224];
  const int count = snprintf(line, sizeof(line), "%lu %.*s\n",
                             static_cast<unsigned long>(millis()), 190, message);
  if (count <= 0) return;
  const size_t length = static_cast<size_t>(count) < sizeof(line)
      ? static_cast<size_t>(count) : sizeof(line) - 1;
  if (g_serial_used + length > sizeof(g_serial_log)) {
    const size_t overflow = g_serial_used + length - sizeof(g_serial_log);
    size_t cut = overflow;
    while (cut < g_serial_used && g_serial_log[cut - 1] != '\n') ++cut;
    memmove(g_serial_log, g_serial_log + cut, g_serial_used - cut);
    g_serial_used -= cut;
  }
  memcpy(g_serial_log + g_serial_used, line, length);
  g_serial_used += length;
  g_serial_dirty = true;
}

void serial_flush(bool force = false) {
  if ((!g_serial_dirty && !g_hid_dirty) || !g_storage || !g_status.mounted ||
      (!force && uint32_t(millis() - g_serial_last_flush) < 1000U)) return;
  g_serial_last_flush = millis();
  if (g_serial_dirty && g_storage->write_file_atomic("/sd/serial.log", g_serial_log, g_serial_used))
    g_serial_dirty = false;
  if (g_hid_dirty && g_storage->write_file_atomic("/sd/gameboy-hid.log", g_hid_trace, g_hid_used))
    g_hid_dirty = false;
}

template <typename Function>
void on_owner(Function function) {
  if (xTaskGetCurrentTaskHandle() == g_owner_task) {
    function();
    return;
  }
  OwnerRequest request{[](void *context) {
    (*static_cast<Function *>(context))();
  }, &function, false};
  OwnerRequest *empty = nullptr;
  while (!__atomic_compare_exchange_n(&g_owner_request, &empty, &request,
                                      false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    empty = nullptr;
    vTaskDelay(1);
  }
  xTaskNotifyGive(g_owner_task);
  // Never time out with a live stack request or caller-owned buffer in flight.
  while (!__atomic_load_n(&request.done, __ATOMIC_ACQUIRE)) vTaskDelay(1);
}

bool host_exists(const char *path) {
  bool result = false;
  on_owner([&] { result = g_storage->exists(path); });
  return result;
}

bool host_write_atomic(const char *path, const void *data, size_t size) {
  bool result = false;
  on_owner([&] { result = g_storage->write_file_atomic(path, data, size); });
  return result;
}

void set_error(PaperboyStorageError error) { g_status.error = error; }

size_t bounded_length(const char *value, size_t limit) {
  if (!value) return limit;
  size_t length = 0;
  while (length < limit && value[length]) ++length;
  return length;
}

bool copy_string(char *destination, size_t capacity, const char *source) {
  if (!destination || !capacity || !source) return false;
  const size_t length = bounded_length(source, capacity);
  if (length >= capacity) { destination[0] = '\0'; return false; }
  memcpy(destination, source, length + 1U);
  return true;
}

unsigned char ascii_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z'
      ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

int compare_ci(const char *left, const char *right) {
  while (*left && *right) {
    const unsigned char a = ascii_lower(static_cast<unsigned char>(*left++));
    const unsigned char b = ascii_lower(static_cast<unsigned char>(*right++));
    if (a != b) return a < b ? -1 : 1;
  }
  if (*left == *right) return 0;
  return *left == '\0' ? -1 : 1;
}

bool has_rom_extension(const char *path) {
  const char *dot = path ? strrchr(path, '.') : nullptr;
  return dot && (compare_ci(dot, ".gb") == 0 || compare_ci(dot, ".gbc") == 0);
}

bool valid_file_path(const char *path) {
  if (!path || path[0] != '/') return false;
  const size_t length = bounded_length(path, PAPERBOY_STORAGE_PATH_MAX);
  if (length < 2U || length >= PAPERBOY_STORAGE_PATH_MAX || path[length - 1U] == '/') return false;
  const char *segment = path + 1;
  for (const char *cursor = segment;; ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (value == '\\' || (value && value < 0x20U)) return false;
    if (*cursor == '/' || *cursor == '\0') {
      const size_t segment_length = static_cast<size_t>(cursor - segment);
      if (!segment_length || (segment_length == 1U && segment[0] == '.') ||
          (segment_length == 2U && segment[0] == '.' && segment[1] == '.')) return false;
      if (!*cursor) break;
      segment = cursor + 1;
    }
  }
  return true;
}

bool host_path(const char *path, char *out, size_t capacity) {
  if (!valid_file_path(path) || !out || capacity == 0U) return false;
  if (strncmp(path, "/sd/", 4U) == 0) return copy_string(out, capacity, path);
  const size_t length = strlen(path);
  if (length + 3U >= capacity) return false;
  memcpy(out, "/sd", 3U);
  memcpy(out + 3U, path, length + 1U);
  return true;
}

bool require_mounted() {
  if (!g_status.mounted || !g_app || !g_storage) {
    set_error(PaperboyStorageError::NotMounted);
    return false;
  }
  return true;
}

bool append_extension(const char *path, const char *extension, bool replace,
                      char *out, size_t capacity) {
  if (!valid_file_path(path) || !has_rom_extension(path) || !extension || !out || !capacity) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  const char *dot = strrchr(path, '.');
  const size_t base = replace ? static_cast<size_t>(dot - path) : strlen(path);
  const size_t extra = strlen(extension);
  if (base + extra >= capacity || base + extra >= PAPERBOY_STORAGE_PATH_MAX) {
    set_error(PaperboyStorageError::PathTooLong);
    return false;
  }
  memcpy(out, path, base);
  memcpy(out + base, extension, extra + 1U);
  set_error(PaperboyStorageError::None);
  return true;
}

bool valid_config(const PaperboyStorageConfig &config) {
  return config.audio_engine < PAPERBOY_STORAGE_AUDIO_ENGINE_COUNT &&
      (config.last_rom[0] == '\0' ||
       (valid_file_path(config.last_rom) && has_rom_extension(config.last_rom)));
}

bool dir_open(const char *path) {
  bool opened = false;
  on_owner([&] { opened = g_app->dir_open(path); });
  if (opened) return true;
  ESP_LOGE(kTag, "dir_open failed path=%s", path ? path : "?");
  return false;
}

bool dir_next(gameboy_dirent_t *entry) {
  t5_app_dirent_t native{};
  bool found = false;
  on_owner([&] { found = g_app->dir_next(&native); });
  if (!found) return false;
  if (!copy_string(entry->name, sizeof(entry->name), native.name)) entry->name[0] = '\0';
  entry->size = native.size;
  entry->is_directory = native.is_directory;
  return true;
}

void dir_close() { on_owner([] { g_app->dir_close(); }); }

gameboy_stream_t stream_open(const char *path, size_t *size) {
  gameboy_stream_t result = GAMEBOY_INVALID_STREAM;
  on_owner([&] { result = g_storage->stream_open(path, size); });
  return result;
}
size_t stream_read(gameboy_stream_t stream, void *buffer, size_t capacity) {
  size_t result = 0U;
  on_owner([&] { result = g_storage->stream_read(stream, buffer, capacity); });
  return result;
}
void stream_close(gameboy_stream_t stream) {
  on_owner([&] { g_storage->stream_close(stream); });
}

const gameboy_rom_host_t kRomHost = {
    dir_open, dir_next, dir_close, stream_open, stream_read, stream_close, nullptr};

bool read_host_file(const char *path, void *buffer, size_t capacity, size_t &size_out) {
  size_out = 0U;
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) return false;
  const t5_storage_stream_t stream = stream_open(translated, &size_out);
  if (stream == T5_STORAGE_STREAM_INVALID) return false;
  if (size_out > capacity || (size_out && !buffer)) {
    stream_close(stream);
    return false;
  }
  size_t offset = 0U;
  while (offset < size_out) {
    const size_t remaining = size_out - offset;
    const size_t chunk = remaining < GAMEBOY_ROM_READ_CHUNK ? remaining : GAMEBOY_ROM_READ_CHUNK;
    const size_t count = stream_read(
        stream, static_cast<uint8_t *>(buffer) + offset, chunk);
    if (!count || count > chunk) { stream_close(stream); return false; }
    offset += count;
    yield();
  }
  stream_close(stream);
  return true;
}
}  // namespace

void paperboy_storage_owner_note_console_done() {
  __atomic_store_n(&g_console_done, true, __ATOMIC_RELEASE);
}

void paperboy_storage_owner_wait() {
  while (!__atomic_load_n(&g_console_done, __ATOMIC_ACQUIRE)) {
    OwnerRequest *request = __atomic_exchange_n(&g_owner_request, nullptr, __ATOMIC_ACQUIRE);
    if (request) {
      request->execute(request->context);
      __atomic_store_n(&request->done, true, __ATOMIC_RELEASE);
    } else {
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
    serial_flush();
  }
  serial_append("GameBoy console task finished");
  serial_flush(true);
}

void paperboy_serial_log(const char *message) {
  if (!message || !g_owner_task) return;
  on_owner([&] { serial_append(message); serial_flush(); });
}

void paperboy_storage_bind_host() {
  if (g_owner_task) return;
  g_owner_task = xTaskGetCurrentTaskHandle();
  __atomic_store_n(&g_console_done, false, __ATOMIC_RELEASE);
  if (!g_app) g_app = t5_app_get_api(T5_APP_ABI_VERSION);
  if (!g_storage) g_storage = t5_storage_get_api(T5_STORAGE_API_VERSION);
}

bool paperboy_storage_begin() {
  paperboy_storage_bind_host();
  if (g_status.mounted && g_app && g_storage) {
    ESP_LOGI(kTag, "reusing host SD mount ROMs=%u", (unsigned)g_status.rom_count);
    return g_scan_ok;
  }
  g_status = {};
  for (auto &rom : g_roms) rom = {};
  g_scan_ok = false;
  if (!g_app || g_app->struct_size < sizeof(t5_app_api_v1) ||
      !g_app->dir_open || !g_app->dir_next || !g_app->dir_close ||
      !g_storage || g_storage->struct_size < sizeof(t5_storage_api_v1) ||
      !g_storage->exists || !g_storage->read_file || !g_storage->write_file_atomic ||
      !g_storage->remove_file || !g_storage->stream_open || !g_storage->stream_read ||
      !g_storage->stream_close) {
    set_error(PaperboyStorageError::MountFailed);
    ESP_LOGE(kTag, "RiscRTE mounted-storage API unavailable");
    return false;
  }
  g_status.mounted = true;
  g_serial_used = 0;
  g_serial_dirty = false;
  serial_append("GameBoy ELF launch; SD ready; bounded 16 KiB rolling trace");
  serial_append("Firmware boot/serial output is not exposed to app ELF; this records GameBoy and USB provider state");
  serial_flush(true);
  g_scan_ok = paperboy_storage_rescan();
  serial_append(g_scan_ok ? "ROM catalog scan complete" : "ROM catalog scan failed");
  serial_flush(true);
  return g_scan_ok;
}

void paperboy_storage_end() {
  serial_append("GameBoy storage shutdown");
  serial_flush(true);
  if (g_app && g_app->dir_close) dir_close();
  g_status = {};
  for (auto &rom : g_roms) rom = {};
  g_scan_ok = false;
}

bool paperboy_storage_rescan() {
  if (!require_mounted()) return false;
  memset(&g_scan_catalog, 0, sizeof(g_scan_catalog));
  const gameboy_rom_result_t result = gameboy_rom_scan(&kRomHost, &g_scan_catalog);
  for (size_t i = 0; i < g_scan_catalog.count; ++i) {
    copy_string(g_roms[i].name, sizeof(g_roms[i].name), g_scan_catalog.roms[i].name);
    copy_string(g_roms[i].path, sizeof(g_roms[i].path), g_scan_catalog.roms[i].path);
    g_roms[i].size_bytes = g_scan_catalog.roms[i].size_bytes;
  }
  g_status.rom_count = g_scan_catalog.count;
  g_status.roms_truncated = g_scan_catalog.truncated;
  set_error(result == GAMEBOY_ROM_OK ? PaperboyStorageError::None : PaperboyStorageError::ScanFailed);
  ESP_LOGI(kTag, "using RiscRTE SD mount ROMs=%u result=%d%s",
           (unsigned)g_scan_catalog.count, static_cast<int>(result),
           g_scan_catalog.truncated ? "+" : "");
  return result == GAMEBOY_ROM_OK;
}

PaperboyStorageStatus paperboy_storage_status() { return g_status; }
PaperboyStorageError paperboy_storage_last_error() { return g_status.error; }

const char *paperboy_storage_error_string(PaperboyStorageError error) {
  static const char *const messages[] = {"ok", "storage not mounted", "SD mount failed",
      "SD card unavailable", "invalid argument", "path too long", "ROM scan failed",
      "file not found", "file open failed", "invalid file size", "file too large",
      "buffer too small", "allocation failed", "file read failed", "file write failed",
      "file rename failed", "invalid config"};
  const size_t index = static_cast<size_t>(error);
  return index < sizeof(messages) / sizeof(messages[0]) ? messages[index] : "unknown storage error";
}

const PaperboyRomInfo *paperboy_storage_rom(size_t index) {
  if (index >= g_status.rom_count) { set_error(PaperboyStorageError::InvalidArgument); return nullptr; }
  set_error(PaperboyStorageError::None);
  return &g_roms[index];
}

bool paperboy_storage_load_rom(const char *path, PaperboyRomData &out) {
  if (!require_mounted() || out.data || !valid_file_path(path) || !has_rom_extension(path)) {
    set_error(PaperboyStorageError::InvalidArgument); return false;
  }
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) { set_error(PaperboyStorageError::PathTooLong); return false; }
  size_t size = 0U;
  const gameboy_stream_t stream = stream_open(translated, &size);
  if (stream == GAMEBOY_INVALID_STREAM) { set_error(PaperboyStorageError::OpenFailed); return false; }
  stream_close(stream);
  if (!size || size > PAPERBOY_STORAGE_MAX_ROM_BYTES) {
    set_error(size ? PaperboyStorageError::FileTooLarge : PaperboyStorageError::InvalidFileSize); return false;
  }
  uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  bool psram = data != nullptr;
  if (!data) { data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); psram = false; }
  if (!data) { set_error(PaperboyStorageError::AllocationFailed); return false; }
  gameboy_rom_info_t info{};
  copy_string(info.path, sizeof(info.path), translated);
  info.size_bytes = static_cast<uint32_t>(size);
  const gameboy_rom_result_t result = gameboy_rom_read_exact(&kRomHost, &info, data, size);
  if (result != GAMEBOY_ROM_OK) { heap_caps_free(data); set_error(PaperboyStorageError::ReadFailed); return false; }
  out.data = data;
  out.size = size;
  out.in_psram = psram;
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_load_rom(size_t index, PaperboyRomData &out) {
  return index < g_status.rom_count
      ? paperboy_storage_load_rom(g_roms[index].path, out)
      : (set_error(PaperboyStorageError::InvalidArgument), false);
}

void paperboy_storage_free_rom(PaperboyRomData &rom) {
  if (rom.data) heap_caps_free(rom.data);
  rom = {};
}

void paperboy_storage_default_config(PaperboyStorageConfig &config) { config = {}; }

bool paperboy_storage_read_config(PaperboyStorageConfig &config) {
  paperboy_storage_default_config(config);
  if (!require_mounted()) return false;
  if (!host_exists(kConfigPath)) { set_error(PaperboyStorageError::None); return true; }
  char content[kConfigMaxBytes + 1U];
  size_t size = 0U;
  if (!read_host_file(kConfigPath, content, kConfigMaxBytes, size) || size > kConfigMaxBytes) {
    set_error(PaperboyStorageError::ConfigInvalid); return false;
  }
  content[size] = '\0';
  PaperboyStorageConfig parsed{};
  char *save = nullptr;
  for (char *line = strtok_r(content, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
    const size_t length = strlen(line);
    if (length && line[length - 1U] == '\r') line[length - 1U] = '\0';
    if (strncmp(line, "last_rom=", 9U) == 0) {
      if (!copy_string(parsed.last_rom, sizeof(parsed.last_rom), line + 9U)) {
        set_error(PaperboyStorageError::ConfigInvalid); return false;
      }
    } else if (strncmp(line, "audio_engine=", 13U) == 0) {
      char *end = nullptr;
      const long value = strtol(line + 13U, &end, 10);
      if (end == line + 13U || *end || value < 0 || value >= PAPERBOY_STORAGE_AUDIO_ENGINE_COUNT) {
        set_error(PaperboyStorageError::ConfigInvalid); return false;
      }
      parsed.audio_engine = static_cast<uint8_t>(value);
    }
  }
  if (!valid_config(parsed)) { set_error(PaperboyStorageError::ConfigInvalid); return false; }
  config = parsed;
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_write_config(const PaperboyStorageConfig &config) {
  if (!require_mounted() || !valid_config(config)) { set_error(PaperboyStorageError::ConfigInvalid); return false; }
  char content[PAPERBOY_STORAGE_PATH_MAX + 40U];
  const int count = config.last_rom[0]
      ? snprintf(content, sizeof(content), "last_rom=%s\naudio_engine=%u\n", config.last_rom, (unsigned)config.audio_engine)
      : snprintf(content, sizeof(content), "audio_engine=%u\n", (unsigned)config.audio_engine);
  if (count < 0 || static_cast<size_t>(count) >= sizeof(content)) {
    set_error(PaperboyStorageError::ConfigInvalid); return false;
  }
  return paperboy_storage_write_blob_atomic(kConfigPath, content, static_cast<size_t>(count));
}

bool paperboy_storage_make_save_path(const char *p, char *o, size_t n) { return append_extension(p, ".sav", false, o, n); }
bool paperboy_storage_make_state_path(const char *p, char *o, size_t n) { return append_extension(p, ".state", false, o, n); }
bool paperboy_storage_make_legacy_save_path(const char *p, char *o, size_t n) { return append_extension(p, ".sav", true, o, n); }
bool paperboy_storage_make_legacy_state_path(const char *p, char *o, size_t n) { return append_extension(p, ".state", true, o, n); }

bool paperboy_storage_file_exists(const char *path) {
  if (!require_mounted()) return false;
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) { set_error(PaperboyStorageError::InvalidArgument); return false; }
  set_error(PaperboyStorageError::None);
  return host_exists(translated);
}

bool paperboy_storage_file_size(const char *path, size_t &size) {
  size = 0U;
  if (!require_mounted()) return false;
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) { set_error(PaperboyStorageError::InvalidArgument); return false; }
  const t5_storage_stream_t stream = stream_open(translated, &size);
  if (stream == T5_STORAGE_STREAM_INVALID) { set_error(PaperboyStorageError::FileNotFound); return false; }
  stream_close(stream);
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_read_blob(const char *path, void *buffer, size_t capacity, size_t &size) {
  if (!require_mounted() || (!buffer && capacity)) { set_error(PaperboyStorageError::InvalidArgument); return false; }
  if (!read_host_file(path, buffer, capacity, size)) {
    set_error(size > capacity ? PaperboyStorageError::BufferTooSmall : PaperboyStorageError::ReadFailed);
    return false;
  }
  if (size > PAPERBOY_STORAGE_MAX_BLOB_BYTES) { set_error(PaperboyStorageError::FileTooLarge); return false; }
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_write_blob_atomic(const char *path, const void *data, size_t size) {
  if (!require_mounted() || (!data && size)) { set_error(PaperboyStorageError::InvalidArgument); return false; }
  if (size > PAPERBOY_STORAGE_MAX_BLOB_BYTES) { set_error(PaperboyStorageError::FileTooLarge); return false; }
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) { set_error(PaperboyStorageError::PathTooLong); return false; }
  if (!host_write_atomic(translated, data, size)) { set_error(PaperboyStorageError::WriteFailed); return false; }
  set_error(PaperboyStorageError::None);
  return true;
}

// Called by HID only on the app owner. Buffer diagnostics here; synchronous
// atomic SD writes between controller reports can stall delivery of releases.
// The owner's rate-limited maintenance pass and final flush persist both logs.
void paperboy_storage_hid_diagnostic(const char *message) {
  if (!message || !g_storage || !g_status.mounted ||
      xTaskGetCurrentTaskHandle() != g_owner_task) return;
  serial_append(message);
  if (g_hid_entries >= 32) return;
  const int n = snprintf(g_hid_trace + g_hid_used, sizeof(g_hid_trace) - g_hid_used, "%lu %.*s\n",
                         static_cast<unsigned long>(millis()), 160, message);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(g_hid_trace) - g_hid_used) return;
  g_hid_used += static_cast<size_t>(n);
  ++g_hid_entries;
  g_hid_dirty = true;
}
