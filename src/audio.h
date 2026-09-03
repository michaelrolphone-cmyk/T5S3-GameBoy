#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum audio_engine_e {
  AUDIO_ENGINE_PCM = 0,
  AUDIO_ENGINE_POLY = 1,
  AUDIO_ENGINE_MUTE = 2,
  AUDIO_ENGINE_COUNT,
} audio_engine_t;

/* The engine can be changed while stopped. Calling audio_deinit() unlocks it. */
void audio_set_engine(audio_engine_t engine);
audio_engine_t audio_get_engine(void);
const char *audio_engine_name(audio_engine_t engine);

void audio_init(void);
void audio_service_frame(void);
void audio_set_paused(bool paused);
void audio_deinit(void);

/* Reset for a new ROM. Output init/deinit and engine switches preserve APU state. */
void audio_reset_apu(void);

/*
 * Versioned full MiniGB APU state, including synthesis counters and the
 * fractional frame/sample phase. Engine, pause, and hardware state are not
 * included. Import flushes queued output and rejects malformed blobs.
 */
size_t audio_apu_state_size(void);
bool audio_apu_state_export(void *buffer, size_t buffer_size);
/* Validate a serialized APU state without changing synthesis or output state. */
bool audio_apu_state_validate(const void *buffer, size_t buffer_size);
bool audio_apu_state_import(const void *buffer, size_t buffer_size);

/* True only when the selected physical PWM backend initialized successfully. */
bool audio_output_available(void);

uint8_t audio_apu_read(uint16_t addr);
void audio_apu_write(uint16_t addr, uint8_t value);

/* Kept for compatibility with the upstream Paperboy audio interface. */
void audio_push_samples(const int16_t *samples, size_t stereo_pair_count);
size_t audio_ring_free(void);

#ifdef __cplusplus
}
#endif
