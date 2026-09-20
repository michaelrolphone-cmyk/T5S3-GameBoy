/* RiscRTE GameBoy: retain the original MiniGB APU and savestate data, but
 * supply silent physical output backends. No speaker/PWM driver is exported by
 * the existing RiscRTE app ABI; attempting to import ESP-IDF LEDC would fail
 * ELF relocation and/or contend with unrelated firmware hardware. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../src/minigb_apu/minigb_apu.h"

void audio_pcm_init(void) {}
void audio_pcm_push_samples(const int16_t *samples, size_t stereo_pair_count) {
    (void)samples; (void)stereo_pair_count;
}
size_t audio_pcm_ring_free(void) { return SIZE_MAX; }
void audio_pcm_deinit(void) {}
bool audio_pcm_output_available(void) { return false; }
void audio_pcm_set_paused(bool paused) { (void)paused; }
void audio_pcm_flush(void) {}

void audio_poly_init(void) {}
void audio_poly_service_frame(const struct minigb_apu_ctx *apu) { (void)apu; }
void audio_poly_push_samples(const int16_t *samples, size_t stereo_pair_count) {
    (void)samples; (void)stereo_pair_count;
}
size_t audio_poly_ring_free(void) { return SIZE_MAX; }
void audio_poly_deinit(void) {}
bool audio_poly_output_available(void) { return false; }
void audio_poly_set_paused(bool paused) { (void)paused; }
void audio_poly_flush(void) {}
