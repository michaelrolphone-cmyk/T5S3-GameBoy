#include "gbemu.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "audio.h"

#define PGB_CGB 0
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PGB_IMPL

uint8_t audio_read(void *audio, const uint16_t addr);
void audio_write(void *audio, const uint16_t addr, const uint8_t value);

#include "crankboy_core/peanut_gb.h"

#ifndef GBEMU_FAST_MONO
#define GBEMU_FAST_MONO 0
#endif

#define GBEMU_PERSIST_VERSION_LEGACY 1U
#define GBEMU_PERSIST_VERSION 2U
#define GBEMU_STATE_VERSION_CORE_ONLY 1U
#define GBEMU_STATE_VERSION 2U

int preferences_cgb_speed = 0;
int preferences_ppu_timing = 0;
int audio_enabled = 1;

typedef struct gbemu_persist_header_s {
  uint8_t magic[4];
  uint8_t version;
  uint8_t has_cart_ram;
  uint8_t has_rtc;
  uint8_t reserved;
  uint32_t cart_ram_size;
  uint32_t timestamp;
} gbemu_persist_header_t;

typedef struct gbemu_state_header_s {
  uint8_t magic[4];
  uint8_t version;
  uint8_t reserved[3];
  uint32_t rom_fingerprint;
  uint32_t payload_size;
} gbemu_state_header_t;

struct gbemu_s {
  gb_s core;
  const uint8_t *rom_data;
  size_t rom_size;
  uint8_t *cart_ram;
  size_t cart_ram_size;
  uint8_t *wram;
  uint8_t *vram;
  uint8_t *lcd;
  uint8_t xram[XRAM_SIZE];
  gb_breakpoint breakpoints[MAX_BREAKPOINTS];
  uint32_t rom_fingerprint;
  uint16_t last_error_addr;
  enum gb_error_e last_error;
  gbemu_status_t status;
  bool runtime_error;
  bool has_rtc;
  int64_t rtc_sync_us;
  char rom_title[17];
};

static const uint8_t kPersistMagic[4] = {'P', 'B', 'S', 'V'};
static const uint8_t kStateMagic[4] = {'G', 'B', 'S', 'T'};

_Static_assert(sizeof(gbemu_persist_header_t) == 16U, "unexpected PBSV header size");
_Static_assert(sizeof(gbemu_state_header_t) == 16U, "unexpected state header size");

uint8_t audio_read(void *audio, const uint16_t addr) {
  (void)audio;
  return audio_apu_read(addr);
}

void audio_write(void *audio, const uint16_t addr, const uint8_t value) {
  (void)audio;
  audio_apu_write(addr, value);
}

void __gb_on_breakpoint(gb_s *gb, int breakpoint_number) {
  (void)gb;
  (void)breakpoint_number;
}

static void *allocate_zeroed(size_t size) {
  void *buffer = heap_caps_calloc(
      1U, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buffer == NULL) {
    buffer = heap_caps_calloc(
        1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (buffer == NULL) {
    buffer = heap_caps_calloc(1U, size, MALLOC_CAP_8BIT);
  }
  return buffer;
}

static size_t decode_rom_size(uint8_t code) {
  static const size_t kRomSizes[] = {
      32U * 1024U,
      64U * 1024U,
      128U * 1024U,
      256U * 1024U,
      512U * 1024U,
      1024U * 1024U,
      2U * 1024U * 1024U,
      4U * 1024U * 1024U,
      8U * 1024U * 1024U,
  };

  if (code >= (sizeof(kRomSizes) / sizeof(kRomSizes[0]))) {
    return 0U;
  }
  return kRomSizes[code];
}

static uint32_t fingerprint_rom(const uint8_t *rom_data, size_t rom_size) {
  uint32_t hash = 2166136261UL;
  for (size_t index = 0U; index < rom_size; ++index) {
    hash ^= rom_data[index];
    hash *= 16777619UL;
  }
  return hash;
}

static inline uint8_t lcd_pixel(const uint8_t *lcd, uint16_t x, uint16_t y) {
  const uint8_t packed = lcd[(size_t)y * LCD_WIDTH_PACKED + (x >> 2U)];
  return (uint8_t)((packed >> ((x & 0x03U) << 1U)) & 0x03U);
}

static inline bool shade_to_white(uint8_t shade, uint16_t x, uint16_t y) {
#if GBEMU_FAST_MONO
  (void)x;
  (void)y;
  return (shade & 0x03U) <= 1U;
#else
  static const uint8_t kBayer2x2[2][2] = {
      {0U, 2U},
      {3U, 1U},
  };
  const uint8_t rank = kBayer2x2[y & 1U][x & 1U];

  switch (shade & 0x03U) {
    case 0U:
      return true;
    case 1U:
      return rank != 3U;
    case 2U:
      return rank == 0U;
    default:
      return false;
  }
#endif
}

static void pack_scaled_line(
    const uint8_t *lcd, uint8_t *framebuffer, uint16_t source_y) {
  const size_t destination_y_base = (size_t)source_y * GBEMU_SCALE;

#if GBEMU_FAST_MONO
  uint8_t packed[GBEMU_FRAME_PITCH_BYTES];
  memset(packed, 0x00, sizeof(packed));

  for (uint16_t source_x = 0U; source_x < GBEMU_SOURCE_WIDTH; ++source_x) {
    if (!shade_to_white(lcd_pixel(lcd, source_x, source_y), 0U, 0U)) {
      continue;
    }
    const uint16_t destination_x_base = source_x * GBEMU_SCALE;
    for (uint8_t dx = 0U; dx < GBEMU_SCALE; ++dx) {
      const uint16_t destination_x = destination_x_base + dx;
      packed[destination_x >> 3U] |=
          (uint8_t)(0x80U >> (destination_x & 7U));
    }
  }

  for (uint8_t dy = 0U; dy < GBEMU_SCALE; ++dy) {
    memcpy(
        framebuffer + ((destination_y_base + dy) * GBEMU_FRAME_PITCH_BYTES),
        packed,
        sizeof(packed));
  }
#else
  uint8_t packed_rows[2][GBEMU_FRAME_PITCH_BYTES];
  memset(packed_rows, 0x00, sizeof(packed_rows));

  for (uint16_t source_x = 0U; source_x < GBEMU_SOURCE_WIDTH; ++source_x) {
    const uint8_t shade = lcd_pixel(lcd, source_x, source_y);
    const uint16_t destination_x_base = source_x * GBEMU_SCALE;
    for (uint8_t parity = 0U; parity < 2U; ++parity) {
      for (uint8_t dx = 0U; dx < GBEMU_SCALE; ++dx) {
        const uint16_t destination_x = destination_x_base + dx;
        if (shade_to_white(shade, destination_x, parity)) {
          packed_rows[parity][destination_x >> 3U] |=
              (uint8_t)(0x80U >> (destination_x & 7U));
        }
      }
    }
  }

  for (uint8_t dy = 0U; dy < GBEMU_SCALE; ++dy) {
    const size_t destination_y = destination_y_base + dy;
    memcpy(
        framebuffer + (destination_y * GBEMU_FRAME_PITCH_BYTES),
        packed_rows[destination_y & 1U],
        GBEMU_FRAME_PITCH_BYTES);
  }
#endif
}

static void render_frame(const gbemu_t *emu, uint8_t *framebuffer) {
  for (uint16_t source_y = 0U; source_y < GBEMU_SOURCE_HEIGHT; ++source_y) {
    pack_scaled_line(emu->lcd, framebuffer, source_y);
  }
}

static void gb_error_cb(
    gb_s *gb, const enum gb_error_e error, const uint16_t addr) {
  gbemu_t *emu = (gbemu_t *)gb->direct.priv;
  if (emu == NULL) {
    return;
  }

  emu->runtime_error = true;
  emu->status = GBEMU_STATUS_RUNTIME_ERROR;
  emu->last_error = error;
  emu->last_error_addr = addr;
}

static bool is_ready(const gbemu_t *emu) {
  return emu != NULL && emu->status == GBEMU_STATUS_OK;
}

static size_t persist_rtc_size(const gbemu_t *emu) {
  return emu->has_rtc ? sizeof(emu->core.cart_rtc) : 0U;
}

static void bind_instance_memory(gbemu_t *emu) {
  emu->core.xram = emu->xram;
  emu->core.breakpoints = emu->breakpoints;
}

static void rebuild_loaded_state_pointers(gbemu_t *emu) {
  bind_instance_memory(emu);
  __gb_update_map_pointers(&emu->core);
}

static void reset_rtc_sync(gbemu_t *emu) {
  emu->rtc_sync_us = emu->has_rtc ? esp_timer_get_time() : 0;
}

static void sync_rtc(gbemu_t *emu) {
  int64_t now_us;
  uint64_t elapsed_seconds;

  if (!emu->has_rtc) {
    return;
  }

  now_us = esp_timer_get_time();
  if (emu->rtc_sync_us <= 0 || now_us < emu->rtc_sync_us) {
    emu->rtc_sync_us = now_us;
    return;
  }
  if ((emu->core.rtc_bits.high & 0x40U) != 0U) {
    emu->rtc_sync_us = now_us;
    return;
  }

  elapsed_seconds =
      (uint64_t)(now_us - emu->rtc_sync_us) / 1000000ULL;
  if (elapsed_seconds == 0U) {
    return;
  }

  emu->rtc_sync_us += (int64_t)(elapsed_seconds * 1000000ULL);
  while (elapsed_seconds > 0U) {
    const uint32_t chunk = elapsed_seconds > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)elapsed_seconds;
    gb_catch_up_rtc_direct(&emu->core, chunk);
    elapsed_seconds -= chunk;
  }
}

gbemu_t *gbemu_create(void) {
  gbemu_t *emu = (gbemu_t *)allocate_zeroed(sizeof(gbemu_t));
  if (emu == NULL) {
    return NULL;
  }

  emu->wram = (uint8_t *)allocate_zeroed(WRAM_SIZE_CGB);
  emu->vram = (uint8_t *)allocate_zeroed(VRAM_SIZE_CGB);
  emu->lcd = (uint8_t *)allocate_zeroed(LCD_BUFFER_BYTES);
  if (emu->wram == NULL || emu->vram == NULL || emu->lcd == NULL) {
    if (emu->lcd != NULL) {
      heap_caps_free(emu->lcd);
    }
    if (emu->vram != NULL) {
      heap_caps_free(emu->vram);
    }
    if (emu->wram != NULL) {
      heap_caps_free(emu->wram);
    }
    heap_caps_free(emu);
    return NULL;
  }

  emu->status = GBEMU_STATUS_NO_ROM;
  return emu;
}

void gbemu_destroy(gbemu_t *emu) {
  if (emu == NULL) {
    return;
  }

  if (emu->cart_ram != NULL) {
    heap_caps_free(emu->cart_ram);
  }
  heap_caps_free(emu->lcd);
  heap_caps_free(emu->vram);
  heap_caps_free(emu->wram);
  heap_caps_free(emu);
}

gbemu_status_t gbemu_init(
    gbemu_t *emu, const uint8_t *rom_data, size_t rom_size) {
  enum gb_init_error_e init_error;
  const uint8_t rom_size_code =
      (rom_data != NULL && rom_size > 0x148U) ? rom_data[0x148U] : 0xFFU;
  const uint8_t ram_size_code =
      (rom_data != NULL && rom_size > 0x149U) ? rom_data[0x149U] : 0xFFU;
  size_t expected_rom_size;

  if (emu == NULL) {
    return GBEMU_STATUS_INVALID_ARGUMENT;
  }

  if (emu->cart_ram != NULL) {
    heap_caps_free(emu->cart_ram);
    emu->cart_ram = NULL;
  }
  memset(&emu->core, 0, sizeof(emu->core));
  memset(emu->wram, 0, WRAM_SIZE_CGB);
  memset(emu->vram, 0, VRAM_SIZE_CGB);
  memset(emu->lcd, 0, LCD_BUFFER_BYTES);
  emu->rom_data = rom_data;
  emu->rom_size = rom_size;
  emu->cart_ram_size = 0U;
  emu->rom_fingerprint = 0U;
  emu->last_error_addr = 0U;
  emu->last_error = GB_UNKNOWN_ERROR;
  emu->runtime_error = false;
  emu->has_rtc = false;
  emu->rtc_sync_us = 0;
  memset(emu->rom_title, 0, sizeof(emu->rom_title));

  if (rom_data == NULL || rom_size == 0U) {
    emu->status = GBEMU_STATUS_NO_ROM;
    return emu->status;
  }
  if (rom_size < 0x150U) {
    emu->status = GBEMU_STATUS_ROM_TOO_SMALL;
    return emu->status;
  }
  if (rom_size > GBEMU_MAX_ROM_BYTES) {
    emu->status = GBEMU_STATUS_ROM_TOO_LARGE;
    return emu->status;
  }

  expected_rom_size = decode_rom_size(rom_size_code);
  if (expected_rom_size == 0U) {
    emu->status = GBEMU_STATUS_UNSUPPORTED_ROM_SIZE;
    return emu->status;
  }
  if (ram_size_code > 5U) {
    emu->status = GBEMU_STATUS_UNSUPPORTED_RAM_SIZE;
    return emu->status;
  }
  if (rom_size < expected_rom_size) {
    emu->status = GBEMU_STATUS_ROM_TRUNCATED;
    return emu->status;
  }
  if (gb_get_models_supported((uint8_t *)rom_data) == GB_SUPPORT_CGB) {
    emu->status = GBEMU_STATUS_CGB_ONLY_ROM;
    return emu->status;
  }

  init_error = gb_init(
      &emu->core,
      emu->wram,
      emu->vram,
      emu->lcd,
      (uint8_t *)rom_data,
      rom_size,
      gb_error_cb,
      emu,
      false);
  if (init_error != GB_INIT_NO_ERROR &&
      init_error != GB_INIT_NO_ERROR_BUT_REQUIRES_CGB) {
    emu->status = (init_error == GB_INIT_INVALID_CHECKSUM)
        ? GBEMU_STATUS_INIT_INVALID_CHECKSUM
        : GBEMU_STATUS_INIT_CARTRIDGE_UNSUPPORTED;
    return emu->status;
  }

  memset(emu->xram, 0, sizeof(emu->xram));
  memset(emu->breakpoints, 0xFF, sizeof(emu->breakpoints));
  bind_instance_memory(emu);
  emu->has_rtc =
      rom_data[0x147U] == 0x0FU || rom_data[0x147U] == 0x10U;

  emu->cart_ram_size = (size_t)gb_get_save_size(&emu->core);
  if (emu->cart_ram_size > 0U) {
    emu->cart_ram = (uint8_t *)allocate_zeroed(emu->cart_ram_size);
    if (emu->cart_ram == NULL) {
      emu->cart_ram_size = 0U;
      emu->status = GBEMU_STATUS_CART_RAM_ALLOC_FAILED;
      return emu->status;
    }
  }
  emu->core.gb_cart_ram = emu->cart_ram;
  emu->core.gb_cart_ram_size = (uint32_t)emu->cart_ram_size;

  audio_reset_apu();
  gb_reset(&emu->core, false);
  gb_init_lcd(&emu->core);
  emu->core.direct.joypad = 0xFFU;
  emu->core.direct.sram_updated = 0U;
  emu->core.direct.sram_dirty = 0U;
  audio_enabled = 1;

  gb_get_rom_name((uint8_t *)rom_data, emu->rom_title);
  emu->rom_fingerprint = fingerprint_rom(rom_data, rom_size);
  emu->status = GBEMU_STATUS_OK;
  reset_rtc_sync(emu);
  return emu->status;
}

bool gbemu_run_frame(
    gbemu_t *emu,
    uint8_t *framebuffer,
    size_t framebuffer_size,
    uint8_t input_mask,
    bool skip_render,
    gbemu_frame_stats_t *out_stats) {
  const int64_t started_at = esp_timer_get_time();
  uint32_t draw_us = 0U;

  if (out_stats != NULL) {
    memset(out_stats, 0, sizeof(*out_stats));
  }
  if (!is_ready(emu)) {
    return false;
  }
  if (!skip_render &&
      (framebuffer == NULL || framebuffer_size < GBEMU_FRAMEBUFFER_SIZE)) {
    emu->status = GBEMU_STATUS_INVALID_ARGUMENT;
    return false;
  }

  emu->runtime_error = false;
  sync_rtc(emu);
  emu->core.direct.joypad = (uint8_t)(~input_mask);
  emu->core.direct.frame_skip = skip_render;
  gb_run_frame__dmg(&emu->core);
  emu->core.direct.frame_skip = false;

  if (!skip_render && !emu->runtime_error) {
    const int64_t draw_started_at = esp_timer_get_time();
    render_frame(emu, framebuffer);
    draw_us = (uint32_t)(esp_timer_get_time() - draw_started_at);
  }

  if (out_stats != NULL) {
    out_stats->run_us = (uint32_t)(esp_timer_get_time() - started_at);
    out_stats->draw_us = draw_us;
    out_stats->lines_drawn =
        (!skip_render && !emu->runtime_error) ? GBEMU_SOURCE_HEIGHT : 0U;
    out_stats->rendered = !skip_render && !emu->runtime_error;
  }
  return !emu->runtime_error;
}

size_t gbemu_get_state_size(const gbemu_t *emu) {
  if (!is_ready(emu)) {
    return 0U;
  }
  return sizeof(gbemu_state_header_t) +
      (size_t)gb_get_state_size((gb_s *)&emu->core) +
      audio_apu_state_size();
}

bool gbemu_save_state(
    const gbemu_t *emu, void *buffer, size_t buffer_size) {
  const size_t core_state_size =
      is_ready(emu) ? (size_t)gb_get_state_size((gb_s *)&emu->core) : 0U;
  const size_t audio_state_size = audio_apu_state_size();
  const size_t required_size = gbemu_get_state_size(emu);
  gbemu_state_header_t header;
  uint8_t *payload;

  if (required_size == 0U || core_state_size == 0U ||
      audio_state_size == 0U || buffer == NULL ||
      buffer_size < required_size || core_state_size > UINT32_MAX) {
    return false;
  }

  sync_rtc((gbemu_t *)emu);

  memcpy(header.magic, kStateMagic, sizeof(header.magic));
  header.version = GBEMU_STATE_VERSION;
  memset(header.reserved, 0, sizeof(header.reserved));
  header.rom_fingerprint = emu->rom_fingerprint;
  header.payload_size = (uint32_t)core_state_size;
  memcpy(buffer, &header, sizeof(header));
  payload = (uint8_t *)buffer + sizeof(gbemu_state_header_t);
  gb_state_save((gb_s *)&emu->core, (char *)payload);
  return audio_apu_state_export(
      payload + core_state_size, audio_state_size);
}

bool gbemu_load_state(
    gbemu_t *emu, const void *buffer, size_t buffer_size) {
  gbemu_state_header_t header;
  const uint8_t *payload;
  size_t audio_state_size = 0U;
  size_t expected_size;
  const char *error;
  if (!is_ready(emu) || buffer == NULL) {
    return false;
  }

  bind_instance_memory(emu);

  if (buffer_size < sizeof(gbemu_state_header_t) ||
      memcmp(buffer, kStateMagic, sizeof(kStateMagic)) != 0) {
    error = gb_state_load(
        &emu->core, (const char *)buffer, buffer_size);
    if (error != NULL) {
      return false;
    }
    rebuild_loaded_state_pointers(emu);
    audio_reset_apu();
    emu->last_error_addr = 0U;
    emu->last_error = GB_UNKNOWN_ERROR;
    emu->runtime_error = false;
    emu->status = GBEMU_STATUS_OK;
    reset_rtc_sync(emu);
    return true;
  }

  memcpy(&header, buffer, sizeof(header));
  if (memcmp(header.magic, kStateMagic, sizeof(header.magic)) != 0 ||
      (header.version != GBEMU_STATE_VERSION_CORE_ONLY &&
       header.version != GBEMU_STATE_VERSION) ||
      header.reserved[0] != 0U || header.reserved[1] != 0U ||
      header.reserved[2] != 0U ||
      header.rom_fingerprint != emu->rom_fingerprint) {
    return false;
  }

  if (header.version == GBEMU_STATE_VERSION) {
    audio_state_size = audio_apu_state_size();
    if (audio_state_size == 0U) {
      return false;
    }
  }
  expected_size = sizeof(gbemu_state_header_t) +
      (size_t)header.payload_size + audio_state_size;
  if (expected_size < (size_t)header.payload_size ||
      buffer_size != expected_size) {
    return false;
  }

  payload = (const uint8_t *)buffer + sizeof(gbemu_state_header_t);
  if (audio_state_size > 0U &&
      !audio_apu_state_validate(
          payload + header.payload_size, audio_state_size)) {
    return false;
  }

  error = gb_state_load(
      &emu->core,
      (const char *)payload,
      header.payload_size);
  if (error != NULL) {
    return false;
  }
  if (audio_state_size > 0U) {
    if (!audio_apu_state_import(
            payload + header.payload_size, audio_state_size)) {
      return false;
    }
  } else {
    audio_reset_apu();
  }

  rebuild_loaded_state_pointers(emu);
  emu->last_error_addr = 0U;
  emu->last_error = GB_UNKNOWN_ERROR;
  emu->runtime_error = false;
  emu->status = GBEMU_STATUS_OK;
  reset_rtc_sync(emu);
  return true;
}

bool gbemu_has_persist(const gbemu_t *emu) {
  return is_ready(emu) &&
      (emu->cart_ram_size > 0U || emu->has_rtc);
}

size_t gbemu_get_persist_size(const gbemu_t *emu) {
  if (!gbemu_has_persist(emu)) {
    return 0U;
  }
  return sizeof(gbemu_persist_header_t) +
      emu->cart_ram_size + persist_rtc_size(emu);
}

bool gbemu_persist_is_dirty(const gbemu_t *emu) {
  return gbemu_has_persist(emu) &&
      (emu->has_rtc || emu->core.direct.sram_updated != 0U);
}

bool gbemu_export_persist(
    const gbemu_t *emu,
    void *buffer,
    size_t buffer_size,
    uint32_t timestamp) {
  const size_t required_size = gbemu_get_persist_size(emu);
  gbemu_persist_header_t header;
  uint8_t *cursor;

  if (required_size == 0U || buffer == NULL || buffer_size < required_size) {
    return false;
  }

  sync_rtc((gbemu_t *)emu);

  memcpy(header.magic, kPersistMagic, sizeof(header.magic));
  header.version = GBEMU_PERSIST_VERSION;
  header.has_cart_ram = emu->cart_ram_size > 0U ? 1U : 0U;
  header.has_rtc = emu->has_rtc ? 1U : 0U;
  header.reserved = 0U;
  header.cart_ram_size = (uint32_t)emu->cart_ram_size;
  header.timestamp = timestamp;

  memcpy(buffer, &header, sizeof(header));
  cursor = (uint8_t *)buffer + sizeof(header);
  if (emu->cart_ram_size > 0U) {
    memcpy(cursor, emu->cart_ram, emu->cart_ram_size);
    cursor += emu->cart_ram_size;
  }
  if (header.has_rtc != 0U) {
    memcpy(cursor, emu->core.cart_rtc, sizeof(emu->core.cart_rtc));
  }
  return true;
}

bool gbemu_import_persist(
    gbemu_t *emu,
    const void *buffer,
    size_t buffer_size,
    uint32_t current_timestamp) {
  gbemu_persist_header_t header;
  const uint8_t *cursor;
  size_t expected_size;
  bool legacy_extra_rtc;

  if (!gbemu_has_persist(emu) || buffer == NULL ||
      buffer_size < sizeof(header)) {
    return false;
  }

  memcpy(&header, buffer, sizeof(header));
  if (memcmp(header.magic, kPersistMagic, sizeof(header.magic)) != 0 ||
      (header.version != GBEMU_PERSIST_VERSION_LEGACY &&
       header.version != GBEMU_PERSIST_VERSION) ||
      header.has_cart_ram > 1U || header.has_rtc > 1U ||
      header.reserved != 0U) {
    return false;
  }
  legacy_extra_rtc =
      header.version == GBEMU_PERSIST_VERSION_LEGACY &&
      !emu->has_rtc && header.has_rtc != 0U && emu->core.cart_battery != 0U;
  if ((header.has_cart_ram != 0U) != (emu->cart_ram_size > 0U) ||
      header.cart_ram_size != emu->cart_ram_size ||
      ((header.has_rtc != 0U) != emu->has_rtc && !legacy_extra_rtc)) {
    return false;
  }

  expected_size = sizeof(header) + header.cart_ram_size +
      (header.has_rtc != 0U ? sizeof(emu->core.cart_rtc) : 0U);
  if (buffer_size != expected_size) {
    return false;
  }

  cursor = (const uint8_t *)buffer + sizeof(header);
  if (header.cart_ram_size > 0U) {
    memcpy(emu->cart_ram, cursor, header.cart_ram_size);
    cursor += header.cart_ram_size;
  }
  if (header.has_rtc != 0U) {
    if (emu->has_rtc) {
      memcpy(emu->core.cart_rtc, cursor, sizeof(emu->core.cart_rtc));
    }
    if (emu->has_rtc && header.version == GBEMU_PERSIST_VERSION &&
        header.timestamp > 0U && current_timestamp >= header.timestamp) {
      gb_catch_up_rtc_direct(
          &emu->core, current_timestamp - header.timestamp);
    }
    if (emu->has_rtc) {
      memcpy(
          emu->core.latched_rtc,
          emu->core.cart_rtc,
          sizeof(emu->core.latched_rtc));
    }
  }

  gbemu_mark_persist_clean(emu);
  reset_rtc_sync(emu);
  return true;
}

void gbemu_mark_persist_clean(gbemu_t *emu) {
  if (!is_ready(emu)) {
    return;
  }
  emu->core.direct.sram_updated = 0U;
  emu->core.direct.sram_dirty = 0U;
}

void gbemu_reset(gbemu_t *emu) {
  if (!is_ready(emu)) {
    return;
  }

  sync_rtc(emu);
  memset(emu->lcd, 0, LCD_BUFFER_BYTES);
  bind_instance_memory(emu);
  audio_reset_apu();
  gb_reset(&emu->core, false);
  gb_init_lcd(&emu->core);
  emu->core.direct.joypad = 0xFFU;
  emu->last_error_addr = 0U;
  emu->last_error = GB_UNKNOWN_ERROR;
  emu->runtime_error = false;
  emu->status = GBEMU_STATUS_OK;
  reset_rtc_sync(emu);
}

gbemu_status_t gbemu_get_status(const gbemu_t *emu) {
  return emu == NULL ? GBEMU_STATUS_INVALID_ARGUMENT : emu->status;
}

uint16_t gbemu_get_last_error_addr(const gbemu_t *emu) {
  return emu == NULL ? 0U : emu->last_error_addr;
}

const char *gbemu_status_string(gbemu_status_t status) {
  switch (status) {
    case GBEMU_STATUS_OK:
      return "ok";
    case GBEMU_STATUS_NO_ROM:
      return "no embedded ROM";
    case GBEMU_STATUS_ROM_TOO_SMALL:
      return "ROM header too small";
    case GBEMU_STATUS_UNSUPPORTED_ROM_SIZE:
      return "unsupported ROM size code";
    case GBEMU_STATUS_UNSUPPORTED_RAM_SIZE:
      return "unsupported RAM size code";
    case GBEMU_STATUS_ROM_TRUNCATED:
      return "ROM image is smaller than its header declares";
    case GBEMU_STATUS_CART_RAM_ALLOC_FAILED:
      return "cart RAM allocation failed";
    case GBEMU_STATUS_INIT_CARTRIDGE_UNSUPPORTED:
      return "unsupported cartridge type";
    case GBEMU_STATUS_INIT_INVALID_CHECKSUM:
      return "invalid ROM header checksum";
    case GBEMU_STATUS_RUNTIME_ERROR:
      return "emulator runtime error";
    case GBEMU_STATUS_CGB_ONLY_ROM:
      return "CGB-only ROM is not supported";
    case GBEMU_STATUS_ROM_TOO_LARGE:
      return "ROM image exceeds the 4 MiB limit";
    default:
      return "invalid argument";
  }
}

const char *gbemu_get_rom_title(const gbemu_t *emu) {
  if (!is_ready(emu) || emu->rom_title[0] == '\0') {
    return "UNKNOWN";
  }
  return emu->rom_title;
}

uint32_t gbemu_get_rom_fingerprint(const gbemu_t *emu) {
  return is_ready(emu) ? emu->rom_fingerprint : 0U;
}
