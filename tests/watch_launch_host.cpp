#include "watch_launch.h"

#include <T5AppApi.h>
#include <T5ProviderCapabilityApi.h>
#include <T5StorageApi.h>
#include <RiscDisplayOutputV1.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
int g_mode = 0;
int g_usb_begins = 0;
int g_sd_paths = 0;
int g_bootfs_dirs = 0;
int g_submits = 0;
int g_inits = 0;
uint8_t g_last_buttons = 0;
std::vector<std::string> g_writes;
std::vector<std::string> g_opens;

struct Step {
  uint32_t now;
  uint32_t buttons;
  uint32_t pressed;
  uint8_t hid;
};
Step g_steps[12];
int g_step = 0;
int g_step_count = 0;

void fail(const char *message) {
  std::fprintf(stderr, "FAIL %s\n", message);
  g_failures++;
}

struct NavFrame {
  uint32_t buttons, pressed, released;
};
struct NavApi {
  uint32_t api_version, struct_size;
  void *context;
  bool (*poll)(void *context, NavFrame *out);
};

bool nav_poll(void *, NavFrame *out) {
  const Step &step = g_steps[g_step];
  out->buttons = step.buttons;
  out->pressed = step.pressed;
  out->released = 0;
  return true;
}

NavApi g_nav{1u, sizeof(NavApi), nullptr, nav_poll};

bool display_info(void *, risc_display_info_v1 *out) {
  std::memset(out, 0, sizeof(*out));
  out->api_version = 1;
  out->struct_size = sizeof(*out);
  if (g_mode == 0) {
    out->width = 960;
    out->height = 540;
    out->supported_formats = RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_MONO1);
  } else {
    out->width = 240;
    out->height = 240;
    out->supported_formats = RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_RGB565);
  }
  return true;
}

uint8_t g_pixels[240 * 240 * 2];
bool display_acquire(void *, uint32_t format, risc_display_surface_v1 *surface) {
  if (format != RISC_DISPLAY_FORMAT_RGB565 || g_mode == 0) return false;
  std::memset(surface, 0, sizeof(*surface));
  surface->frame = 7;
  surface->pixels = g_pixels;
  surface->width = 240;
  surface->height = 240;
  surface->stride_bytes = 480;
  surface->size_bytes = sizeof(g_pixels);
  surface->pixel_format = RISC_DISPLAY_FORMAT_RGB565;
  return true;
}
void display_release(void *, risc_display_frame_v1) {}
bool display_submit(void *, risc_display_frame_v1, const risc_display_rect_v1 *, size_t,
                    const risc_display_present_options_v1 *, risc_display_present_token_v1 *token) {
  g_submits++;
  if (token) *token = 1;
  return true;
}

risc_display_output_api_v1 g_display{
  1u, sizeof(g_display), nullptr, display_info, display_acquire, display_release,
  display_submit, nullptr, nullptr, nullptr};

bool provider_acquire(const char *capability, uint32_t, t5_provider_capability_lease_t *lease,
                      const void **iface) {
  if (std::strcmp(capability, "display.output") == 0) {
    *lease = 1;
    *iface = &g_display;
    return true;
  }
  if (g_mode && std::strcmp(capability, "input.navigation") == 0) {
    *lease = 2;
    *iface = &g_nav;
    return true;
  }
  return false;
}
bool provider_release(t5_provider_capability_lease_t) { return true; }
t5_provider_capability_api_v1 g_provider{1u, sizeof(g_provider), provider_acquire, provider_release, nullptr};

const char *g_names[] = {"Zelda.gb", "notes.txt", "a.gbc"};
int g_name = 0;
bool dir_open(const char *path) {
  g_opens.push_back(path);
  if (std::strstr(path, "/sd")) {
    g_sd_paths++;
    return false;
  }
  if (std::strcmp(path, "/bootfs/gameboy/roms") == 0) {
    g_bootfs_dirs++;
    g_name = 0;
    return g_mode == 1;
  }
  return false;
}
bool dir_next(t5_app_dirent_t *entry) {
  if (g_name >= 3) return false;
  std::memset(entry, 0, sizeof(*entry));
  std::snprintf(entry->name, sizeof(entry->name), "%s", g_names[g_name]);
  entry->is_directory = false;
  entry->size = 32;
  g_name++;
  return true;
}
void dir_close() {}
t5_app_api_v1 g_app{};

t5_storage_stream_t stream_open(const char *path, size_t *size) {
  g_opens.push_back(path);
  if (std::strstr(path, "/sd")) g_sd_paths++;
  if (!path || std::strncmp(path, "/bootfs/gameboy/", 16) != 0) return 0;
  *size = std::strstr(path, "/roms/") ? 32u : 4u;
  return 3;
}
size_t stream_read(t5_storage_stream_t, void *buffer, size_t capacity) {
  if (capacity < 4) return 0;
  std::memset(buffer, 0x11, capacity < 32 ? capacity : 32);
  return capacity < 32 ? capacity : 32;
}
void stream_close(t5_storage_stream_t) {}
bool storage_exists(const char *path) {
  g_opens.push_back(path);
  return std::strstr(path, "/saves/") != nullptr && !g_writes.empty();
}
bool storage_write(const char *path, const void *, size_t) {
  g_writes.push_back(path);
  if (std::strstr(path, "/sd")) g_sd_paths++;
  return true;
}
t5_storage_api_v1 g_storage{};
}  // namespace

const t5_provider_capability_api_v1 *t5_provider_capability_get_api(uint32_t) { return &g_provider; }
const t5_app_api_v1 *t5_app_get_api(uint32_t) {
  g_app.struct_size = sizeof(g_app);
  g_app.dir_open = dir_open;
  g_app.dir_next = dir_next;
  g_app.dir_close = dir_close;
  return &g_app;
}
const t5_storage_api_v1 *t5_storage_get_api(uint32_t) {
  g_storage.api_version = 1;
  g_storage.struct_size = sizeof(g_storage);
  g_storage.exists = storage_exists;
  g_storage.write_file_atomic = storage_write;
  g_storage.stream_open = stream_open;
  g_storage.stream_read = stream_read;
  g_storage.stream_close = stream_close;
  return &g_storage;
}

uint32_t millis() { return g_steps[g_step].now; }
void vTaskDelay(uint32_t) {
  if (g_step + 1 < g_step_count) g_step++;
}
void *heap_caps_malloc(size_t size, uint32_t) { return std::malloc(size); }
void heap_caps_free(void *ptr) { std::free(ptr); }

void paperboy_usb_owner_begin() { g_usb_begins++; }
void paperboy_usb_owner_poll() {}
void paperboy_usb_owner_end() {}
uint8_t usb_hid_gamepad_buttons() { return g_steps[g_step].hid; }

struct gbemu_s { int alive; };
#include "gbemu.h"
gbemu_t *gbemu_create(void) { return new gbemu_s(); }
void gbemu_destroy(gbemu_t *emu) { delete emu; }
gbemu_status_t gbemu_init(gbemu_t *, const uint8_t *, size_t) { g_inits++; return GBEMU_STATUS_OK; }
bool gbemu_run_frame(gbemu_t *, uint8_t *, size_t, uint8_t input, bool, gbemu_frame_stats_t *) {
  g_last_buttons = input;
  return true;
}
bool gbemu_has_persist(const gbemu_t *) { return true; }
size_t gbemu_get_persist_size(const gbemu_t *) { return 4; }
bool gbemu_persist_is_dirty(const gbemu_t *) { return true; }
bool gbemu_export_persist(const gbemu_t *, void *buffer, size_t size, uint32_t) {
  if (size < 4) return false;
  std::memcpy(buffer, "SAVE", 4);
  return true;
}
bool gbemu_import_persist(gbemu_t *, const void *, size_t, uint32_t) { return true; }

int main() {
  g_mode = 0;
  g_step = 0;
  g_step_count = 1;
  g_steps[0] = {0, 0, 0, 0};
  if (paperboy_watch_launch()) fail("paper panel must stay on the paper path");
  if (g_usb_begins || g_bootfs_dirs || g_submits || g_sd_paths) fail("paper probe touched the watch path");

  g_mode = 1;
  g_step = 0;
  Step script[] = {
    {0, 1u << 0, 1u << 0, 0},
    {80, 0, 0, 0},
    {100, 1u << 0, 1u << 0, 0},
    {900, 0, 0, 0x01},
    {920, 0, 0, 0x01},
    {940, 1u << 0, 1u << 0, 0x01},
    {1800, 1u << 0, 0, 0},
    {1900, 1u << 0, 1u << 0, 0},
    {4000, 1u << 0, 0, 0},
  };
  g_step_count = static_cast<int>(sizeof(script) / sizeof(script[0]));
  for (int i = 0; i < g_step_count; ++i) g_steps[i] = script[i];
  if (!paperboy_watch_launch()) fail("watch panel must take the watch path");
  if (g_usb_begins != 1) fail("watch path must start the hid owner");
  if (g_bootfs_dirs != 1) fail("roms must be scanned on bootfs");
  if (g_sd_paths) fail("watch path used an sd path");
  if (!g_inits) fail("selected rom was not loaded");
  if (g_last_buttons != 0x01) fail("hid buttons did not reach the core");
  if (g_writes.size() != 1 || g_writes[0] != "/bootfs/gameboy/saves/Zelda.pbsv")
    fail("save was not written to watch flash");
  if (!g_submits) fail("watch path did not present a frame");
  bool saw_boot = false;
  for (const auto &path : g_opens) {
    if (path.find("/sd") != std::string::npos) fail("opened sd");
    if (path == "/bootfs/gameboy/roms") saw_boot = true;
  }
  if (!saw_boot) fail("missing bootfs rom directory");
  if (g_failures) {
    std::fprintf(stderr, "%d watch launch checks failed\n", g_failures);
    return 1;
  }
  std::puts("watch launch host checks passed");
  return 0;
}
