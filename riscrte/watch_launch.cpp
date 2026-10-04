#include "watch_launch.h"

#include "watch_path.h"
#include "font8x8_basic.h"

#include <Arduino.h>
#include <T5AppApi.h>
#include <T5ProviderCapabilityApi.h>
#include <T5StorageApi.h>
#include <RiscDisplayOutputV1.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "elf_lifecycle.h"
#include "gbemu.h"
#include "usb_hid_gamepad.h"

namespace {
constexpr char kTag[] = "gameboy_watch";
constexpr uint16_t kWhite = 0xffffu;
constexpr uint16_t kBlack = 0x0000u;
constexpr uint16_t kAmber = 0xfbe0u;

/* Prefix of risc_input_navigation_api_v1 from T-Watch PR #8 (ae819bb).
 * Later callbacks are not called. input.navigation is the crown/button
 * capability that driver publishes. usb.hid.* stays on the existing adapter. */
struct NavFrame {
  uint32_t buttons, pressed, released;
};
struct NavApi {
  uint32_t api_version, struct_size;
  void *context;
  bool (*poll)(void *context, NavFrame *out);
};

const t5_provider_capability_api_v1 *g_provider = nullptr;
const risc_display_output_api_v1 *g_display = nullptr;
t5_provider_capability_lease_t g_display_lease = 0;
const NavApi *g_nav = nullptr;
t5_provider_capability_lease_t g_nav_lease = 0;
int g_rich = 0;

void *alloc_bytes(size_t size) {
  void *spiram = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return spiram ? spiram : malloc(size);
}

void free_bytes(void *ptr) {
  if (!ptr) return;
  heap_caps_free(ptr);
}

void put_px(uint8_t *pixels, uint32_t stride, int x, int y, uint16_t color) {
  if (!pixels || x < 0 || y < 0 || x >= 240 || y >= 240) return;
  uint8_t *p = pixels + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 2u;
  p[0] = static_cast<uint8_t>(color);
  p[1] = static_cast<uint8_t>(color >> 8);
}

void fill(uint8_t *pixels, uint32_t stride, uint32_t size, uint16_t color) {
  if (!pixels) return;
  for (uint32_t y = 0; y < 240; ++y) {
    for (uint32_t x = 0; x < 240; ++x) put_px(pixels, stride, static_cast<int>(x), static_cast<int>(y), color);
    if ((y + 1u) * stride > size) break;
  }
}

void draw_char(uint8_t *pixels, uint32_t stride, int x, int y, char ch, uint16_t color) {
  unsigned char index = static_cast<unsigned char>(ch);
  if (index >= 128) index = '?';
  const unsigned char *rows = font8x8_basic[index];
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      if (rows[row] & (1u << col)) put_px(pixels, stride, x + col, y + row, color);
    }
  }
}

void draw_text(uint8_t *pixels, uint32_t stride, int x, int y, const char *text, uint16_t color) {
  if (!text) return;
  for (int i = 0; text[i] && x < 232; ++i, x += 8) draw_char(pixels, stride, x, y, text[i], color);
}

bool present(uint8_t *, uint32_t, risc_display_frame_v1 frame) {
  if (!g_display || !frame) return false;
  risc_display_present_token_v1 token = 0;
  return g_display->submit(g_display->context, frame, nullptr, 0, nullptr, &token);
}

bool read_input(watch_input *input) {
  paperboy_usb_owner_poll();
  input->hid = usb_hid_gamepad_buttons();
  input->buttons = 0;
  input->pressed = 0;
  if (!g_nav || !g_nav->poll) return true;
  NavFrame frame{};
  if (!g_nav->poll(g_nav->context, &frame)) return false;
  input->buttons = frame.buttons;
  input->pressed = frame.pressed;
  if ((frame.buttons | frame.pressed) & WATCH_NAV_RICH) g_rich = 1;
  return true;
}

bool blit_game(uint8_t *pixels, uint32_t stride, const uint8_t *mono) {
  constexpr int origin_x = (240 - static_cast<int>(GBEMU_SOURCE_WIDTH)) / 2;
  constexpr int origin_y = (240 - static_cast<int>(GBEMU_SOURCE_HEIGHT)) / 2;
  for (int sy = 0; sy < static_cast<int>(GBEMU_SOURCE_HEIGHT); ++sy) {
    const int dy = sy * static_cast<int>(GBEMU_SCALE);
    for (int sx = 0; sx < static_cast<int>(GBEMU_SOURCE_WIDTH); ++sx) {
      const int dx = sx * static_cast<int>(GBEMU_SCALE);
      const size_t bit = static_cast<size_t>(dy) * GBEMU_FRAME_WIDTH + static_cast<size_t>(dx);
      const int white = (mono[bit >> 3] & static_cast<uint8_t>(0x80u >> (bit & 7u))) != 0;
      put_px(pixels, stride, origin_x + sx, origin_y + sy, white ? kWhite : kBlack);
    }
  }
  return true;
}

bool load_exact(const t5_storage_api_v1 *storage, const char *path, uint8_t **out, size_t *out_size) {
  size_t size = 0;
  *out = nullptr;
  *out_size = 0;
  if (!storage || !storage->stream_open || !storage->stream_read || !storage->stream_close) return false;
  const t5_storage_stream_t stream = storage->stream_open(path, &size);
  if (stream == T5_STORAGE_STREAM_INVALID || size == 0 || size > GBEMU_MAX_ROM_BYTES) {
    if (stream != T5_STORAGE_STREAM_INVALID) storage->stream_close(stream);
    return false;
  }
  uint8_t *buffer = static_cast<uint8_t *>(alloc_bytes(size));
  if (!buffer) {
    storage->stream_close(stream);
    return false;
  }
  size_t got = 0;
  while (got < size) {
    const size_t n = storage->stream_read(stream, buffer + got, size - got);
    if (n == 0 || n > size - got) break;
    got += n;
  }
  storage->stream_close(stream);
  if (got != size) {
    free_bytes(buffer);
    return false;
  }
  *out = buffer;
  *out_size = size;
  return true;
}

constexpr size_t kPersistMax = 128u * 1024u;

void persist_save(const t5_storage_api_v1 *storage, gbemu_t *emu, const char *rom_name) {
  char path[192];
  if (!storage || !storage->write_file_atomic || !emu || !gbemu_has_persist(emu) ||
      !gbemu_persist_is_dirty(emu) || !watch_save_path(rom_name, path, sizeof(path))) return;
  const size_t size = gbemu_get_persist_size(emu);
  if (size == 0 || size > kPersistMax) return;
  uint8_t *buffer = static_cast<uint8_t *>(alloc_bytes(size));
  if (!buffer) return;
  if (gbemu_export_persist(emu, buffer, size, 0))
    (void)storage->write_file_atomic(path, buffer, size);
  free_bytes(buffer);
}

bool scan_roms(watch_catalog *catalog) {
  memset(catalog, 0, sizeof(*catalog));
  const t5_app_api_v1 *app = t5_app_get_api(T5_APP_ABI_VERSION);
  if (!app || !app->dir_open || !app->dir_next || !app->dir_close) return false;
  if (!app->dir_open(WATCH_ROM_DIR)) return false;
  for (;;) {
    t5_app_dirent_t entry{};
    if (!app->dir_next(&entry)) break;
    if (entry.is_directory) continue;
    (void)watch_catalog_add(catalog, entry.name);
  }
  app->dir_close();
  watch_catalog_sort(catalog);
  return true;
}

bool acquire_surface(risc_display_surface_v1 *surface) {
  memset(surface, 0, sizeof(*surface));
  if (!g_display || !g_display->acquire) return false;
  if (!g_display->acquire(g_display->context, RISC_DISPLAY_FORMAT_RGB565, surface)) return false;
  if (surface->pixels && surface->width == 240 && surface->height == 240 &&
      surface->stride_bytes >= 480 &&
      surface->pixel_format == RISC_DISPLAY_FORMAT_RGB565) return true;
  if (surface->frame && g_display->release) g_display->release(g_display->context, surface->frame);
  memset(surface, 0, sizeof(*surface));
  return false;
}

void release_surface(risc_display_surface_v1 *surface) {
  if (g_display && surface->frame && g_display->release)
    g_display->release(g_display->context, surface->frame);
  memset(surface, 0, sizeof(*surface));
}

enum class Mode : uint8_t { Picker, Play };

bool run_session() {
  watch_catalog catalog{};
  (void)scan_roms(&catalog);
  const t5_storage_api_v1 *storage = t5_storage_get_api(T5_STORAGE_API_VERSION);
  size_t selected = 0;
  Mode mode = Mode::Picker;
  watch_picker_state picker{};
  watch_picker_reset(&picker);
  gbemu_t *emu = nullptr;
  uint8_t *rom = nullptr;
  size_t rom_size = 0;
  uint8_t *mono = nullptr;
  char rom_name[WATCH_NAME_MAX] = {};
  int back_down = 0;
  uint32_t back_since = 0;
  bool leave = false;

  while (!leave) {
    watch_input input{};
    if (!read_input(&input)) {
      vTaskDelay(1);
      continue;
    }
    const uint32_t now = millis();
    risc_display_surface_v1 surface{};
    const bool have_frame = acquire_surface(&surface);
    uint8_t *pixels = have_frame ? static_cast<uint8_t *>(surface.pixels) : nullptr;
    if (have_frame) fill(pixels, surface.stride_bytes, surface.size_bytes, kBlack);

    if (mode == Mode::Picker) {
      const watch_ui_action action = watch_picker_step(&picker, &input, now);
      if (action == WATCH_UI_EXIT_APP) leave = true;
      else if (action == WATCH_UI_PREVIOUS && catalog.count)
        selected = (selected + catalog.count - 1u) % catalog.count;
      else if (action == WATCH_UI_NEXT && catalog.count)
        selected = (selected + 1u) % catalog.count;
      else if (action == WATCH_UI_LAUNCH && catalog.count) {
        char path[192];
        if (watch_rom_path(catalog.roms[selected].name, path, sizeof(path)) &&
            load_exact(storage, path, &rom, &rom_size)) {
          emu = gbemu_create();
          if (emu && gbemu_init(emu, rom, rom_size) == GBEMU_STATUS_OK) {
            memcpy(rom_name, catalog.roms[selected].name, sizeof(rom_name));
            char save_path[192];
            uint8_t *save = nullptr;
            size_t save_size = 0;
            if (watch_save_path(rom_name, save_path, sizeof(save_path)) &&
                storage && storage->exists && storage->exists(save_path) &&
                load_exact(storage, save_path, &save, &save_size)) {
              (void)gbemu_import_persist(emu, save, save_size, 0);
              free_bytes(save);
            }
            mono = static_cast<uint8_t *>(alloc_bytes(GBEMU_FRAMEBUFFER_SIZE));
            if (mono) mode = Mode::Play;
            back_down = 0;
          }
          if (mode != Mode::Play) {
            if (emu) gbemu_destroy(emu);
            emu = nullptr;
            free_bytes(rom);
            rom = nullptr;
            free_bytes(mono);
            mono = nullptr;
          }
        }
      }
      if (have_frame && mode == Mode::Picker) {
        draw_text(pixels, surface.stride_bytes, 8, 8, "Game Boy", kAmber);
        draw_text(pixels, surface.stride_bytes, 8, 24, WATCH_ROM_DIR, kWhite);
        if (!catalog.count) {
          draw_text(pixels, surface.stride_bytes, 8, 48, "No ROMs in flash", kWhite);
        } else {
          const size_t first = selected > 4 ? selected - 4 : 0;
          for (size_t row = 0; row < 8 && first + row < catalog.count; ++row) {
            const size_t index = first + row;
            draw_text(pixels, surface.stride_bytes, 8, 48 + static_cast<int>(row) * 16,
                      index == selected ? ">" : " ", index == selected ? kAmber : kWhite);
            draw_text(pixels, surface.stride_bytes, 20, 48 + static_cast<int>(row) * 16,
                      catalog.roms[index].name, kWhite);
          }
        }
        draw_text(pixels, surface.stride_bytes, 8, 216,
                  g_rich ? "pad: move confirm" : "crown: tap next hold play", kWhite);
      }
    } else if (emu && mono) {
      if (input.buttons & WATCH_NAV_BACK) {
        if (!back_down) {
          back_down = 1;
          back_since = now;
        }
      } else back_down = 0;
      const uint32_t held = back_down ? now - back_since : 0;
      if (watch_play_leave(&input, held, g_rich)) {
        persist_save(storage, emu, rom_name);
        gbemu_destroy(emu);
        emu = nullptr;
        free_bytes(rom);
        rom = nullptr;
        free_bytes(mono);
        mono = nullptr;
        mode = Mode::Picker;
        watch_picker_reset(&picker);
        if (g_rich) picker.rich = 1;
      } else {
        (void)gbemu_run_frame(emu, mono, GBEMU_FRAMEBUFFER_SIZE,
                              watch_play_buttons(&input, g_rich), false, nullptr);
        if (have_frame) blit_game(pixels, surface.stride_bytes, mono);
      }
    }

    if (have_frame) {
      (void)present(pixels, surface.stride_bytes, surface.frame);
      release_surface(&surface);
    }
    vTaskDelay(1);
  }
  if (emu) {
    persist_save(storage, emu, rom_name);
    gbemu_destroy(emu);
  }
  free_bytes(rom);
  free_bytes(mono);
  return true;
}
}  // namespace

bool paperboy_watch_launch() {
  g_provider = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
  if (!g_provider || g_provider->struct_size < sizeof(*g_provider) ||
      !g_provider->acquire || !g_provider->release) return false;
  const void *iface = nullptr;
  if (!g_provider->acquire(RISC_DISPLAY_OUTPUT_CAPABILITY, RISC_DISPLAY_OUTPUT_API_V1,
                           &g_display_lease, &iface) || !g_display_lease || !iface) {
    return false;
  }
  g_display = static_cast<const risc_display_output_api_v1 *>(iface);
  risc_display_info_v1 info{};
  const bool watch = g_display->api_version == RISC_DISPLAY_OUTPUT_API_V1 &&
                     g_display->struct_size >= sizeof(*g_display) && g_display->get_info &&
                     g_display->get_info(g_display->context, &info) &&
                     watch_display_is_twatch(info.width, info.height, info.supported_formats);
  if (!watch) {
    (void)g_provider->release(g_display_lease);
    g_display_lease = 0;
    g_display = nullptr;
    g_provider = nullptr;
    return false;
  }
  paperboy_usb_owner_begin();
  iface = nullptr;
  if (g_provider->acquire("input.navigation", 1u, &g_nav_lease, &iface) && iface) {
    const auto *nav = static_cast<const NavApi *>(iface);
    if (nav->api_version == 1u && nav->struct_size >= sizeof(NavApi) && nav->poll) g_nav = nav;
  }
  if (!g_nav && g_nav_lease) {
    (void)g_provider->release(g_nav_lease);
    g_nav_lease = 0;
  }
  ESP_LOGI(kTag, "T-Watch path roms=%s saves=%s nav=%u", WATCH_ROM_DIR, WATCH_SAVE_DIR,
           static_cast<unsigned>(g_nav != nullptr));
  (void)run_session();
  paperboy_usb_owner_end();
  if (g_nav_lease) (void)g_provider->release(g_nav_lease);
  if (g_display_lease) (void)g_provider->release(g_display_lease);
  g_nav = nullptr;
  g_nav_lease = 0;
  g_display = nullptr;
  g_display_lease = 0;
  g_provider = nullptr;
  g_rich = 0;
  return true;
}
