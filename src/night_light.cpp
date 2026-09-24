#include "night_light.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/ledc.h>
#include <esp_err.h>
#include <esp_log.h>

#include "t5s3_epd_pins.h"

namespace {

constexpr char kTag[] = "night_light";
constexpr char kPreferencesNamespace[] = "gb-light";
constexpr char kBrightnessKey[] = "level";
constexpr char kBrightnessTenthsKey[] = "level10";
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
// Game audio owns LEDC timer 0/channel 0. Never share its timer: the
// audio engine changes that timer's frequency dynamically.
constexpr ledc_timer_t kTimer = LEDC_TIMER_2;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_2;
constexpr uint32_t kPwmHz = 1000U;  // PT4103B23F EN: <= approximately 1 kHz.
constexpr uint32_t kDutyMax = 1023U;
constexpr uint16_t kMaxBrightnessTenths = 100U;  // 10.0%
constexpr uint16_t kFineBrightnessThresholdTenths = 10U;  // 1.0%

bool g_ready = false;
uint16_t g_brightness_tenths = 0U;

bool apply_brightness(uint16_t brightness_tenths) {
  // One tenth of one percent maps to about one count at 10-bit resolution.
  const uint32_t duty =
      (static_cast<uint32_t>(brightness_tenths) * kDutyMax + 500U) / 1000U;
  const esp_err_t set_result = ledc_set_duty(kMode, kChannel, duty);
  if (set_result != ESP_OK) {
    ESP_LOGE(kTag, "set duty failed: %s", esp_err_to_name(set_result));
    return false;
  }
  const esp_err_t update_result = ledc_update_duty(kMode, kChannel);
  if (update_result != ESP_OK) {
    ESP_LOGE(kTag, "update duty failed: %s", esp_err_to_name(update_result));
    return false;
  }
  return true;
}

}  // namespace

void night_light_init() {
  if (g_ready) {
    return;
  }

  // Keep the LED boost converter off until both the pin and timer are ready.
  pinMode(t5s3_epd::kBacklightEnable, OUTPUT);
  digitalWrite(t5s3_epd::kBacklightEnable, LOW);

  uint16_t saved_tenths = 0U;
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, true)) {
    const uint16_t saved_precise =
        preferences.getUShort(kBrightnessTenthsKey, UINT16_MAX);
    if (saved_precise != UINT16_MAX) {
      saved_tenths = saved_precise;
    } else {
      // Migrate the original whole-percent value on first boot after upgrade.
      uint8_t saved_percent = preferences.getUChar(kBrightnessKey, 0U);
      if (saved_percent > 10U) saved_percent = 10U;
      saved_tenths = static_cast<uint16_t>(saved_percent) * 10U;
    }
    preferences.end();
  } else {
    ESP_LOGW(kTag, "NVS unavailable; starting with light off");
  }
  if (saved_tenths > kMaxBrightnessTenths) {
    saved_tenths = kMaxBrightnessTenths;
  }

  ledc_timer_config_t timer = {};
  timer.speed_mode = kMode;
  timer.timer_num = kTimer;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.freq_hz = kPwmHz;
  timer.clk_cfg = LEDC_AUTO_CLK;
  esp_err_t result = ledc_timer_config(&timer);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "PWM timer init failed: %s", esp_err_to_name(result));
    return;
  }

  ledc_channel_config_t channel = {};
  channel.gpio_num = t5s3_epd::kBacklightEnable;
  channel.speed_mode = kMode;
  channel.channel = kChannel;
  channel.intr_type = LEDC_INTR_DISABLE;
  channel.timer_sel = kTimer;
  channel.duty = 0U;
  channel.hpoint = 0U;
  result = ledc_channel_config(&channel);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "PWM channel init failed: %s", esp_err_to_name(result));
    digitalWrite(t5s3_epd::kBacklightEnable, LOW);
    return;
  }

  g_ready = true;
  if (apply_brightness(saved_tenths)) {
    g_brightness_tenths = saved_tenths;
  }
  ESP_LOGI(kTag, "ready gpio=%u PWM=%luHz brightness=%u.%u%% (max 10.0%%)",
           t5s3_epd::kBacklightEnable,
           static_cast<unsigned long>(kPwmHz),
           static_cast<unsigned>(g_brightness_tenths / 10U),
           static_cast<unsigned>(g_brightness_tenths % 10U));
}

uint16_t night_light_brightness_tenths() {
  return g_brightness_tenths;
}

bool night_light_set_brightness_tenths(uint16_t tenths_percent) {
  if (!g_ready) {
    return false;
  }
  if (tenths_percent > kMaxBrightnessTenths) {
    tenths_percent = kMaxBrightnessTenths;
  }
  if (tenths_percent == g_brightness_tenths) {
    return true;
  }
  if (!apply_brightness(tenths_percent)) {
    return false;
  }
  g_brightness_tenths = tenths_percent;
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, false)) {
    if (preferences.putUShort(kBrightnessTenthsKey, tenths_percent) != sizeof(uint16_t)) {
      ESP_LOGW(kTag, "could not persist brightness; change remains active");
    }
    preferences.end();
  } else {
    ESP_LOGW(kTag, "NVS unavailable; brightness change remains active");
  }
  ESP_LOGI(kTag, "brightness=%u.%u%% duty=%lu/%lu",
           static_cast<unsigned>(tenths_percent / 10U),
           static_cast<unsigned>(tenths_percent % 10U),
           static_cast<unsigned long>(
               (static_cast<uint32_t>(tenths_percent) * kDutyMax + 500U) / 1000U),
           static_cast<unsigned long>(kDutyMax));
  return true;
}

bool night_light_adjust_brightness(bool brighter) {
  const uint16_t current = g_brightness_tenths;
  uint16_t target = current;

  if (brighter) {
    if (current < kFineBrightnessThresholdTenths) {
      target = static_cast<uint16_t>(current + 1U);
    } else if (current < kMaxBrightnessTenths) {
      target = static_cast<uint16_t>(
          current + 10U > kMaxBrightnessTenths
              ? kMaxBrightnessTenths
              : current + 10U);
    }
  } else {
    if (current <= kFineBrightnessThresholdTenths) {
      target = current == 0U ? 0U : static_cast<uint16_t>(current - 1U);
    } else {
      target = static_cast<uint16_t>(current - 10U);
    }
  }

  return night_light_set_brightness_tenths(target);
}

uint8_t night_light_brightness() {
  return static_cast<uint8_t>(g_brightness_tenths / 10U);
}

bool night_light_set_brightness(uint8_t percent) {
  if (percent > 10U) percent = 10U;
  return night_light_set_brightness_tenths(static_cast<uint16_t>(percent) * 10U);
}

void night_light_shutdown() {
  if (g_ready) {
    (void)ledc_stop(kMode, kChannel, 0U);
  }
  g_ready = false;
  g_brightness_tenths = 0U;
  pinMode(t5s3_epd::kBacklightEnable, OUTPUT);
  digitalWrite(t5s3_epd::kBacklightEnable, LOW);
}
