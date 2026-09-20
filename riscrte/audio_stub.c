#include "audio.h"
#include <string.h>

static uint8_t regs[0x30];
static audio_engine_t engine = AUDIO_ENGINE_MUTE;

void audio_set_engine(audio_engine_t value) { engine = value; }
audio_engine_t audio_get_engine(void) { return engine; }
const char *audio_engine_name(audio_engine_t value) {
    switch (value) {
        case AUDIO_ENGINE_PCM: return "PCM";
        case AUDIO_ENGINE_POLY: return "POLY";
        default: return "MUTE";
    }
}
void audio_init(void) {}
void audio_service_frame(void) {}
void audio_set_paused(bool paused) { (void)paused; }
void audio_deinit(void) {}
void audio_reset_apu(void) { memset(regs, 0, sizeof(regs)); }
size_t audio_apu_state_size(void) { return sizeof(regs); }
bool audio_apu_state_export(void *buffer, size_t size) {
    if (!buffer || size < sizeof(regs)) return false;
    memcpy(buffer, regs, sizeof(regs));
    return true;
}
bool audio_apu_state_validate(const void *buffer, size_t size) { return buffer && size == sizeof(regs); }
bool audio_apu_state_import(const void *buffer, size_t size) {
    if (!audio_apu_state_validate(buffer, size)) return false;
    memcpy(regs, buffer, sizeof(regs));
    return true;
}
bool audio_output_available(void) { return false; }
uint8_t audio_apu_read(uint16_t addr) {
    return (addr >= 0xFF10u && addr < 0xFF40u) ? regs[addr - 0xFF10u] : 0xFFu;
}
void audio_apu_write(uint16_t addr, uint8_t value) {
    if (addr >= 0xFF10u && addr < 0xFF40u) regs[addr - 0xFF10u] = value;
}
void audio_push_samples(const int16_t *samples, size_t count) { (void)samples; (void)count; }
size_t audio_ring_free(void) { return 0; }
