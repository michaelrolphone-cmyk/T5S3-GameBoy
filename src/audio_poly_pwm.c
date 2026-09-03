#include "audio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

#include "minigb_apu/minigb_apu.h"
#include "paperboy_config.h"

#define POLY_LEDC_MODE LEDC_LOW_SPEED_MODE
#define POLY_LEDC_TIMER LEDC_TIMER_0
#define POLY_LEDC_CHANNEL LEDC_CHANNEL_0
#define POLY_DUTY_HALF 2048U
#define POLY_MIN_HZ 65U
#define POLY_MAX_HZ 16000U

static const char *const kTag = "paperboy_audio";
static const uint16_t kNoiseDivisors[8] = {8U, 16U, 32U, 48U, 64U, 80U, 96U, 112U};
static uint8_t s_channel = 3U;
static bool s_output_available;
static bool s_channel_configured;

static uint8_t wave_nibble(const struct minigb_apu_ctx *apu, uint8_t index) {
  const uint8_t value = apu->audio_mem[
      (0xFF30U - AUDIO_ADDR_COMPENSATION) + (index >> 1U)];
  return (index & 1U) != 0U ? (value & 0x0FU) : (value >> 4U);
}

static uint8_t wave_cycle_multiplier(const struct minigb_apu_ctx *apu) {
  static const uint8_t kPeriods[] = {2U, 4U, 8U, 16U};
  for (size_t i = 0; i < sizeof(kPeriods); ++i) {
    const uint8_t period = kPeriods[i];
    bool matches = true;
    for (uint8_t sample = 0U; sample < (uint8_t)(32U - period); ++sample) {
      if (wave_nibble(apu, sample) !=
          wave_nibble(apu, (uint8_t)(sample + period))) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return (uint8_t)(32U / period);
    }
  }
  return 1U;
}

static uint32_t channel_frequency(
    const struct minigb_apu_ctx *apu, uint8_t index) {
  const struct chan *channel = &apu->chans[index];
  if (!channel->enabled || !channel->powered || channel->volume == 0U) {
    return 0U;
  }

  uint32_t frequency = 0U;
  if (index < 2U && channel->freq < 2048U) {
    frequency = 131072U / (2048U - channel->freq);
  } else if (index == 2U && channel->freq < 2048U) {
    frequency = 65536U / (2048U - channel->freq);
    frequency *= wave_cycle_multiplier(apu);
  } else if (index == 3U && channel->noise.lfsr_div < 8U && channel->freq < 14U) {
    const uint32_t divisor =
        (uint32_t)kNoiseDivisors[channel->noise.lfsr_div] << channel->freq;
    frequency = (4194304U / divisor) / 128U;
  }

  return frequency >= POLY_MIN_HZ && frequency <= POLY_MAX_HZ ? frequency : 0U;
}

static void set_silent(void) {
  ledc_set_duty(POLY_LEDC_MODE, POLY_LEDC_CHANNEL, 0U);
  ledc_update_duty(POLY_LEDC_MODE, POLY_LEDC_CHANNEL);
}

void audio_poly_init(void) {
  s_channel = 3U;
  s_output_available = false;
  s_channel_configured = false;

#if PAPERBOY_AUDIO_GPIO < 0
    ESP_LOGI(kTag, "poly synthesis ready; physical audio output is disabled");
    return;
#else
  const int output_gpio = PAPERBOY_AUDIO_GPIO;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(output_gpio)) {
    ESP_LOGE(kTag, "invalid audio output GPIO %d", output_gpio);
    return;
  }

  ledc_timer_config_t timer_config;
  memset(&timer_config, 0, sizeof(timer_config));
  timer_config.speed_mode = POLY_LEDC_MODE;
  timer_config.timer_num = POLY_LEDC_TIMER;
  timer_config.duty_resolution = LEDC_TIMER_12_BIT;
  timer_config.freq_hz = 1000U;
  timer_config.clk_cfg = LEDC_AUTO_CLK;

  esp_err_t err = ledc_timer_config(&timer_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "poly timer init failed: %s", esp_err_to_name(err));
    return;
  }

  ledc_channel_config_t channel_config;
  memset(&channel_config, 0, sizeof(channel_config));
  channel_config.gpio_num = output_gpio;
  channel_config.speed_mode = POLY_LEDC_MODE;
  channel_config.channel = POLY_LEDC_CHANNEL;
  channel_config.intr_type = LEDC_INTR_DISABLE;
  channel_config.timer_sel = POLY_LEDC_TIMER;
  channel_config.duty = 0U;
  channel_config.hpoint = 0U;

  err = ledc_channel_config(&channel_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "poly channel init failed: %s", esp_err_to_name(err));
    return;
  }

  s_channel_configured = true;
  s_output_available = true;
  ESP_LOGI(kTag, "poly PWM ready on GPIO%d", output_gpio);
#endif
}

void audio_poly_service_frame(const struct minigb_apu_ctx *apu) {
  if (!s_output_available) {
    return;
  }

  s_channel = (uint8_t)((s_channel + 1U) & 3U);
  uint32_t frequency = 0U;
  for (uint8_t offset = 0U; offset < 4U; ++offset) {
    frequency = channel_frequency(apu, (uint8_t)((s_channel + offset) & 3U));
    if (frequency != 0U) {
      break;
    }
  }

  if (frequency == 0U ||
      ledc_set_freq(POLY_LEDC_MODE, POLY_LEDC_TIMER, frequency) == 0U) {
    set_silent();
    return;
  }

  ledc_set_duty(POLY_LEDC_MODE, POLY_LEDC_CHANNEL, POLY_DUTY_HALF);
  ledc_update_duty(POLY_LEDC_MODE, POLY_LEDC_CHANNEL);
}

void audio_poly_push_samples(const int16_t *samples, size_t stereo_pair_count) {
  (void)samples;
  (void)stereo_pair_count;
}

size_t audio_poly_ring_free(void) {
  return SIZE_MAX;
}

bool audio_poly_output_available(void) {
  return s_output_available;
}

void audio_poly_set_paused(bool paused) {
  if (paused && s_channel_configured) {
    set_silent();
  }
}

void audio_poly_flush(void) {
  s_channel = 3U;
  if (s_channel_configured) {
    set_silent();
  }
}

void audio_poly_deinit(void) {
  if (s_channel_configured) {
    set_silent();
    ledc_stop(POLY_LEDC_MODE, POLY_LEDC_CHANNEL, 0U);
    s_channel_configured = false;
  }
  s_output_available = false;
}
