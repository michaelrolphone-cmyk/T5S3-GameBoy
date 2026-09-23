#include "paperboy_storage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t5s3_epd_pins.h"

namespace {

constexpr char kTag[] = "storage";
constexpr char kConfigPath[] = "/paperboy.cfg";
constexpr char kMountPoint[] = "/sdcard";
constexpr char kTemporarySuffix[] = ".tmp";
constexpr char kBackupSuffix[] = ".bak";
constexpr size_t kAtomicSuffixBytes = sizeof(kTemporarySuffix) - 1U;
constexpr uint32_t kSdFrequencyHz = 4000000U;
constexpr uint8_t kSdMaxOpenFiles = 8U;
constexpr size_t kIoChunkBytes = 16U * 1024U;
constexpr size_t kVerifyChunkBytes = 512U;
constexpr size_t kConfigMaxBytes = 1024U;

static_assert(
    sizeof(kTemporarySuffix) == sizeof(kBackupSuffix),
    "atomic path suffixes must have equal lengths");

SPIClass g_sd_spi(FSPI);
PaperboyRomInfo g_roms[PAPERBOY_STORAGE_MAX_ROMS];
PaperboyStorageStatus g_status;

void set_error(PaperboyStorageError error) {
  g_status.error = error;
}

void set_error_if_none(PaperboyStorageError error) {
  if (g_status.error == PaperboyStorageError::None) {
    g_status.error = error;
  }
}

void clear_catalog() {
  memset(g_roms, 0, sizeof(g_roms));
  g_status.rom_count = 0;
  g_status.roms_truncated = false;
}

void refresh_capacity() {
  if (!g_status.mounted) {
    g_status.card_capacity_bytes = 0;
    g_status.filesystem_total_bytes = 0;
    g_status.filesystem_used_bytes = 0;
    return;
  }
  g_status.card_capacity_bytes = SD.cardSize();
  g_status.filesystem_total_bytes = SD.totalBytes();
  g_status.filesystem_used_bytes = SD.usedBytes();
}

unsigned char ascii_lower(unsigned char value) {
  return (value >= 'A' && value <= 'Z')
      ? static_cast<unsigned char>(value + ('a' - 'A'))
      : value;
}

int compare_case_insensitive(const char *left, const char *right) {
  while (*left != '\0' && *right != '\0') {
    const unsigned char left_char = ascii_lower(static_cast<unsigned char>(*left));
    const unsigned char right_char = ascii_lower(static_cast<unsigned char>(*right));
    if (left_char != right_char) {
      return left_char < right_char ? -1 : 1;
    }
    ++left;
    ++right;
  }
  if (*left == *right) {
    return 0;
  }
  return *left == '\0' ? -1 : 1;
}

int compare_roms(const PaperboyRomInfo &left, const PaperboyRomInfo &right) {
  int result = compare_case_insensitive(left.name, right.name);
  if (result == 0) {
    result = compare_case_insensitive(left.path, right.path);
  }
  if (result == 0) {
    result = strcmp(left.path, right.path);
  }
  return result;
}

size_t bounded_string_length(const char *value, size_t limit) {
  if (value == nullptr) {
    return limit;
  }
  size_t length = 0;
  while (length < limit && value[length] != '\0') {
    ++length;
  }
  return length;
}

bool copy_string(char *destination, size_t destination_size, const char *source) {
  if (destination == nullptr || destination_size == 0U || source == nullptr) {
    return false;
  }
  const size_t length = bounded_string_length(source, destination_size);
  if (length >= destination_size) {
    destination[0] = '\0';
    return false;
  }
  memcpy(destination, source, length + 1U);
  return true;
}

bool valid_file_path(const char *path) {
  if (path == nullptr || path[0] != '/') {
    return false;
  }
  const size_t length = bounded_string_length(path, PAPERBOY_STORAGE_PATH_MAX);
  if (length < 2U || length >= PAPERBOY_STORAGE_PATH_MAX || path[length - 1U] == '/') {
    return false;
  }

  const char *segment = path + 1;
  for (const char *cursor = segment;; ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (value == '\\' || (value != '\0' && value < 0x20U)) {
      return false;
    }
    if (*cursor == '/' || *cursor == '\0') {
      const size_t segment_length = static_cast<size_t>(cursor - segment);
      if (segment_length == 0U ||
          (segment_length == 1U && segment[0] == '.') ||
          (segment_length == 2U && segment[0] == '.' && segment[1] == '.')) {
        return false;
      }
      if (*cursor == '\0') {
        break;
      }
      segment = cursor + 1;
    }
  }
  return true;
}

bool has_rom_extension(const char *path) {
  if (path == nullptr) {
    return false;
  }
  const char *dot = strrchr(path, '.');
  if (dot == nullptr) {
    return false;
  }
  return compare_case_insensitive(dot, ".gb") == 0 ||
      compare_case_insensitive(dot, ".gbc") == 0;
}

bool make_suffixed_path(
    const char *path, const char *suffix, char *out_path, size_t out_path_size) {
  if (!valid_file_path(path) || suffix == nullptr || out_path == nullptr || out_path_size == 0U) {
    return false;
  }
  const size_t path_length = strlen(path);
  const size_t suffix_length = strlen(suffix);
  if (path_length + suffix_length >= out_path_size) {
    return false;
  }
  memcpy(out_path, path, path_length);
  memcpy(out_path + path_length, suffix, suffix_length + 1U);
  return true;
}

void insert_rom(const PaperboyRomInfo &candidate) {
  size_t count = g_status.rom_count;
  if (count == PAPERBOY_STORAGE_MAX_ROMS) {
    g_status.roms_truncated = true;
    if (compare_roms(candidate, g_roms[count - 1U]) >= 0) {
      return;
    }
    --count;
    g_status.rom_count = count;
  }

  size_t position = count;
  while (position > 0U && compare_roms(candidate, g_roms[position - 1U]) < 0) {
    g_roms[position] = g_roms[position - 1U];
    --position;
  }
  g_roms[position] = candidate;
  g_status.rom_count = count + 1U;
}

bool scan_directory(const char *directory_path, uint8_t remaining_depth) {
  File directory = SD.open(directory_path, FILE_READ);
  if (!directory || !directory.isDirectory()) {
    if (directory) {
      directory.close();
    }
    set_error_if_none(PaperboyStorageError::ScanFailed);
    return false;
  }

  bool success = true;
  while (true) {
    File entry = directory.openNextFile(FILE_READ);
    if (!entry) {
      break;
    }

    const char *name = entry.name();
    const char *path = entry.path();
    if (name == nullptr || path == nullptr || name[0] == '\0') {
      entry.close();
      set_error_if_none(PaperboyStorageError::ScanFailed);
      success = false;
      continue;
    }
    if (name[0] == '.') {
      entry.close();
      continue;
    }

    if (entry.isDirectory()) {
      if (remaining_depth > 0U) {
        char child_path[PAPERBOY_STORAGE_PATH_MAX];
        if (!copy_string(child_path, sizeof(child_path), path)) {
          set_error_if_none(PaperboyStorageError::PathTooLong);
          success = false;
        } else {
          entry.close();
          if (!scan_directory(child_path, static_cast<uint8_t>(remaining_depth - 1U))) {
            success = false;
          }
          continue;
        }
      }
      entry.close();
      continue;
    }

    const size_t file_size = entry.size();
    if (has_rom_extension(name) && file_size > 0U &&
        file_size <= PAPERBOY_STORAGE_MAX_ROM_BYTES) {
      PaperboyRomInfo candidate;
      if (!valid_file_path(path) ||
          !copy_string(candidate.name, sizeof(candidate.name), name) ||
          !copy_string(candidate.path, sizeof(candidate.path), path)) {
        set_error_if_none(PaperboyStorageError::PathTooLong);
        success = false;
      } else {
        candidate.size_bytes = static_cast<uint32_t>(file_size);
        insert_rom(candidate);
      }
    }
    entry.close();
  }

  directory.close();
  return success;
}

bool require_mounted() {
  if (!g_status.mounted) {
    set_error(PaperboyStorageError::NotMounted);
    return false;
  }
  return true;
}

bool read_exact(File &file, uint8_t *buffer, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const size_t remaining = size - offset;
    const size_t chunk = remaining < kIoChunkBytes ? remaining : kIoChunkBytes;
    const size_t read_size = file.read(buffer + offset, chunk);
    if (read_size == 0U) {
      return false;
    }
    offset += read_size;
    yield();
  }
  return true;
}

bool write_exact(File &file, const uint8_t *data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const size_t remaining = size - offset;
    const size_t chunk = remaining < kIoChunkBytes ? remaining : kIoChunkBytes;
    const size_t write_size = file.write(data + offset, chunk);
    if (write_size == 0U) {
      return false;
    }
    offset += write_size;
    yield();
  }
  return true;
}

bool verify_contents(File &file, const uint8_t *expected, size_t size) {
  uint8_t buffer[kVerifyChunkBytes];
  size_t offset = 0U;
  while (offset < size) {
    const size_t remaining = size - offset;
    const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    if (!read_exact(file, buffer, chunk) ||
        memcmp(buffer, expected + offset, chunk) != 0) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool recover_atomic_target(const char *path, bool require_clean = false) {
  char temporary_path[PAPERBOY_STORAGE_PATH_MAX];
  char backup_path[PAPERBOY_STORAGE_PATH_MAX];
  if (!make_suffixed_path(path, kTemporarySuffix, temporary_path, sizeof(temporary_path)) ||
      !make_suffixed_path(path, kBackupSuffix, backup_path, sizeof(backup_path))) {
    set_error(PaperboyStorageError::PathTooLong);
    return false;
  }

  bool target_exists = SD.exists(path);
  bool backup_exists = SD.exists(backup_path);
  if (!target_exists && backup_exists) {
    if (!SD.rename(backup_path, path)) {
      set_error(PaperboyStorageError::RenameFailed);
      return false;
    }
    target_exists = true;
    backup_exists = false;
  }
  if (target_exists && backup_exists && !SD.remove(backup_path)) {
    if (require_clean) {
      set_error(PaperboyStorageError::WriteFailed);
      return false;
    }
    ESP_LOGW(kTag, "Could not remove stale backup: %s", backup_path);
  }
  if (SD.exists(temporary_path) && !SD.remove(temporary_path)) {
    if (require_clean) {
      set_error(PaperboyStorageError::WriteFailed);
      return false;
    }
    ESP_LOGW(kTag, "Could not remove stale temporary file: %s", temporary_path);
  }
  return true;
}

bool append_sidecar_extension(
    const char *rom_path,
    const char *extension,
    char *out_path,
    size_t out_path_size) {
  if (!valid_file_path(rom_path) || !has_rom_extension(rom_path) ||
      extension == nullptr || out_path == nullptr || out_path_size == 0U) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }

  const size_t base_length = strlen(rom_path);
  const size_t extension_length = strlen(extension);
  const size_t sidecar_length = base_length + extension_length;
  if (sidecar_length + kAtomicSuffixBytes >= out_path_size ||
      sidecar_length + kAtomicSuffixBytes >= PAPERBOY_STORAGE_PATH_MAX) {
    set_error(PaperboyStorageError::PathTooLong);
    return false;
  }

  char result[PAPERBOY_STORAGE_PATH_MAX];
  memcpy(result, rom_path, base_length);
  memcpy(result + base_length, extension, extension_length + 1U);
  memcpy(out_path, result, sidecar_length + 1U);
  set_error(PaperboyStorageError::None);
  return true;
}

bool replace_rom_extension(
    const char *rom_path,
    const char *extension,
    char *out_path,
    size_t out_path_size) {
  if (!valid_file_path(rom_path) || !has_rom_extension(rom_path) ||
      extension == nullptr || out_path == nullptr || out_path_size == 0U) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }

  const char *dot = strrchr(rom_path, '.');
  const size_t base_length = static_cast<size_t>(dot - rom_path);
  const size_t extension_length = strlen(extension);
  const size_t sidecar_length = base_length + extension_length;
  if (sidecar_length + kAtomicSuffixBytes >= out_path_size ||
      sidecar_length + kAtomicSuffixBytes >= PAPERBOY_STORAGE_PATH_MAX) {
    set_error(PaperboyStorageError::PathTooLong);
    return false;
  }

  memcpy(out_path, rom_path, base_length);
  memcpy(out_path + base_length, extension, extension_length + 1U);
  set_error(PaperboyStorageError::None);
  return true;
}

void normalize_legacy_mount_path(char *path, size_t path_size) {
  constexpr char kLegacyPrefix[] = "/sdcard/";
  constexpr size_t kMountLength = sizeof(kMountPoint) - 1U;
  if (path == nullptr || path_size == 0U ||
      strncmp(path, kLegacyPrefix, sizeof(kLegacyPrefix) - 1U) != 0) {
    return;
  }

  const char *relative_path = path + kMountLength;
  if (SD.exists(path) || !SD.exists(relative_path)) {
    return;
  }
  memmove(path, relative_path, strlen(relative_path) + 1U);
}

bool valid_config(const PaperboyStorageConfig &config) {
  if (config.audio_engine >= PAPERBOY_STORAGE_AUDIO_ENGINE_COUNT ||
      !paperboy_display_fps_valid(config.display_fps)) {
    return false;
  }
  return config.last_rom[0] == '\0' ||
      (valid_file_path(config.last_rom) && has_rom_extension(config.last_rom));
}

}  // namespace

bool paperboy_storage_begin() {
  if (g_status.mounted) {
    refresh_capacity();
    return paperboy_storage_rescan();
  }

  g_status = {};
  clear_catalog();

  pinMode(t5s3_epd::kLoraCs, OUTPUT);
  digitalWrite(t5s3_epd::kLoraCs, HIGH);
  pinMode(t5s3_epd::kSdCs, OUTPUT);
  digitalWrite(t5s3_epd::kSdCs, HIGH);
  pinMode(t5s3_epd::kSdMiso, INPUT_PULLUP);

  g_sd_spi.begin(
      t5s3_epd::kSdSck,
      t5s3_epd::kSdMiso,
      t5s3_epd::kSdMosi,
      t5s3_epd::kSdCs);
  if (!SD.begin(
          t5s3_epd::kSdCs,
          g_sd_spi,
          kSdFrequencyHz,
          kMountPoint,
          kSdMaxOpenFiles,
          false)) {
    g_sd_spi.end();
    set_error(PaperboyStorageError::MountFailed);
    ESP_LOGW(kTag, "SD mount failed");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    SD.end();
    g_sd_spi.end();
    set_error(PaperboyStorageError::CardUnavailable);
    ESP_LOGW(kTag, "SD card is unavailable");
    return false;
  }

  g_status.mounted = true;
  refresh_capacity();
  const bool scan_ok = paperboy_storage_rescan();
  ESP_LOGI(
      kTag,
      "SD mounted capacity=%llu bytes ROMs=%u%s",
      static_cast<unsigned long long>(g_status.card_capacity_bytes),
      static_cast<unsigned>(g_status.rom_count),
      g_status.roms_truncated ? "+" : "");
  return scan_ok;
}

void paperboy_storage_end() {
  if (g_status.mounted) {
    SD.end();
    g_sd_spi.end();
  }
  g_status = {};
  clear_catalog();
}

bool paperboy_storage_rescan() {
  if (!require_mounted()) {
    return false;
  }
  clear_catalog();
  set_error(PaperboyStorageError::None);
  const bool success = scan_directory("/", 1U);
  if (!success && g_status.error == PaperboyStorageError::None) {
    set_error(PaperboyStorageError::ScanFailed);
  }
  return success;
}

PaperboyStorageStatus paperboy_storage_status() {
  return g_status;
}

PaperboyStorageError paperboy_storage_last_error() {
  return g_status.error;
}

const char *paperboy_storage_error_string(PaperboyStorageError error) {
  switch (error) {
    case PaperboyStorageError::None:
      return "ok";
    case PaperboyStorageError::NotMounted:
      return "storage not mounted";
    case PaperboyStorageError::MountFailed:
      return "SD mount failed";
    case PaperboyStorageError::CardUnavailable:
      return "SD card unavailable";
    case PaperboyStorageError::InvalidArgument:
      return "invalid argument";
    case PaperboyStorageError::PathTooLong:
      return "path too long";
    case PaperboyStorageError::ScanFailed:
      return "ROM scan failed";
    case PaperboyStorageError::FileNotFound:
      return "file not found";
    case PaperboyStorageError::OpenFailed:
      return "file open failed";
    case PaperboyStorageError::InvalidFileSize:
      return "invalid file size";
    case PaperboyStorageError::FileTooLarge:
      return "file too large";
    case PaperboyStorageError::BufferTooSmall:
      return "buffer too small";
    case PaperboyStorageError::AllocationFailed:
      return "allocation failed";
    case PaperboyStorageError::ReadFailed:
      return "file read failed";
    case PaperboyStorageError::WriteFailed:
      return "file write failed";
    case PaperboyStorageError::RenameFailed:
      return "file rename failed";
    case PaperboyStorageError::ConfigInvalid:
      return "invalid config";
    default:
      return "unknown storage error";
  }
}

const PaperboyRomInfo *paperboy_storage_rom(size_t index) {
  if (index >= g_status.rom_count) {
    set_error(PaperboyStorageError::InvalidArgument);
    return nullptr;
  }
  set_error(PaperboyStorageError::None);
  return &g_roms[index];
}

bool paperboy_storage_load_rom(const char *path, PaperboyRomData &out_rom) {
  if (!require_mounted()) {
    return false;
  }
  if (out_rom.data != nullptr || !valid_file_path(path) || !has_rom_extension(path)) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }

  out_rom = {};
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    set_error(SD.exists(path)
                  ? PaperboyStorageError::OpenFailed
                  : PaperboyStorageError::FileNotFound);
    return false;
  }

  const size_t size = file.size();
  if (size == 0U) {
    file.close();
    set_error(PaperboyStorageError::InvalidFileSize);
    return false;
  }
  if (size > PAPERBOY_STORAGE_MAX_ROM_BYTES) {
    file.close();
    set_error(PaperboyStorageError::FileTooLarge);
    return false;
  }

  uint8_t *data = static_cast<uint8_t *>(
      heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  bool in_psram = data != nullptr;
  if (data == nullptr) {
    data = static_cast<uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    in_psram = false;
  }
  if (data == nullptr) {
    file.close();
    set_error(PaperboyStorageError::AllocationFailed);
    return false;
  }

  const bool read_ok = read_exact(file, data, size);
  file.close();
  if (!read_ok) {
    heap_caps_free(data);
    set_error(PaperboyStorageError::ReadFailed);
    return false;
  }

  out_rom.data = data;
  out_rom.size = size;
  out_rom.in_psram = in_psram;
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_load_rom(size_t index, PaperboyRomData &out_rom) {
  if (index >= g_status.rom_count) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  return paperboy_storage_load_rom(g_roms[index].path, out_rom);
}

void paperboy_storage_free_rom(PaperboyRomData &rom) {
  if (rom.data != nullptr) {
    heap_caps_free(rom.data);
  }
  rom = {};
}

void paperboy_storage_default_config(PaperboyStorageConfig &config) {
  config = {};
  config.audio_engine = 0U;
}

bool paperboy_storage_read_config(PaperboyStorageConfig &config) {
  paperboy_storage_default_config(config);
  if (!require_mounted()) {
    return false;
  }
  if (!recover_atomic_target(kConfigPath)) {
    return false;
  }
  if (!SD.exists(kConfigPath)) {
    set_error(PaperboyStorageError::None);
    return true;
  }

  File file = SD.open(kConfigPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    set_error(PaperboyStorageError::OpenFailed);
    return false;
  }
  const size_t size = file.size();
  if (size > kConfigMaxBytes) {
    file.close();
    set_error(PaperboyStorageError::ConfigInvalid);
    return false;
  }

  char content[kConfigMaxBytes + 1U];
  const bool read_ok = read_exact(file, reinterpret_cast<uint8_t *>(content), size);
  file.close();
  if (!read_ok) {
    set_error(PaperboyStorageError::ReadFailed);
    return false;
  }
  content[size] = '\0';

  PaperboyStorageConfig parsed;
  paperboy_storage_default_config(parsed);
  char *save_pointer = nullptr;
  for (char *line = strtok_r(content, "\n", &save_pointer);
       line != nullptr;
       line = strtok_r(nullptr, "\n", &save_pointer)) {
    const size_t line_length = strlen(line);
    if (line_length > 0U && line[line_length - 1U] == '\r') {
      line[line_length - 1U] = '\0';
    }
    if (strncmp(line, "last_rom=", 9U) == 0) {
      const char *value = line + 9U;
      if (!copy_string(parsed.last_rom, sizeof(parsed.last_rom), value)) {
        set_error(PaperboyStorageError::ConfigInvalid);
        return false;
      }
    } else if (strncmp(line, "audio_engine=", 13U) == 0) {
      char *end = nullptr;
      const long value = strtol(line + 13U, &end, 10);
      if (end == line + 13U || *end != '\0' || value < 0L ||
          value >= PAPERBOY_STORAGE_AUDIO_ENGINE_COUNT) {
        set_error(PaperboyStorageError::ConfigInvalid);
        return false;
      }
      parsed.audio_engine = static_cast<uint8_t>(value);
    } else if (strncmp(line, "display_fps=", 12U) == 0) {
      char *end = nullptr;
      const long value = strtol(line + 12U, &end, 10);
      parsed.display_fps = end != line + 12U && *end == '\0' && paperboy_display_fps_valid(value)
          ? static_cast<uint8_t>(value) : PAPERBOY_DISPLAY_FPS_DEFAULT;
    }
  }

  if (!valid_config(parsed)) {
    set_error(PaperboyStorageError::ConfigInvalid);
    return false;
  }
  normalize_legacy_mount_path(parsed.last_rom, sizeof(parsed.last_rom));
  config = parsed;
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_write_config(const PaperboyStorageConfig &config) {
  if (!require_mounted()) {
    return false;
  }
  if (!valid_config(config)) {
    set_error(PaperboyStorageError::ConfigInvalid);
    return false;
  }

  char content[PAPERBOY_STORAGE_PATH_MAX + 64U];
  const int written = config.last_rom[0] == '\0'
      ? snprintf(
            content,
            sizeof(content),
            "audio_engine=%u\ndisplay_fps=%u\n",
            static_cast<unsigned>(config.audio_engine), static_cast<unsigned>(config.display_fps))
      : snprintf(
            content,
            sizeof(content),
            "last_rom=%s\naudio_engine=%u\ndisplay_fps=%u\n",
            config.last_rom,
            static_cast<unsigned>(config.audio_engine), static_cast<unsigned>(config.display_fps));
  if (written < 0 || static_cast<size_t>(written) >= sizeof(content)) {
    set_error(PaperboyStorageError::ConfigInvalid);
    return false;
  }
  return paperboy_storage_write_blob_atomic(
      kConfigPath, content, static_cast<size_t>(written));
}

bool paperboy_storage_make_save_path(
    const char *rom_path, char *out_path, size_t out_path_size) {
  return append_sidecar_extension(rom_path, ".sav", out_path, out_path_size);
}

bool paperboy_storage_make_state_path(
    const char *rom_path, char *out_path, size_t out_path_size) {
  return append_sidecar_extension(rom_path, ".state", out_path, out_path_size);
}

bool paperboy_storage_make_legacy_save_path(
    const char *rom_path, char *out_path, size_t out_path_size) {
  return replace_rom_extension(rom_path, ".sav", out_path, out_path_size);
}

bool paperboy_storage_make_legacy_state_path(
    const char *rom_path, char *out_path, size_t out_path_size) {
  return replace_rom_extension(rom_path, ".state", out_path, out_path_size);
}

bool paperboy_storage_file_exists(const char *path) {
  if (!require_mounted()) {
    return false;
  }
  if (!valid_file_path(path)) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  if (!recover_atomic_target(path)) {
    return false;
  }
  set_error(PaperboyStorageError::None);
  return SD.exists(path);
}

bool paperboy_storage_file_size(const char *path, size_t &out_size) {
  out_size = 0U;
  if (!require_mounted()) {
    return false;
  }
  if (!valid_file_path(path)) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  if (!recover_atomic_target(path)) {
    return false;
  }

  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    set_error(SD.exists(path)
                  ? PaperboyStorageError::OpenFailed
                  : PaperboyStorageError::FileNotFound);
    return false;
  }
  out_size = file.size();
  file.close();
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_read_blob(
    const char *path, void *buffer, size_t buffer_size, size_t &out_size) {
  out_size = 0U;
  if (!require_mounted()) {
    return false;
  }
  if (!valid_file_path(path) || (buffer == nullptr && buffer_size != 0U)) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  if (!recover_atomic_target(path)) {
    return false;
  }

  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    set_error(SD.exists(path)
                  ? PaperboyStorageError::OpenFailed
                  : PaperboyStorageError::FileNotFound);
    return false;
  }

  const size_t size = file.size();
  out_size = size;
  if (size > PAPERBOY_STORAGE_MAX_BLOB_BYTES) {
    file.close();
    set_error(PaperboyStorageError::FileTooLarge);
    return false;
  }
  if (size > buffer_size || (size > 0U && buffer == nullptr)) {
    file.close();
    set_error(PaperboyStorageError::BufferTooSmall);
    return false;
  }

  const bool success = size == 0U || read_exact(
      file, static_cast<uint8_t *>(buffer), size);
  file.close();
  if (!success) {
    set_error(PaperboyStorageError::ReadFailed);
    return false;
  }
  set_error(PaperboyStorageError::None);
  return true;
}

bool paperboy_storage_write_blob_atomic(
    const char *path, const void *data, size_t size) {
  if (!require_mounted()) {
    return false;
  }
  if (!valid_file_path(path) || (data == nullptr && size != 0U)) {
    set_error(PaperboyStorageError::InvalidArgument);
    return false;
  }
  if (size > PAPERBOY_STORAGE_MAX_BLOB_BYTES) {
    set_error(PaperboyStorageError::FileTooLarge);
    return false;
  }

  char temporary_path[PAPERBOY_STORAGE_PATH_MAX];
  char backup_path[PAPERBOY_STORAGE_PATH_MAX];
  if (!make_suffixed_path(path, kTemporarySuffix, temporary_path, sizeof(temporary_path)) ||
      !make_suffixed_path(path, kBackupSuffix, backup_path, sizeof(backup_path))) {
    set_error(PaperboyStorageError::PathTooLong);
    return false;
  }
  if (!recover_atomic_target(path, true)) {
    return false;
  }

  File file = SD.open(temporary_path, FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    set_error(PaperboyStorageError::OpenFailed);
    return false;
  }
  const bool write_ok = size == 0U || write_exact(
      file, static_cast<const uint8_t *>(data), size);
  file.flush();
  file.close();
  if (!write_ok) {
    SD.remove(temporary_path);
    set_error(PaperboyStorageError::WriteFailed);
    return false;
  }

  File verify = SD.open(temporary_path, FILE_READ);
  const bool verified = verify && !verify.isDirectory() &&
      verify.size() == size &&
      verify_contents(
          verify, static_cast<const uint8_t *>(data), size);
  if (verify) {
    verify.close();
  }
  if (!verified) {
    SD.remove(temporary_path);
    set_error(PaperboyStorageError::WriteFailed);
    return false;
  }

  const bool had_target = SD.exists(path);
  if (had_target && !SD.rename(path, backup_path)) {
    SD.remove(temporary_path);
    set_error(PaperboyStorageError::RenameFailed);
    return false;
  }
  if (!SD.rename(temporary_path, path)) {
    if (had_target) {
      (void)SD.rename(backup_path, path);
    }
    SD.remove(temporary_path);
    set_error(PaperboyStorageError::RenameFailed);
    return false;
  }
  if (had_target && SD.exists(backup_path)) {
    (void)SD.remove(backup_path);
  }

  refresh_capacity();
  set_error(PaperboyStorageError::None);
  return true;
}
