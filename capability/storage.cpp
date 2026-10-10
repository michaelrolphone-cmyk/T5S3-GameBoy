// Runtime volume boundary for the original Paperboy application. Public paths
// and rom_port retain their /sd spelling; only this file translates the mount.
#include "platform.hpp"
#include "load_trace.hpp"
#include "paperboy_storage.h"
#include "rom_port.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr char kConfigPath[] = "/sd/paperboy.cfg";
constexpr size_t kConfigMaxBytes = 1024U;
const risc_storage_volume_api_v1 *g_storage = nullptr;
const risc_storage_volume_api_v1_ext *g_volume = nullptr;
PaperboyRomInfo g_roms[PAPERBOY_STORAGE_MAX_ROMS];
PaperboyStorageStatus g_status;
bool g_scan_ok = false;
bool g_io_failed = false;
gameboy_rom_catalog_t g_scan_catalog;
risc_storage_dir_t g_directory = RISC_STORAGE_DIR_INVALID;
risc_storage_file_t g_reader = RISC_STORAGE_FILE_INVALID;
risc_storage_file_t g_writer = RISC_STORAGE_FILE_INVALID;
bool rom_read_active;
size_t rom_read_bytes,rom_read_calls;
uint32_t rom_read_started,rom_read_progress;

bool host_exists(const char *path);
bool host_write_atomic(const char *path, const void *data, size_t size);
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
  if (!cap_ready() || !g_status.mounted || !g_storage || !g_volume) {
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

bool make_system_sidecar_path(const char *rom_path, const char *extension,
                              char *out, size_t capacity) {
  if (!valid_file_path(rom_path) || !has_rom_extension(rom_path) ||
      !extension || !out || !capacity) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  const char *slash = strrchr(rom_path, '/');
  const char *filename = slash ? slash + 1 : rom_path;
  if (!filename[0]) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  const size_t root_length = strlen(GAMEBOY_STATE_ROM_DIRECTORY);
  const size_t name_length = strlen(filename);
  const size_t extension_length = strlen(extension);
  const size_t total = root_length + 1U + name_length + extension_length;
  if (total >= capacity || total >= PAPERBOY_STORAGE_PATH_MAX) {
    set_error(PaperboyStorageError::PathTooLong);
    return false;
  }
  memcpy(out, GAMEBOY_STATE_ROM_DIRECTORY, root_length);
  out[root_length] = '/';
  memcpy(out + root_length + 1U, filename, name_length);
  memcpy(out + root_length + 1U + name_length, extension, extension_length + 1U);
  set_error(PaperboyStorageError::None);
  return true;
}

bool make_legacy_sidecar_path(const char *rom_path, const char *extension,
                              char *out, size_t capacity) {
  char appended[PAPERBOY_STORAGE_PATH_MAX];
  if (append_extension(rom_path, extension, false, appended, sizeof(appended))) {
    char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
    if (host_path(appended, translated, sizeof(translated)) && host_exists(translated)) {
      return copy_string(out, capacity, appended);
    }
  } else if (paperboy_storage_last_error() != PaperboyStorageError::PathTooLong) {
    return false;
  }
  if (!cap_ready()) return false;
  return append_extension(rom_path, extension, true, out, capacity);
}

bool valid_config(const PaperboyStorageConfig &config) {
  return config.audio_engine < PAPERBOY_STORAGE_AUDIO_ENGINE_COUNT &&
      (config.last_rom[0] == '\0' ||
       (valid_file_path(config.last_rom) && has_rom_extension(config.last_rom)));
}


// Accept mount-qualified paths only at the provider boundary. Internal atomic
// suffixes fit the provider's larger path limit without changing public limits.
const char *volume_path(const char *path) {
  if (!path) return nullptr;
  if (strcmp(path, "/sd") == 0) return "/";
  if (strncmp(path, "/sd/", 4U) != 0) return nullptr;
  const char *relative = path + 3;
  const size_t length = bounded_length(relative, RISC_STORAGE_VOLUME_PATH_MAX);
  if (length < 2U || length >= RISC_STORAGE_VOLUME_PATH_MAX || relative[length - 1] == '/') return nullptr;
  const char *segment = relative + 1;
  for (const char *cursor = segment;; ++cursor) {
    const unsigned char ch = static_cast<unsigned char>(*cursor);
    if (ch == '\\' || (ch && ch < 0x20U)) return nullptr;
    if (!ch || ch == '/') {
      const size_t n = static_cast<size_t>(cursor - segment);
      if (!n || (n == 1 && segment[0] == '.') ||
          (n == 2 && segment[0] == '.' && segment[1] == '.')) return nullptr;
      if (!ch) return relative;
      segment = cursor + 1;
    }
  }
}

bool usable() { return cap_ready() && g_storage && g_volume; }
void retain(const char *operation) {
  g_io_failed = true;
  if (cap_ready()) cap_hold(operation);
}

bool host_stat(const char *path, uint64_t *size, bool *directory) {
  const char *relative = volume_path(path);
  if (!usable() || !relative) return false;
  // The volume contract requires both output pointers, even when a caller
  // only needs existence or one field. Copy requested fields only on success.
  uint64_t actual_size = 0;
  bool actual_directory = false;
  const bool result = g_storage->stat(g_storage->context, relative, &actual_size, &actual_directory);
  if (!cap_ready() || !result) return false;
  if (size) *size = actual_size;
  if (directory) *directory = actual_directory;
  return true;
}
bool host_exists(const char *path) { return host_stat(path, nullptr, nullptr); }

bool host_remove(const char *path) {
  const char *relative = volume_path(path);
  if (!usable() || !relative) return false;
  const bool result = g_storage->remove(g_storage->context, relative);
  return cap_ready() && result;
}
bool host_rename(const char *source, const char *destination) {
  const char *from = volume_path(source), *to = volume_path(destination);
  if (!usable() || !from || !to) return false;
  const bool result = g_volume->rename(g_storage->context, from, to);
  return cap_ready() && result;
}

bool close_file(risc_storage_file_t &file, bool commit) {
  if (!file) return cap_ready();
  if (!usable()) return false;
  if (!g_storage->file_close(g_storage->context, file, commit)) {
    retain("storage file close");
    return false; // Failed checked close retains the handle and provider.
  }
  if (!cap_ready()) return false;
  file = RISC_STORAGE_FILE_INVALID;
  return true;
}

bool dir_open(const char *path) {
  const char *relative = volume_path(path);
  if (!usable() || !relative || g_directory) { g_io_failed = true; return false; }
  g_directory = g_storage->dir_open(g_storage->context, relative);
  if (!cap_ready() || !g_directory) { g_io_failed = true; return false; }
  return true;
}
bool dir_next(gameboy_dirent_t *entry) {
  if (!usable() || !g_directory || !entry || g_io_failed) return false;
  risc_storage_dirent_v1 native{};
  const bool found = g_storage->dir_next(g_storage->context, g_directory, &native);
  if (!cap_ready()) return false;
  const uint32_t error = g_volume->handle_error(g_storage->context, g_directory, true);
  if (!cap_ready()) return false;
  if (error) { g_io_failed = true; return false; }
  if (!found) return false;
  if (!copy_string(entry->name, sizeof(entry->name), native.name)) entry->name[0] = '\0';
  entry->size = native.size;
  entry->is_directory = native.is_directory;
  cap_yield();
  return cap_ready();
}
void dir_close() {
  if (!g_directory || !usable()) return;
  if (!g_volume->dir_close_checked(g_storage->context, g_directory)) {
    retain("storage directory close");
    return;
  }
  if (cap_ready()) g_directory = RISC_STORAGE_DIR_INVALID;
}

gameboy_stream_t stream_open(const char *path, size_t *size) {
  const char *relative = volume_path(path);
  if (size) *size = 0;
  if (!usable() || !relative || g_reader) return GAMEBOY_INVALID_STREAM;
  uint64_t bytes = 0;
  g_reader = g_storage->file_open_read(g_storage->context, relative, &bytes);
  if (!cap_ready()) return GAMEBOY_INVALID_STREAM;
  if (bytes > SIZE_MAX) {
    g_io_failed = true;
    (void)close_file(g_reader, true);
    return GAMEBOY_INVALID_STREAM;
  }
  if (size) *size = static_cast<size_t>(bytes);
  return g_reader;
}
size_t stream_read(gameboy_stream_t stream, void *buffer, size_t capacity) {
  if (!usable() || !stream || stream != g_reader || !buffer || g_io_failed) return 0;
  const size_t chunk = capacity < RISC_STORAGE_VOLUME_IO_MAX ? capacity : RISC_STORAGE_VOLUME_IO_MAX;
  const size_t count = g_storage->file_read(g_storage->context, stream, buffer, chunk);
  if (!cap_ready()) return 0;
  if(rom_read_active){++rom_read_calls;rom_read_bytes+=count;}
  if (count > chunk) { retain("storage read count"); return 0; }
  const uint32_t error = g_volume->handle_error(g_storage->context, stream, false);
  if (!cap_ready()) return 0;
  if (error) { g_io_failed = true; return 0; }
  if(rom_read_active && rom_read_bytes-rom_read_progress>=256u*1024u){
    char detail[100];snprintf(detail,sizeof(detail),"bytes=%lu read_calls=%lu elapsed_ms=%lu",(unsigned long)rom_read_bytes,(unsigned long)rom_read_calls,(unsigned long)(cap_millis()-rom_read_started));
    cap_log("rom-read","progress",detail);rom_read_progress=(uint32_t)rom_read_bytes;
  }
  cap_yield();
  return cap_ready() ? count : 0;
}
void stream_close(gameboy_stream_t stream) {
  if (!stream || stream != g_reader) return;
  if (!close_file(g_reader, true)) g_io_failed = true;
}
const gameboy_rom_host_t kRomHost = {
    dir_open, dir_next, dir_close, stream_open, stream_read, stream_close, nullptr, host_exists};

bool atomic_paths(const char *path, char *temporary, char *backup) {
  if (!volume_path(path)) return false;
  const size_t n = bounded_length(path, RISC_STORAGE_VOLUME_PATH_MAX);
  if (n + 4U >= RISC_STORAGE_VOLUME_PATH_MAX) return false;
  memcpy(temporary, path, n); memcpy(temporary + n, ".tmp", 5U);
  memcpy(backup, path, n); memcpy(backup + n, ".bak", 5U);
  return true;
}

// These are the original .tmp/.bak recovery semantics. A backup is always the
// old exact target, never a basename-derived alternate save destination.
bool recover_atomic_target(const char *path) {
  char temporary[RISC_STORAGE_VOLUME_PATH_MAX], backup[RISC_STORAGE_VOLUME_PATH_MAX];
  if (!usable() || !atomic_paths(path, temporary, backup)) return false;
  bool target_directory = false, backup_directory = false, temporary_directory = false;
  const bool target = host_stat(path, nullptr, &target_directory);
  const bool old = host_stat(backup, nullptr, &backup_directory);
  const bool staged = host_stat(temporary, nullptr, &temporary_directory);
  if (!cap_ready() || target_directory || backup_directory || temporary_directory) return false;
  if (!target && old && !host_rename(backup, path)) return false;
  if (target && old && !host_remove(backup)) return false;
  if (staged && !host_remove(temporary)) return false;
  return cap_ready();
}

bool ensure_parent(const char *path) {
  char parent[RISC_STORAGE_VOLUME_PATH_MAX];
  if (!copy_string(parent, sizeof(parent), path) || !volume_path(parent)) return false;
  for (char *cursor = parent + 4; *cursor; ++cursor) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    bool directory = false;
    const bool exists = host_stat(parent, nullptr, &directory);
    if (!cap_ready()) return false;
    if (exists && !directory) return false;
    if (!exists) {
      if (!g_volume->mkdir(g_storage->context, volume_path(parent)) || !cap_ready()) return false;
    }
    *cursor = '/';
  }
  return true;
}

bool verify_contents(const char *path, const void *data, size_t size) {
  size_t actual = 0;
  const gameboy_stream_t stream = stream_open(path, &actual);
  if (!stream) return false;
  bool valid = actual == size;
  uint8_t buffer[512];
  size_t offset = 0;
  while (valid && offset < size) {
    const size_t remaining = size - offset;
    const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const size_t count = stream_read(stream, buffer, chunk);
    if (!count || memcmp(buffer, static_cast<const uint8_t *>(data) + offset, count) != 0) valid = false;
    else offset += count;
  }
  stream_close(stream);
  return valid && cap_ready() && !g_io_failed;
}

bool host_write_atomic(const char *path, const void *data, size_t size) {
  char temporary[RISC_STORAGE_VOLUME_PATH_MAX], backup[RISC_STORAGE_VOLUME_PATH_MAX];
  if (!usable() || g_writer || g_reader || g_directory ||
      !atomic_paths(path, temporary, backup) || !ensure_parent(path) ||
      !recover_atomic_target(path)) return false;
  g_io_failed = false;
  g_writer = g_storage->file_open_write(g_storage->context, volume_path(temporary));
  if (!cap_ready() || !g_writer) return false;
  bool written = true;
  size_t offset = 0;
  while (offset < size && written && cap_ready()) {
    const size_t remaining = size - offset;
    const size_t chunk = remaining < RISC_STORAGE_VOLUME_IO_MAX ? remaining : RISC_STORAGE_VOLUME_IO_MAX;
    const size_t count = g_storage->file_write(g_storage->context, g_writer,
        static_cast<const uint8_t *>(data) + offset, chunk);
    if (!cap_ready()) return false;
    if (count > chunk) { retain("storage write count"); return false; }
    const uint32_t error = g_volume->handle_error(g_storage->context, g_writer, false);
    if (!cap_ready()) return false;
    written = count != 0 && error == 0;
    offset += count;
    if (written) cap_yield();
  }
  if (!cap_ready()) return false;
  if (written) written = g_volume->file_sync(g_storage->context, g_writer);
  if (!cap_ready() || !close_file(g_writer, written)) return false;
  if (!written) return false;
  if (!verify_contents(temporary, data, size)) {
    if (cap_ready()) (void)host_remove(temporary);
    return false;
  }
  bool directory = false;
  const bool had_target = host_stat(path, nullptr, &directory);
  if (!cap_ready()) return false;
  if (directory || (had_target && !host_rename(path, backup))) {
    if (cap_ready()) (void)host_remove(temporary);
    return false;
  }
  if (!host_rename(temporary, path)) {
    if (!cap_ready()) return false;
    if (had_target && !host_rename(backup, path)) {
      retain("storage atomic rollback"); // Preserve both paths and all custody.
      return false;
    }
    (void)host_remove(temporary);
    return false;
  }
  if (had_target && !host_remove(backup)) return false;
  return cap_ready();
}

bool read_host_file(const char *path, void *buffer, size_t capacity, size_t &size_out) {
  size_out = 0U;
  g_io_failed = false;
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated)) || !recover_atomic_target(translated)) return false;
  const gameboy_stream_t stream = stream_open(translated, &size_out);
  if (stream == GAMEBOY_INVALID_STREAM) return false;
  if (size_out > capacity || size_out > PAPERBOY_STORAGE_MAX_BLOB_BYTES || (size_out && !buffer)) {
    stream_close(stream);
    return false;
  }
  size_t offset = 0U;
  while (offset < size_out) {
    const size_t count = stream_read(stream, static_cast<uint8_t *>(buffer) + offset, size_out - offset);
    if (!count) { stream_close(stream); return false; }
    offset += count;
  }
  stream_close(stream);
  return cap_ready() && !g_io_failed;
}
}  // namespace

void paperboy_serial_log(const char *message) {
  if (message && cap_ready()) cap_log("gameboy", "info", message);
}
void paperboy_storage_hid_diagnostic(const char *message) {
  if (message && cap_ready()) cap_log("hid", "info", message);
}

void paperboy_storage_bind_host() {
  if (!cap_ready() || g_storage) return;
  const risc_storage_volume_api_v1 *api = cap_storage();
  const risc_storage_volume_api_v1_ext *extension = risc_storage_volume_extension(api);
  if (!extension || !api->refresh || !api->ready || !api->stat ||
      !api->dir_open || !api->dir_next || !api->file_open_read || !api->file_read ||
      !api->file_open_write || !api->file_write || !api->file_close || !api->remove ||
      !extension->file_sync || !extension->dir_close_checked || !extension->handle_error ||
      !extension->mkdir || !extension->rename) return;
  g_storage = api;
  g_volume = extension;
}
bool paperboy_storage_begin() {
  if (!cap_ready()) return false;
  paperboy_storage_bind_host();
  if (g_status.mounted && usable()) return g_scan_ok;
  g_status = {};
  for (auto &rom : g_roms) rom = {};
  g_scan_ok = false;
  g_io_failed = false;
  if (!usable()) { set_error(PaperboyStorageError::MountFailed); return false; }
  const bool refreshed = g_storage->refresh(g_storage->context);
  if (!cap_ready() || !refreshed) { set_error(PaperboyStorageError::MountFailed); return false; }
  const bool ready = g_storage->ready(g_storage->context);
  if (!cap_ready() || !ready) { set_error(PaperboyStorageError::CardUnavailable); return false; }
  g_status.mounted = true;
  return paperboy_storage_rescan();
}
void paperboy_storage_end() {
  if (!cap_ready()) return;
  dir_close();
  if (!cap_ready() || !close_file(g_reader, true) || !close_file(g_writer, false)) return;
  g_status = {};
  for (auto &rom : g_roms) rom = {};
  g_scan_ok = false;
  g_io_failed = false;
  g_storage = nullptr;
  g_volume = nullptr;
}

bool paperboy_storage_rescan() {
  if (!require_mounted()) return false;
  CapLoadTrace trace("directory-scan");
  g_io_failed = false;
  memset(&g_scan_catalog, 0, sizeof(g_scan_catalog));
  const gameboy_rom_result_t result = gameboy_rom_scan(&kRomHost, &g_scan_catalog);
  if (!cap_ready() || g_io_failed) {
    g_scan_ok = false; set_error(PaperboyStorageError::ScanFailed); return false;
  }
  for (size_t i = 0; i < g_scan_catalog.count; ++i) {
    copy_string(g_roms[i].name, sizeof(g_roms[i].name), g_scan_catalog.roms[i].name);
    copy_string(g_roms[i].path, sizeof(g_roms[i].path), g_scan_catalog.roms[i].path);
    g_roms[i].size_bytes = g_scan_catalog.roms[i].size_bytes;
  }
  g_status.rom_count = g_scan_catalog.count;
  g_status.roms_truncated = g_scan_catalog.truncated;
  set_error(result == GAMEBOY_ROM_OK ? PaperboyStorageError::None : PaperboyStorageError::ScanFailed);
  g_scan_ok = result == GAMEBOY_ROM_OK;
  char detail[64];snprintf(detail,sizeof(detail),"roms=%lu truncated=%u",(unsigned long)g_status.rom_count,unsigned(g_status.roms_truncated));
  trace.end(g_scan_ok,int(result),detail);
  return g_scan_ok;
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
  CapLoadTrace trace("rom-load",path);
  if (!require_mounted() || out.data || !valid_file_path(path) || !has_rom_extension(path)) {
    set_error(PaperboyStorageError::InvalidArgument); return false;
  }
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) { set_error(PaperboyStorageError::PathTooLong); return false; }
  g_io_failed = false;
  size_t size = 0U;
  CapLoadTrace opened("rom-file-open",translated);
  const gameboy_stream_t stream = stream_open(translated, &size);
  if (stream == GAMEBOY_INVALID_STREAM) set_error(PaperboyStorageError::OpenFailed);
  char detail[100];snprintf(detail,sizeof(detail),"bytes=%lu",(unsigned long)size);opened.end(stream!=GAMEBOY_INVALID_STREAM,int(g_status.error),detail);
  if (stream == GAMEBOY_INVALID_STREAM) return false;
  stream_close(stream);
  if (!cap_ready() || g_io_failed) { set_error(PaperboyStorageError::ReadFailed); return false; }
  if (!size || size > PAPERBOY_STORAGE_MAX_ROM_BYTES) {
    set_error(size ? PaperboyStorageError::FileTooLarge : PaperboyStorageError::InvalidFileSize); return false;
  }
  CapLoadTrace allocated("rom-allocation",detail);
  uint8_t *data = static_cast<uint8_t *>(cap_alloc(size));
  allocated.end(data!=nullptr);
  if (!data) { set_error(PaperboyStorageError::AllocationFailed); return false; }
  gameboy_rom_info_t info{};
  copy_string(info.path, sizeof(info.path), translated);
  info.size_bytes = static_cast<uint32_t>(size);
  CapLoadTrace reading("rom-read",detail);
  rom_read_active=true;rom_read_bytes=rom_read_calls=rom_read_progress=0;rom_read_started=cap_millis();
  const gameboy_rom_result_t result = gameboy_rom_read_exact(&kRomHost, &info, data, size);
  rom_read_active=false;
  snprintf(detail,sizeof(detail),"bytes=%lu read_calls=%lu",(unsigned long)rom_read_bytes,(unsigned long)rom_read_calls);
  reading.end(result==GAMEBOY_ROM_OK&&!g_io_failed,int(result),detail);
  if (result != GAMEBOY_ROM_OK || !cap_ready() || g_io_failed) { if (cap_ready()) cap_free(data); set_error(PaperboyStorageError::ReadFailed); return false; }
  out.data = data;
  out.size = size;
  out.in_psram = false; // Runtime owns allocation placement; no physical heap claim.
  set_error(PaperboyStorageError::None);
  trace.end(true,0,detail);
  return true;
}

bool paperboy_storage_load_rom(size_t index, PaperboyRomData &out) {
  return index < g_status.rom_count
      ? paperboy_storage_load_rom(g_roms[index].path, out)
      : (set_error(PaperboyStorageError::InvalidArgument), false);
}

void paperboy_storage_free_rom(PaperboyRomData &rom) {
  if (!cap_ready()) return;
  if (rom.data) cap_free(rom.data);
  rom = {};
}

void paperboy_storage_default_config(PaperboyStorageConfig &config) { config = {}; }

bool paperboy_storage_read_config(PaperboyStorageConfig &config) {
  paperboy_storage_default_config(config);
  if (!require_mounted()) return false;
  if (!recover_atomic_target(kConfigPath)) return false;
  if (!host_exists(kConfigPath)) { if (!cap_ready()) return false; set_error(PaperboyStorageError::None); return true; }
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

bool paperboy_storage_make_save_path(const char *p, char *o, size_t n) {
  return make_system_sidecar_path(p, ".sav", o, n);
}
bool paperboy_storage_make_state_path(const char *p, char *o, size_t n) {
  return make_system_sidecar_path(p, ".state", o, n);
}
bool paperboy_storage_make_legacy_save_path(const char *p, char *o, size_t n) {
  return make_legacy_sidecar_path(p, ".sav", o, n);
}
bool paperboy_storage_make_legacy_state_path(const char *p, char *o, size_t n) {
  return make_legacy_sidecar_path(p, ".state", o, n);
}

bool paperboy_storage_file_exists(const char *path) {
  if (!require_mounted()) return false;
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) { set_error(PaperboyStorageError::InvalidArgument); return false; }
  set_error(PaperboyStorageError::None);
  return recover_atomic_target(translated) && host_exists(translated);
}

bool paperboy_storage_file_size(const char *path, size_t &size) {
  size = 0U;
  if (!require_mounted()) return false;
  char translated[PAPERBOY_STORAGE_PATH_MAX + 4U];
  if (!host_path(path, translated, sizeof(translated))) { set_error(PaperboyStorageError::InvalidArgument); return false; }
  g_io_failed = false;
  if (!recover_atomic_target(translated)) return false;
  const gameboy_stream_t stream = stream_open(translated, &size);
  if (stream == GAMEBOY_INVALID_STREAM) { set_error(PaperboyStorageError::FileNotFound); return false; }
  stream_close(stream);
  if (!cap_ready() || g_io_failed) { set_error(PaperboyStorageError::ReadFailed); return false; }
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_read_blob(const char *path, void *buffer, size_t capacity, size_t &size) {
  if (!require_mounted() || (!buffer && capacity)) { set_error(PaperboyStorageError::InvalidArgument); return false; }
  if (!read_host_file(path, buffer, capacity, size)) {
    set_error(size > PAPERBOY_STORAGE_MAX_BLOB_BYTES ? PaperboyStorageError::FileTooLarge : size > capacity ? PaperboyStorageError::BufferTooSmall : PaperboyStorageError::ReadFailed);
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

