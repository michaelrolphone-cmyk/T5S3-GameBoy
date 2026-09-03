#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBEMU_SOURCE_WIDTH 160U
#define GBEMU_SOURCE_HEIGHT 144U
#define GBEMU_SCALE 3U
#define GBEMU_FRAME_WIDTH (GBEMU_SOURCE_WIDTH * GBEMU_SCALE)
#define GBEMU_FRAME_HEIGHT (GBEMU_SOURCE_HEIGHT * GBEMU_SCALE)
#define GBEMU_FRAME_PITCH_BYTES ((GBEMU_FRAME_WIDTH + 7U) / 8U)
#define GBEMU_FRAMEBUFFER_SIZE (GBEMU_FRAME_PITCH_BYTES * GBEMU_FRAME_HEIGHT)
#define GBEMU_MAX_ROM_BYTES (4U * 1024U * 1024U)

enum {
  GBEMU_INPUT_A = 0x01U,
  GBEMU_INPUT_B = 0x02U,
  GBEMU_INPUT_SELECT = 0x04U,
  GBEMU_INPUT_START = 0x08U,
  GBEMU_INPUT_RIGHT = 0x10U,
  GBEMU_INPUT_LEFT = 0x20U,
  GBEMU_INPUT_UP = 0x40U,
  GBEMU_INPUT_DOWN = 0x80U,
};

typedef enum gbemu_status_e {
  GBEMU_STATUS_OK = 0,
  GBEMU_STATUS_NO_ROM,
  GBEMU_STATUS_ROM_TOO_SMALL,
  GBEMU_STATUS_UNSUPPORTED_ROM_SIZE,
  GBEMU_STATUS_UNSUPPORTED_RAM_SIZE,
  GBEMU_STATUS_ROM_TRUNCATED,
  GBEMU_STATUS_CART_RAM_ALLOC_FAILED,
  GBEMU_STATUS_INIT_CARTRIDGE_UNSUPPORTED,
  GBEMU_STATUS_INIT_INVALID_CHECKSUM,
  GBEMU_STATUS_RUNTIME_ERROR,
  GBEMU_STATUS_INVALID_ARGUMENT,
  GBEMU_STATUS_CGB_ONLY_ROM,
  GBEMU_STATUS_ROM_TOO_LARGE,
} gbemu_status_t;

typedef struct gbemu_frame_stats_s {
  uint32_t run_us;
  uint32_t draw_us;
  uint16_t lines_drawn;
  bool rendered;
} gbemu_frame_stats_t;

typedef struct gbemu_s gbemu_t;

gbemu_t *gbemu_create(void);
void gbemu_destroy(gbemu_t *emu);

gbemu_status_t gbemu_init(gbemu_t *emu, const uint8_t *rom_data, size_t rom_size);
bool gbemu_run_frame(
    gbemu_t *emu,
    uint8_t *framebuffer,
    size_t framebuffer_size,
    uint8_t input_mask,
    bool skip_render,
    gbemu_frame_stats_t *out_stats);
size_t gbemu_get_state_size(const gbemu_t *emu);
bool gbemu_save_state(const gbemu_t *emu, void *buffer, size_t buffer_size);
bool gbemu_load_state(gbemu_t *emu, const void *buffer, size_t buffer_size);

/* Battery-backed cartridge RAM and RTC data, encoded in Paperboy's PBSV format. */
bool gbemu_has_persist(const gbemu_t *emu);
size_t gbemu_get_persist_size(const gbemu_t *emu);
bool gbemu_persist_is_dirty(const gbemu_t *emu);
bool gbemu_export_persist(
    const gbemu_t *emu,
    void *buffer,
    size_t buffer_size,
    uint32_t timestamp);
bool gbemu_import_persist(
    gbemu_t *emu,
    const void *buffer,
    size_t buffer_size,
    uint32_t current_timestamp);
void gbemu_mark_persist_clean(gbemu_t *emu);

void gbemu_reset(gbemu_t *emu);

gbemu_status_t gbemu_get_status(const gbemu_t *emu);
uint16_t gbemu_get_last_error_addr(const gbemu_t *emu);
const char *gbemu_get_last_error_string(const gbemu_t *emu);
const char *gbemu_status_string(gbemu_status_t status);
const char *gbemu_get_rom_title(const gbemu_t *emu);
uint32_t gbemu_get_rom_fingerprint(const gbemu_t *emu);

#ifdef __cplusplus
}
#endif
