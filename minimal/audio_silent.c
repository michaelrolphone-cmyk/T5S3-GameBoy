/* Deliberately silent output, with the complete original MiniGB APU state and
 * synthesis retained in audio.c. No GPIO, PWM, DMA, I2S or audio grant. */
#include "audio.h"
struct minigb_apu_ctx;
void audio_pcm_init(void){}
void audio_pcm_push_samples(const int16_t* p,size_t n){(void)p;(void)n;}
size_t audio_pcm_ring_free(void){return 0;}
void audio_pcm_deinit(void){}
bool audio_pcm_output_available(void){return false;}
void audio_pcm_set_paused(bool p){(void)p;}
void audio_pcm_flush(void){}
void audio_poly_init(void){}
void audio_poly_service_frame(const struct minigb_apu_ctx* p){(void)p;}
void audio_poly_push_samples(const int16_t* p,size_t n){(void)p;(void)n;}
size_t audio_poly_ring_free(void){return 0;}
void audio_poly_deinit(void){}
bool audio_poly_output_available(void){return false;}
void audio_poly_set_paused(bool p){(void)p;}
void audio_poly_flush(void){}
