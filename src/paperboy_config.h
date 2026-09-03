#pragma once

/*
 * T5S3 Pro has no onboard speaker, and GPIO1 is wired to LORA_RST. Audio is
 * disabled at the pin by default to avoid driving or back-powering that rail.
 * Set this macro at build time only after choosing an isolated free output.
 */
#ifndef PAPERBOY_AUDIO_GPIO
#define PAPERBOY_AUDIO_GPIO (-1)
#endif

#ifndef PAPERBOY_AUDIO_SAMPLE_RATE
#define PAPERBOY_AUDIO_SAMPLE_RATE 32768U
#endif

/* Floor of 32768 * 70224 / 4194304. The audio scheduler adds 5/8 sample/frame. */
#ifndef PAPERBOY_AUDIO_SAMPLES_PER_FRAME
#define PAPERBOY_AUDIO_SAMPLES_PER_FRAME 548U
#endif

#ifndef PAPERBOY_AUDIO_RING_SAMPLES
#define PAPERBOY_AUDIO_RING_SAMPLES 4096U
#endif
