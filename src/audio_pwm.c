#include "audio.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "hal/ledc_ll.h"
#include "soc/interrupts.h"
#include "soc/ledc_struct.h"

#include "minigb_apu/minigb_apu.h"
#include "paperboy_config.h"

#define AUDIO_LEDC_MODE LEDC_LOW_SPEED_MODE
#define AUDIO_LEDC_TIMER LEDC_TIMER_0
#define AUDIO_LEDC_CHANNEL LEDC_CHANNEL_0
#define AUDIO_DUTY_BITS 10U
#define AUDIO_DUTY_MAX (1U << AUDIO_DUTY_BITS)
#define AUDIO_DUTY_MID (AUDIO_DUTY_MAX / 2U)
#define AUDIO_LEDC_OVF_MASK (1U << (unsigned)AUDIO_LEDC_TIMER)
#define AUDIO_RING_MASK (PAPERBOY_AUDIO_RING_SAMPLES - 1U)

_Static_assert(PAPERBOY_AUDIO_SAMPLE_RATE == AUDIO_SAMPLE_RATE,
               "MiniGB APU and PWM sample rates must match");
_Static_assert(PAPERBOY_AUDIO_SAMPLES_PER_FRAME == AUDIO_SAMPLES,
               "MiniGB APU and frame sample counts must match");
_Static_assert(PAPERBOY_AUDIO_RING_SAMPLES >= AUDIO_SAMPLES_MAX,
               "audio ring must hold at least one synthesized frame");
_Static_assert((PAPERBOY_AUDIO_RING_SAMPLES & AUDIO_RING_MASK) == 0U,
               "audio ring size must be a power of two");

static const char *const kTag = "paperboy_audio";
static int16_t s_ring[PAPERBOY_AUDIO_RING_SAMPLES];
static _Atomic uint32_t s_ring_head;
static _Atomic uint32_t s_ring_tail;
static intr_handle_t s_interrupt;
static bool s_output_available;
static bool s_channel_configured;

void audio_pcm_push_samples(const int16_t *samples, size_t stereo_pair_count);
void audio_pcm_flush(void);

static uint32_t ring_count(void) {
  const uint32_t head = atomic_load_explicit(&s_ring_head, memory_order_acquire);
  const uint32_t tail = atomic_load_explicit(&s_ring_tail, memory_order_acquire);
  const uint32_t count = head - tail;
  return count > PAPERBOY_AUDIO_RING_SAMPLES ? PAPERBOY_AUDIO_RING_SAMPLES : count;
}

static uint32_t ring_free_count(void) {
  return PAPERBOY_AUDIO_RING_SAMPLES - ring_count();
}

static void IRAM_ATTR audio_pwm_isr(void *arg) {
  (void)arg;
  if ((LEDC.int_st.val & AUDIO_LEDC_OVF_MASK) == 0U) {
    return;
  }
  LEDC.int_clr.val = AUDIO_LEDC_OVF_MASK;

  const uint32_t tail = atomic_load_explicit(&s_ring_tail, memory_order_relaxed);
  const uint32_t head = atomic_load_explicit(&s_ring_head, memory_order_acquire);
  uint32_t duty = AUDIO_DUTY_MID;

  if (tail != head) {
    const int16_t sample = s_ring[tail & AUDIO_RING_MASK];
    atomic_store_explicit(&s_ring_tail, tail + 1U, memory_order_release);

    int32_t scaled = ((int32_t)sample >> (16U - AUDIO_DUTY_BITS)) + AUDIO_DUTY_MID;
    if (scaled < 0) {
      scaled = 0;
    } else if (scaled >= (int32_t)AUDIO_DUTY_MAX) {
      scaled = (int32_t)AUDIO_DUTY_MAX - 1;
    }
    duty = (uint32_t)scaled;
  }

  ledc_ll_set_duty_int_part(
      &LEDC, AUDIO_LEDC_MODE, AUDIO_LEDC_CHANNEL, duty);
  ledc_ll_ls_channel_update(&LEDC, AUDIO_LEDC_MODE, AUDIO_LEDC_CHANNEL);
}

static void reset_ring(void) {
  const uint32_t head = atomic_load_explicit(&s_ring_head, memory_order_acquire);
  atomic_store_explicit(&s_ring_tail, head, memory_order_release);
}

void audio_pcm_init(void) {
  reset_ring();
  s_output_available = false;
  s_channel_configured = false;

#if PAPERBOY_AUDIO_GPIO < 0
    ESP_LOGI(kTag, "PCM synthesis ready; physical audio output is disabled");
    return;
#else
  const int output_gpio = PAPERBOY_AUDIO_GPIO;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(output_gpio)) {
    ESP_LOGE(kTag, "invalid audio output GPIO %d", output_gpio);
    return;
  }

  ledc_timer_config_t timer_config;
  memset(&timer_config, 0, sizeof(timer_config));
  timer_config.speed_mode = AUDIO_LEDC_MODE;
  timer_config.timer_num = AUDIO_LEDC_TIMER;
  timer_config.duty_resolution = LEDC_TIMER_10_BIT;
  timer_config.freq_hz = PAPERBOY_AUDIO_SAMPLE_RATE;
  timer_config.clk_cfg = LEDC_AUTO_CLK;

  esp_err_t err = ledc_timer_config(&timer_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "PWM timer init failed: %s", esp_err_to_name(err));
    return;
  }

  ledc_channel_config_t channel_config;
  memset(&channel_config, 0, sizeof(channel_config));
  channel_config.gpio_num = output_gpio;
  channel_config.speed_mode = AUDIO_LEDC_MODE;
  channel_config.channel = AUDIO_LEDC_CHANNEL;
  channel_config.intr_type = LEDC_INTR_DISABLE;
  channel_config.timer_sel = AUDIO_LEDC_TIMER;
  channel_config.duty = AUDIO_DUTY_MID;
  channel_config.hpoint = 0U;

  err = ledc_channel_config(&channel_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "PWM channel init failed: %s", esp_err_to_name(err));
    return;
  }
  s_channel_configured = true;

  LEDC.int_clr.val = AUDIO_LEDC_OVF_MASK;
  err = esp_intr_alloc(
      ETS_LEDC_INTR_SOURCE,
      ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
      audio_pwm_isr,
      NULL,
      &s_interrupt);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "PWM interrupt init failed: %s", esp_err_to_name(err));
    ledc_stop(AUDIO_LEDC_MODE, AUDIO_LEDC_CHANNEL, 0U);
    s_channel_configured = false;
    return;
  }

  LEDC.int_ena.val |= AUDIO_LEDC_OVF_MASK;
  s_output_available = true;
  ESP_LOGI(
      kTag,
      "PCM PWM ready on GPIO%d at %u Hz",
      output_gpio,
      (unsigned)ledc_get_freq(AUDIO_LEDC_MODE, AUDIO_LEDC_TIMER));
#endif
}

void audio_pcm_push_samples(const int16_t *samples, size_t stereo_pair_count) {
  if (!s_output_available || samples == NULL || stereo_pair_count == 0U) {
    return;
  }

  uint32_t head = atomic_load_explicit(&s_ring_head, memory_order_relaxed);
  const size_t free_count = (size_t)ring_free_count();
  if (stereo_pair_count > free_count) {
    stereo_pair_count = free_count;
  }

  for (size_t i = 0; i < stereo_pair_count; ++i) {
    const int32_t mono = ((int32_t)samples[i * 2U] + samples[i * 2U + 1U]) / 2;
    s_ring[(head + (uint32_t)i) & AUDIO_RING_MASK] = (int16_t)mono;
  }
  atomic_store_explicit(
      &s_ring_head, head + (uint32_t)stereo_pair_count, memory_order_release);
}

size_t audio_pcm_ring_free(void) {
  return s_output_available ? (size_t)ring_free_count() : 0U;
}

bool audio_pcm_output_available(void) {
  return s_output_available;
}

void audio_pcm_set_paused(bool paused) {
  (void)paused;
  audio_pcm_flush();
}

void audio_pcm_flush(void) {
  reset_ring();
  if (s_channel_configured) {
    ledc_set_duty(AUDIO_LEDC_MODE, AUDIO_LEDC_CHANNEL, AUDIO_DUTY_MID);
    ledc_update_duty(AUDIO_LEDC_MODE, AUDIO_LEDC_CHANNEL);
  }
}

void audio_pcm_deinit(void) {
  s_output_available = false;
  if (s_interrupt != NULL) {
    LEDC.int_ena.val &= ~AUDIO_LEDC_OVF_MASK;
    LEDC.int_clr.val = AUDIO_LEDC_OVF_MASK;
    esp_intr_free(s_interrupt);
    s_interrupt = NULL;
  }
  if (s_channel_configured) {
    ledc_stop(AUDIO_LEDC_MODE, AUDIO_LEDC_CHANNEL, 0U);
    s_channel_configured = false;
  }
  reset_ring();
}
