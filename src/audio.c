#include "audio.h"

#include <limits.h>
#include <string.h>

#include "minigb_apu/minigb_apu.h"
#include "paperboy_config.h"

void audio_pcm_init(void);
void audio_pcm_push_samples(const int16_t *samples, size_t stereo_pair_count);
size_t audio_pcm_ring_free(void);
void audio_pcm_deinit(void);
bool audio_pcm_output_available(void);
void audio_pcm_set_paused(bool paused);
void audio_pcm_flush(void);

void audio_poly_init(void);
void audio_poly_service_frame(const struct minigb_apu_ctx *apu);
void audio_poly_push_samples(const int16_t *samples, size_t stereo_pair_count);
size_t audio_poly_ring_free(void);
void audio_poly_deinit(void);
bool audio_poly_output_available(void);
void audio_poly_set_paused(bool paused);
void audio_poly_flush(void);

#define AUDIO_DMG_CLOCK_HZ 4194304ULL
#define AUDIO_FRAME_CLOCKS 70224ULL
#define AUDIO_FRAME_SAMPLE_NUMERATOR \
  ((uint64_t)PAPERBOY_AUDIO_SAMPLE_RATE * AUDIO_FRAME_CLOCKS)
#define AUDIO_FRAME_SAMPLE_FLOOR \
  (AUDIO_FRAME_SAMPLE_NUMERATOR / AUDIO_DMG_CLOCK_HZ)
#define AUDIO_FRAME_SAMPLE_REMAINDER \
  (AUDIO_FRAME_SAMPLE_NUMERATOR % AUDIO_DMG_CLOCK_HZ)
#define AUDIO_FRAME_SAMPLE_CEILING \
  (AUDIO_FRAME_SAMPLE_FLOOR + (AUDIO_FRAME_SAMPLE_REMAINDER != 0U ? 1U : 0U))

#define AUDIO_APU_STATE_VERSION 1U
#define AUDIO_APU_STATE_HEADER_SIZE 16U
#define AUDIO_APU_STATE_CHANNEL_SIZE 57U
#define AUDIO_APU_STATE_PAYLOAD_SIZE \
  ((4U * AUDIO_APU_STATE_CHANNEL_SIZE) + 8U + AUDIO_MEM_SIZE + 4U)
#define AUDIO_APU_STATE_BLOB_SIZE \
  (AUDIO_APU_STATE_HEADER_SIZE + AUDIO_APU_STATE_PAYLOAD_SIZE)

_Static_assert(PAPERBOY_AUDIO_SAMPLES_PER_FRAME == AUDIO_SAMPLES,
               "configured frame sample floor must match MiniGB APU");
_Static_assert(AUDIO_FRAME_SAMPLE_FLOOR == AUDIO_SAMPLES,
               "unexpected Game Boy frame sample floor");
_Static_assert(AUDIO_FRAME_SAMPLE_CEILING <= AUDIO_SAMPLES_MAX,
               "MiniGB APU frame buffer is too small");
_Static_assert(AUDIO_MEM_SIZE == 48U, "unexpected MiniGB APU register size");

static const uint8_t kApuStateMagic[4] = {'M', 'G', 'A', 'P'};

static audio_engine_t s_engine = AUDIO_ENGINE_PCM;
static bool s_initialized = false;
static bool s_paused = false;
static bool s_apu_initialized = false;
static struct minigb_apu_ctx s_apu;
static int16_t s_apu_samples[AUDIO_SAMPLES_TOTAL_MAX];
static uint32_t s_frame_sample_phase;

static bool valid_apu_addr(uint16_t addr) {
  return addr >= 0xFF10U && addr <= 0xFF3FU;
}

static void flush_selected_output(void) {
  if (!s_initialized) {
    return;
  }

  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      audio_pcm_flush();
      break;
    case AUDIO_ENGINE_POLY:
      audio_poly_flush();
      break;
    default:
      break;
  }
}

void audio_reset_apu(void) {
  minigb_apu_audio_init(&s_apu);
  s_frame_sample_phase = 0U;
  s_apu_initialized = true;
  flush_selected_output();
}

static void ensure_apu_initialized(void) {
  if (!s_apu_initialized) {
    audio_reset_apu();
  }
}

static uint16_t next_frame_sample_count(void) {
  uint64_t phase =
      (uint64_t)s_frame_sample_phase + AUDIO_FRAME_SAMPLE_REMAINDER;
  uint16_t sample_count = (uint16_t)AUDIO_FRAME_SAMPLE_FLOOR;
  if (phase >= AUDIO_DMG_CLOCK_HZ) {
    phase -= AUDIO_DMG_CLOCK_HZ;
    ++sample_count;
  }
  s_frame_sample_phase = (uint32_t)phase;
  return sample_count;
}

void audio_set_engine(audio_engine_t engine) {
  if (!s_initialized && engine >= AUDIO_ENGINE_PCM && engine < AUDIO_ENGINE_COUNT) {
    s_engine = engine;
  }
}

audio_engine_t audio_get_engine(void) {
  return s_engine;
}

const char *audio_engine_name(audio_engine_t engine) {
  switch (engine) {
    case AUDIO_ENGINE_PCM:
      return "PCM";
    case AUDIO_ENGINE_POLY:
      return "Polyphony";
    case AUDIO_ENGINE_MUTE:
      return "Mute";
    default:
      return "?";
  }
}

void audio_init(void) {
  if (s_initialized) {
    return;
  }

  ensure_apu_initialized();
  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      audio_pcm_init();
      break;
    case AUDIO_ENGINE_POLY:
      audio_poly_init();
      break;
    case AUDIO_ENGINE_MUTE:
    default:
      break;
  }
  s_initialized = true;
  audio_set_paused(s_paused);
}

void audio_service_frame(void) {
  uint16_t sample_count;

  if (!s_initialized || s_paused) {
    return;
  }

  ensure_apu_initialized();
  sample_count = next_frame_sample_count();
  minigb_apu_audio_callback_samples(&s_apu, s_apu_samples, sample_count);

  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      audio_pcm_push_samples(s_apu_samples, sample_count);
      break;
    case AUDIO_ENGINE_POLY:
      audio_poly_service_frame(&s_apu);
      break;
    case AUDIO_ENGINE_MUTE:
    default:
      break;
  }
}

void audio_set_paused(bool paused) {
  s_paused = paused;
  if (!s_initialized) {
    return;
  }
  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      audio_pcm_set_paused(paused);
      break;
    case AUDIO_ENGINE_POLY:
      audio_poly_set_paused(paused);
      break;
    default:
      break;
  }
}

void audio_push_samples(const int16_t *samples, size_t stereo_pair_count) {
  if (!s_initialized || samples == NULL) {
    return;
  }

  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      audio_pcm_push_samples(samples, stereo_pair_count);
      break;
    case AUDIO_ENGINE_POLY:
      audio_poly_push_samples(samples, stereo_pair_count);
      break;
    default:
      break;
  }
}

size_t audio_ring_free(void) {
  if (!s_initialized) {
    return 0U;
  }

  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      return audio_pcm_ring_free();
    case AUDIO_ENGINE_POLY:
      return audio_poly_ring_free();
    default:
      return SIZE_MAX;
  }
}

uint8_t audio_apu_read(uint16_t addr) {
  if (!valid_apu_addr(addr)) {
    return 0xFFU;
  }
  ensure_apu_initialized();
  return minigb_apu_audio_read(&s_apu, addr);
}

void audio_apu_write(uint16_t addr, uint8_t value) {
  if (!valid_apu_addr(addr)) {
    return;
  }
  ensure_apu_initialized();
  minigb_apu_audio_write(&s_apu, addr, value);
}

bool audio_output_available(void) {
  if (!s_initialized) {
    return false;
  }

  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      return audio_pcm_output_available();
    case AUDIO_ENGINE_POLY:
      return audio_poly_output_available();
    default:
      return false;
  }
}

void audio_deinit(void) {
  if (!s_initialized) {
    return;
  }

  switch (s_engine) {
    case AUDIO_ENGINE_PCM:
      audio_pcm_deinit();
      break;
    case AUDIO_ENGINE_POLY:
      audio_poly_deinit();
      break;
    default:
      break;
  }
  s_initialized = false;
}

static void state_write_u8(uint8_t **cursor, uint8_t value) {
  *(*cursor)++ = value;
}

static void state_write_u16(uint8_t **cursor, uint16_t value) {
  state_write_u8(cursor, (uint8_t)value);
  state_write_u8(cursor, (uint8_t)(value >> 8U));
}

static void state_write_u32(uint8_t **cursor, uint32_t value) {
  state_write_u16(cursor, (uint16_t)value);
  state_write_u16(cursor, (uint16_t)(value >> 16U));
}

static uint8_t state_read_u8(const uint8_t **cursor) {
  return *(*cursor)++;
}

static uint16_t state_read_u16(const uint8_t **cursor) {
  const uint16_t low = state_read_u8(cursor);
  return (uint16_t)(low | ((uint16_t)state_read_u8(cursor) << 8U));
}

static uint32_t state_read_u32(const uint8_t **cursor) {
  const uint32_t low = state_read_u16(cursor);
  return low | ((uint32_t)state_read_u16(cursor) << 16U);
}

static int32_t state_read_i32(const uint8_t **cursor) {
  const uint32_t bits = state_read_u32(cursor);
  int32_t value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint32_t state_checksum(const uint8_t *data, size_t size) {
  uint32_t hash = 2166136261U;
  for (size_t index = 0U; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

static void state_write_channel(
    uint8_t **cursor, const struct chan *channel, uint8_t index) {
  state_write_u8(cursor, channel->enabled);
  state_write_u8(cursor, channel->powered);
  state_write_u8(cursor, channel->on_left);
  state_write_u8(cursor, channel->on_right);
  state_write_u8(cursor, channel->volume);
  state_write_u8(cursor, channel->volume_init);
  state_write_u16(cursor, channel->freq);
  state_write_u32(cursor, channel->freq_counter);
  state_write_u32(cursor, channel->freq_inc);
  state_write_u32(cursor, (uint32_t)channel->val);
  state_write_u8(cursor, channel->len.load);
  state_write_u8(cursor, channel->len.enabled);
  state_write_u32(cursor, channel->len.counter);
  state_write_u32(cursor, channel->len.inc);
  state_write_u8(cursor, channel->env.step);
  state_write_u8(cursor, channel->env.up);
  state_write_u32(cursor, channel->env.counter);
  state_write_u32(cursor, channel->env.inc);
  state_write_u8(cursor, channel->sweep.rate);
  state_write_u8(cursor, channel->sweep.shift);
  state_write_u8(cursor, channel->sweep.down);
  state_write_u16(cursor, channel->sweep.freq);
  state_write_u32(cursor, channel->sweep.counter);
  state_write_u32(cursor, channel->sweep.inc);

  if (index < 2U) {
    state_write_u8(cursor, channel->square.duty);
    state_write_u8(cursor, channel->square.duty_counter);
    state_write_u16(cursor, 0U);
  } else if (index == 2U) {
    state_write_u8(cursor, channel->wave.sample);
    state_write_u8(cursor, 0U);
    state_write_u16(cursor, 0U);
  } else {
    state_write_u16(cursor, channel->noise.lfsr_reg);
    state_write_u8(cursor, channel->noise.lfsr_wide);
    state_write_u8(cursor, channel->noise.lfsr_div);
  }
}

static void state_read_channel(
    const uint8_t **cursor, struct chan *channel, uint8_t index) {
  channel->enabled = state_read_u8(cursor);
  channel->powered = state_read_u8(cursor);
  channel->on_left = state_read_u8(cursor);
  channel->on_right = state_read_u8(cursor);
  channel->volume = state_read_u8(cursor);
  channel->volume_init = state_read_u8(cursor);
  channel->freq = state_read_u16(cursor);
  channel->freq_counter = state_read_u32(cursor);
  channel->freq_inc = state_read_u32(cursor);
  channel->val = state_read_i32(cursor);
  channel->len.load = state_read_u8(cursor);
  channel->len.enabled = state_read_u8(cursor);
  channel->len.counter = state_read_u32(cursor);
  channel->len.inc = state_read_u32(cursor);
  channel->env.step = state_read_u8(cursor);
  channel->env.up = state_read_u8(cursor);
  channel->env.counter = state_read_u32(cursor);
  channel->env.inc = state_read_u32(cursor);
  channel->sweep.rate = state_read_u8(cursor);
  channel->sweep.shift = state_read_u8(cursor);
  channel->sweep.down = state_read_u8(cursor);
  channel->sweep.freq = state_read_u16(cursor);
  channel->sweep.counter = state_read_u32(cursor);
  channel->sweep.inc = state_read_u32(cursor);

  if (index < 2U) {
    channel->square.duty = state_read_u8(cursor);
    channel->square.duty_counter = state_read_u8(cursor);
    (void)state_read_u16(cursor);
  } else if (index == 2U) {
    channel->wave.sample = state_read_u8(cursor);
    (void)state_read_u8(cursor);
    (void)state_read_u16(cursor);
  } else {
    channel->noise.lfsr_reg = state_read_u16(cursor);
    channel->noise.lfsr_wide = state_read_u8(cursor);
    channel->noise.lfsr_div = state_read_u8(cursor);
  }
}

static bool state_channel_valid(const struct chan *channel, uint8_t index) {
  if (channel->enabled > 1U || channel->powered > 1U ||
      channel->on_left > 1U || channel->on_right > 1U ||
      channel->volume > 15U || channel->volume_init > 15U ||
      (channel->len.enabled != 0U && channel->len.enabled != 0x40U) ||
      channel->env.step > 7U ||
      (channel->env.up != 0U && channel->env.up != 0x08U) ||
      channel->sweep.rate > 7U || channel->sweep.shift > 7U ||
      (channel->sweep.down != 0U && channel->sweep.down != 0x08U)) {
    return false;
  }
  if (index != 2U && channel->len.load > 63U) {
    return false;
  }
  if (index < 3U && channel->freq > 2047U) {
    return false;
  }
  if (index < 2U) {
    return channel->square.duty_counter <= 7U &&
        channel->val >= VOL_INIT_MIN && channel->val <= VOL_INIT_MAX;
  }
  if (index == 2U) {
    return channel->volume <= 3U && channel->volume_init <= 3U &&
        channel->wave.sample <= 15U && channel->val >= 0 && channel->val <= 31;
  }
  return channel->noise.lfsr_wide <= 1U && channel->noise.lfsr_div <= 7U &&
      channel->val >= VOL_INIT_MIN && channel->val <= VOL_INIT_MAX;
}

size_t audio_apu_state_size(void) {
  return AUDIO_APU_STATE_BLOB_SIZE;
}

bool audio_apu_state_export(void *buffer, size_t buffer_size) {
  uint8_t *bytes = (uint8_t *)buffer;
  uint8_t *cursor;
  uint8_t *header;

  if (bytes == NULL || buffer_size < AUDIO_APU_STATE_BLOB_SIZE) {
    return false;
  }
  ensure_apu_initialized();

  cursor = bytes + AUDIO_APU_STATE_HEADER_SIZE;
  for (uint8_t index = 0U; index < 4U; ++index) {
    state_write_channel(&cursor, &s_apu.chans[index], index);
  }
  state_write_u32(&cursor, (uint32_t)s_apu.vol_l);
  state_write_u32(&cursor, (uint32_t)s_apu.vol_r);
  memcpy(cursor, s_apu.audio_mem, AUDIO_MEM_SIZE);
  cursor += AUDIO_MEM_SIZE;
  state_write_u32(&cursor, s_frame_sample_phase);

  header = bytes;
  memcpy(header, kApuStateMagic, sizeof(kApuStateMagic));
  header += sizeof(kApuStateMagic);
  state_write_u16(&header, AUDIO_APU_STATE_VERSION);
  state_write_u16(&header, AUDIO_APU_STATE_HEADER_SIZE);
  state_write_u32(&header, AUDIO_APU_STATE_PAYLOAD_SIZE);
  state_write_u32(
      &header,
      state_checksum(
          bytes + AUDIO_APU_STATE_HEADER_SIZE, AUDIO_APU_STATE_PAYLOAD_SIZE));
  return (size_t)(cursor - bytes) == AUDIO_APU_STATE_BLOB_SIZE;
}

static bool audio_apu_state_decode(
    const void *buffer,
    size_t buffer_size,
    struct minigb_apu_ctx *restored,
    uint32_t *restored_sample_phase) {
  const uint8_t *bytes = (const uint8_t *)buffer;
  const uint8_t *header;
  const uint8_t *cursor;
  uint32_t checksum;
  uint32_t sample_phase;

  if (bytes == NULL || buffer_size != AUDIO_APU_STATE_BLOB_SIZE ||
      memcmp(bytes, kApuStateMagic, sizeof(kApuStateMagic)) != 0) {
    return false;
  }

  header = bytes + sizeof(kApuStateMagic);
  if (state_read_u16(&header) != AUDIO_APU_STATE_VERSION ||
      state_read_u16(&header) != AUDIO_APU_STATE_HEADER_SIZE ||
      state_read_u32(&header) != AUDIO_APU_STATE_PAYLOAD_SIZE) {
    return false;
  }
  checksum = state_read_u32(&header);
  if (checksum != state_checksum(
          bytes + AUDIO_APU_STATE_HEADER_SIZE, AUDIO_APU_STATE_PAYLOAD_SIZE)) {
    return false;
  }

  memset(restored, 0, sizeof(*restored));
  cursor = bytes + AUDIO_APU_STATE_HEADER_SIZE;
  for (uint8_t index = 0U; index < 4U; ++index) {
    state_read_channel(&cursor, &restored->chans[index], index);
    if (!state_channel_valid(&restored->chans[index], index)) {
      return false;
    }
  }
  restored->vol_l = state_read_i32(&cursor);
  restored->vol_r = state_read_i32(&cursor);
  if (restored->vol_l < 0 || restored->vol_l > 7 ||
      restored->vol_r < 0 || restored->vol_r > 7) {
    return false;
  }
  memcpy(restored->audio_mem, cursor, AUDIO_MEM_SIZE);
  cursor += AUDIO_MEM_SIZE;
  sample_phase = state_read_u32(&cursor);
  if (sample_phase >= AUDIO_DMG_CLOCK_HZ ||
      (size_t)(cursor - bytes) != AUDIO_APU_STATE_BLOB_SIZE) {
    return false;
  }

  *restored_sample_phase = sample_phase;
  return true;
}

bool audio_apu_state_validate(const void *buffer, size_t buffer_size) {
  struct minigb_apu_ctx restored;
  uint32_t restored_sample_phase;
  return audio_apu_state_decode(
      buffer, buffer_size, &restored, &restored_sample_phase);
}

bool audio_apu_state_import(const void *buffer, size_t buffer_size) {
  struct minigb_apu_ctx restored;
  uint32_t restored_sample_phase;

  if (!audio_apu_state_decode(
          buffer, buffer_size, &restored, &restored_sample_phase)) {
    return false;
  }

  s_apu = restored;
  s_frame_sample_phase = restored_sample_phase;
  s_apu_initialized = true;
  flush_selected_output();
  return true;
}
