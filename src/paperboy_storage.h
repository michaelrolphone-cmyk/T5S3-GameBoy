#pragma once

#include <stddef.h>
#include <stdint.h>
#include "paperboy_display_settings.h"

static constexpr size_t PAPERBOY_STORAGE_MAX_ROMS = 64U;
static constexpr size_t PAPERBOY_STORAGE_NAME_MAX = 128U;
static constexpr size_t PAPERBOY_STORAGE_PATH_MAX = 256U;
static constexpr size_t PAPERBOY_STORAGE_MAX_ROM_BYTES = 4U * 1024U * 1024U;
static constexpr size_t PAPERBOY_STORAGE_MAX_BLOB_BYTES = 8U * 1024U * 1024U;
static constexpr uint8_t PAPERBOY_STORAGE_AUDIO_ENGINE_COUNT = 3U;

enum class PaperboyStorageError : uint8_t {
  None,
  NotMounted,
  MountFailed,
  CardUnavailable,
  InvalidArgument,
  PathTooLong,
  ScanFailed,
  FileNotFound,
  OpenFailed,
  InvalidFileSize,
  FileTooLarge,
  BufferTooSmall,
  AllocationFailed,
  ReadFailed,
  WriteFailed,
  RenameFailed,
  ConfigInvalid,
};

struct PaperboyRomInfo {
  char name[PAPERBOY_STORAGE_NAME_MAX] = {0};
  char path[PAPERBOY_STORAGE_PATH_MAX] = {0};
  uint32_t size_bytes = 0;
};

struct PaperboyRomData {
  uint8_t *data = nullptr;
  size_t size = 0;
  bool in_psram = false;
};

struct PaperboyStorageConfig {
  char last_rom[PAPERBOY_STORAGE_PATH_MAX] = {0};
  uint8_t audio_engine = 0;
  uint8_t display_fps = PAPERBOY_DISPLAY_FPS_DEFAULT;
};

struct PaperboyStorageStatus {
  bool mounted = false;
  bool roms_truncated = false;
  PaperboyStorageError error = PaperboyStorageError::None;
  uint64_t card_capacity_bytes = 0;
  uint64_t filesystem_total_bytes = 0;
  uint64_t filesystem_used_bytes = 0;
  size_t rom_count = 0;
};

bool paperboy_storage_begin();
void paperboy_storage_end();
bool paperboy_storage_rescan();

PaperboyStorageStatus paperboy_storage_status();
PaperboyStorageError paperboy_storage_last_error();
const char *paperboy_storage_error_string(PaperboyStorageError error);
const PaperboyRomInfo *paperboy_storage_rom(size_t index);

bool paperboy_storage_load_rom(const char *path, PaperboyRomData &out_rom);
bool paperboy_storage_load_rom(size_t index, PaperboyRomData &out_rom);
void paperboy_storage_free_rom(PaperboyRomData &rom);

void paperboy_storage_default_config(PaperboyStorageConfig &config);
bool paperboy_storage_read_config(PaperboyStorageConfig &config);
bool paperboy_storage_write_config(const PaperboyStorageConfig &config);

bool paperboy_storage_make_save_path(
    const char *rom_path, char *out_path, size_t out_path_size);
bool paperboy_storage_make_state_path(
    const char *rom_path, char *out_path, size_t out_path_size);
bool paperboy_storage_make_legacy_save_path(
    const char *rom_path, char *out_path, size_t out_path_size);
bool paperboy_storage_make_legacy_state_path(
    const char *rom_path, char *out_path, size_t out_path_size);

bool paperboy_storage_file_exists(const char *path);
bool paperboy_storage_file_size(const char *path, size_t &out_size);
bool paperboy_storage_read_blob(
    const char *path, void *buffer, size_t buffer_size, size_t &out_size);
bool paperboy_storage_write_blob_atomic(
    const char *path, const void *data, size_t size);
