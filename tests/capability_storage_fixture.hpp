#pragma once
// Include from one fixture translation unit. Paths exposed to the fake volume
// are root-relative. Optional callbacks connect terminal faults to a real
// backend and assert that it makes no volume call after retaining custody.
#include <RiscStorageVolumeV1.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace capability_storage_fixture {
struct File { std::string path; size_t offset; bool writer; };
struct Directory { std::vector<risc_storage_dirent_v1> entries; size_t offset; };
std::map<std::string, std::vector<uint8_t>> files;
std::set<std::string> directories{"/"};
std::map<uint32_t, File> handles;
std::map<uint32_t, Directory> dirs;
std::set<void *> allocations;
risc_storage_volume_api_v1_ext api{};
bool (*admitted)() = nullptr;
void (*on_terminal)() = nullptr;
bool held = false, fail_refresh = false, media_ready = true;
bool fail_dir_close = false, fail_file_close = false, fail_sync = false;
bool fail_read = false, fail_write = false, over_read = false, over_write = false;
bool directory_error = false, corrupt_verify = false, fail_remove = false;
bool fail_alloc = false;
size_t partial = SIZE_MAX, max_read = 0, max_write = 0;
unsigned calls = 0, hold_count = 0, yields = 0, logs = 0, closes = 0;
unsigned next_handle = 1, renames = 0, terminal_at = 0, terminal_yield = 0;
std::set<unsigned> fail_rename_calls;
std::vector<std::string> trace;

bool invoke(const char *operation, const char *path = nullptr) {
  assert(!held && (!admitted || admitted()));
  ++calls;
  if (path) {
    assert(path[0] == '/');
    assert(std::strncmp(path, "/sd/", 4) != 0 && std::strcmp(path, "/sd") != 0);
    assert(std::strstr(path, "//") == nullptr && std::strstr(path, "/../") == nullptr);
  }
  trace.emplace_back(std::string(operation) + (path ? path : ""));
  if (terminal_at && calls == terminal_at) { held = true; if (on_terminal) on_terminal(); return false; }
  return true;
}
std::string parent(const std::string &path) {
  const size_t slash = path.rfind('/');
  return slash == 0 ? "/" : path.substr(0, slash);
}
void add_file(const std::string &path, size_t size, uint8_t byte = 0x5a) {
  files[path] = std::vector<uint8_t>(size, byte);
  std::string path_parent = parent(path);
  while (directories.insert(path_parent).second && path_parent != "/") path_parent = parent(path_parent);
}
bool refresh(void *) { return invoke("refresh") && !fail_refresh; }
bool ready(void *) { return invoke("ready") && media_ready; }
bool stat_file(void *, const char *path, uint64_t *size, bool *directory) {
  if (!size || !directory || !invoke("stat", path)) return false;
  if (size) *size = 0;
  if (directory) *directory = false;
  if (directories.count(path)) { if (directory) *directory = true; return true; }
  const auto it = files.find(path);
  if (it == files.end()) return false;
  if (size) *size = it->second.size();
  return true;
}
uint32_t dir_open(void *, const char *path) {
  if (!invoke("dir_open", path) || !directories.count(path)) return 0;
  Directory directory{};
  auto append = [&](const std::string &name, bool folder, size_t size) {
    risc_storage_dirent_v1 entry{};
    assert(name.size() < sizeof(entry.name));
    std::strcpy(entry.name, name.c_str()); entry.size = size; entry.is_directory = folder;
    directory.entries.push_back(entry);
  };
  for (const auto &folder : directories)
    if (folder != "/" && parent(folder) == path) append(folder.substr(folder.rfind('/') + 1), true, 0);
  for (const auto &file : files)
    if (parent(file.first) == path) append(file.first.substr(file.first.rfind('/') + 1), false, file.second.size());
  // Reverse provider order to prove rom_port owns stable sorting and truncation.
  std::reverse(directory.entries.begin(), directory.entries.end());
  const uint32_t handle = next_handle++;
  dirs.emplace(handle, directory);
  return handle;
}
bool dir_next(void *, uint32_t handle, risc_storage_dirent_v1 *entry) {
  if (!invoke("dir_next")) return false;
  auto &directory = dirs.at(handle);
  if (directory_error || directory.offset == directory.entries.size()) return false;
  *entry = directory.entries[directory.offset++]; return true;
}
bool dir_close(void *, uint32_t handle) {
  if (!invoke("dir_close") || fail_dir_close) return false;
  assert(dirs.erase(handle) == 1); return true;
}
uint32_t open_read(void *, const char *path, uint64_t *size) {
  if (!invoke("open_read", path) || !files.count(path)) return 0;
  if (size) *size = files.at(path).size();
  const uint32_t handle = next_handle++;
  handles.emplace(handle, File{path, 0, false}); return handle;
}
size_t read_file(void *, uint32_t handle, void *buffer, size_t capacity) {
  assert(capacity <= RISC_STORAGE_VOLUME_IO_MAX);
  max_read = std::max(max_read, capacity);
  if (!invoke("read") || fail_read) return 0;
  if (over_read) return capacity + 1;
  auto &file = handles.at(handle);
  assert(!file.writer);
  const auto &bytes = files.at(file.path);
  const size_t count = std::min(std::min(capacity, bytes.size() - file.offset), partial);
  if (count) std::memcpy(buffer, bytes.data() + file.offset, count);
  if (corrupt_verify && file.path.find(".tmp") != std::string::npos && count)
    static_cast<uint8_t *>(buffer)[0] ^= 1;
  file.offset += count; return count;
}
uint32_t open_write(void *, const char *path) {
  if (!invoke("open_write", path) || files.count(path) || directories.count(path) || !directories.count(parent(path))) return 0;
  files[path] = {};
  const uint32_t handle = next_handle++;
  handles.emplace(handle, File{path, 0, true}); return handle;
}
size_t write_file(void *, uint32_t handle, const void *data, size_t size) {
  assert(size <= RISC_STORAGE_VOLUME_IO_MAX);
  max_write = std::max(max_write, size);
  if (!invoke("write") || fail_write) return 0;
  if (over_write) return size + 1;
  auto &file = handles.at(handle);
  assert(file.writer);
  const size_t count = std::min(size, partial);
  const auto *bytes = static_cast<const uint8_t *>(data);
  files.at(file.path).insert(files.at(file.path).end(), bytes, bytes + count);
  file.offset += count; return count;
}
bool close_file(void *, uint32_t handle, bool commit) {
  if (!invoke("file_close") || fail_file_close) return false;
  ++closes;
  const auto file = handles.at(handle);
  if (file.writer && !commit) files.erase(file.path);
  handles.erase(handle); return true;
}
bool remove_file(void *, const char *path) {
  if (!invoke("remove", path) || fail_remove) return false;
  return files.erase(path) == 1;
}
bool sync_file(void *, uint32_t handle) {
  if (!invoke("sync")) return false;
  assert(handles.count(handle)); return !fail_sync;
}
uint32_t handle_error(void *, uint32_t handle, bool directory) {
  if (!invoke("handle_error")) return 1;
  if (directory) { assert(dirs.count(handle)); return directory_error ? 1 : 0; }
  assert(handles.count(handle)); return fail_read || fail_write ? 1 : 0;
}
bool mkdir(void *, const char *path) {
  if (!invoke("mkdir", path) || files.count(path) || !directories.count(parent(path))) return false;
  return directories.insert(path).second;
}
bool rename_file(void *, const char *source, const char *destination) {
  if (!invoke("rename", source)) return false;
  assert(destination[0] == '/' && std::strncmp(destination, "/sd/", 4) != 0);
  ++renames;
  if (fail_rename_calls.count(renames) || !files.count(source) || files.count(destination) || directories.count(destination)) return false;
  files[destination] = files.at(source); files.erase(source); return true;
}
void setup() {
  api.base.api_version = RISC_STORAGE_VOLUME_API_V1;
  api.base.struct_size = sizeof(api);
  api.base.refresh = refresh; api.base.ready = ready; api.base.stat = stat_file;
  api.base.dir_open = dir_open; api.base.dir_next = dir_next;
  api.base.file_open_read = open_read; api.base.file_read = read_file;
  api.base.file_open_write = open_write; api.base.file_write = write_file;
  api.base.file_close = close_file; api.base.remove = remove_file;
  api.file_sync = sync_file; api.dir_close_checked = dir_close;
  api.handle_error = handle_error; api.mkdir = mkdir; api.rename = rename_file;
}
} // namespace capability_storage_fixture
