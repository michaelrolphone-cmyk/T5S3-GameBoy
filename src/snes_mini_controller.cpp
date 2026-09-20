#include "snes_mini_controller.h"

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "gbemu.h"
#include "t5s3_epd_pins.h"

namespace {

constexpr char kTag[] = "snes_mini";
constexpr uint8_t kAddress = 0x52U;
constexpr uint32_t kPollIntervalMs = 16U;
constexpr uint32_t kReconnectIntervalMs = 1000U;
constexpr uint32_t kConversionDelayMs = 2U;
constexpr uint32_t kDiagnosticIntervalMs = 5000U;
// The original firmware calls Wire.setTimeout(100), which sets Stream's
// timeout, not the I2C transaction timeout. This is the TwoWire-specific API.
// Wire is shared with the touch, RTC and PCA9535, so cap their stall time too.
constexpr uint16_t kBusTimeoutMs = 15U;

// Albert Gonzalez's SNES Mini report: ~(byte[4]<<8 | byte[5]).
constexpr uint16_t kUp = 0x0001U;
constexpr uint16_t kRight = 0x8000U;
constexpr uint16_t kDown = 0x4000U;
constexpr uint16_t kLeft = 0x0002U;
constexpr uint16_t kA = 0x0010U;
constexpr uint16_t kB = 0x0040U;
constexpr uint16_t kX = 0x0008U;
constexpr uint16_t kY = 0x0020U;
constexpr uint16_t kStart = 0x0400U;
constexpr uint16_t kSelect = 0x1000U;

// The console task only loads this atomic byte; it never waits for Wire,
// delays for the controller, or acquires an I2C mutex from inside a frame.
std::atomic<uint8_t> g_buttons{0U};
std::atomic<bool> g_worker_started{false};
uint32_t g_last_start_failure_ms = 0U;

// These fields are owned exclusively by the low-priority I2C worker.
bool g_initialized = false;
bool g_connected = false;
uint32_t g_last_poll_ms = 0U;
uint32_t g_next_probe_ms = 0U;
uint32_t g_last_diagnostic_ms = 0U;

bool bus_idle() {
  // A bad breakout can hold the shared bus low. Never initiate a gamepad
  // probe while the pins are already low. This cannot fix an electrical short.
  return digitalRead(t5s3_epd::kI2cSda) == HIGH &&
         digitalRead(t5s3_epd::kI2cScl) == HIGH;
}

void log_bus_failure(uint32_t now, const char *stage, int result,
                     uint32_t elapsed_ms) {
  if (g_last_diagnostic_ms != 0U &&
      (now - g_last_diagnostic_ms) < kDiagnosticIntervalMs) {
    return;
  }
  g_last_diagnostic_ms = now == 0U ? 1U : now;
  ESP_LOGW(kTag,
           "I2C %s rc=%d elapsed=%lums SDA39=%d SCL40=%d timeout=%ums; "
           "check 3.3V, ground and connector orientation",
           stage, result, static_cast<unsigned long>(elapsed_ms),
           digitalRead(t5s3_epd::kI2cSda),
           digitalRead(t5s3_epd::kI2cScl),
           static_cast<unsigned>(kBusTimeoutMs));
}

void disconnect(uint32_t now, const char *reason, int result,
                uint32_t elapsed_ms) {
  if (g_connected) {
    ESP_LOGW(kTag, "controller disconnected: %s rc=%d", reason, result);
  }
  // Release buttons before attempting any recovery or retry.
  g_buttons.store(0U, std::memory_order_relaxed);
  g_connected = false;
  g_initialized = false;
  g_next_probe_ms = now + kReconnectIntervalMs;
  if (result != 2 || !bus_idle()) {
    // An unconnected address normally NACKs (rc=2); do not spam on absence.
    log_bus_failure(now, reason, result, elapsed_ms);
  }
}

uint8_t write_register(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission();
}

bool probe_and_initialize(uint32_t now) {
  if (!bus_idle()) {
    disconnect(now, "bus held low before probe", -1, 0U);
    return false;
  }
  uint32_t started = millis();
  Wire.beginTransmission(kAddress);
  const uint8_t probe_result = Wire.endTransmission();
  if (probe_result != 0U) {
    disconnect(now, "address 0x52 probe", probe_result, millis() - started);
    return false;
  }

  started = millis();
  const uint8_t init_result = write_register(0xF0U, 0x55U);
  if (init_result != 0U) {
    disconnect(now, "F0/55 init", init_result, millis() - started);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(5));
  started = millis();
  // Some clones ignore FB/00. Verify actual presence using a six-byte report.
  const uint8_t second_result = write_register(0xFBU, 0x00U);
  if (second_result != 0U) {
    log_bus_failure(now, "FB/00 init (optional)", second_result,
                    millis() - started);
  }
  vTaskDelay(pdMS_TO_TICKS(5));
  g_initialized = true;
  g_last_poll_ms = millis() - kPollIntervalMs;
  return true;
}

uint8_t decode_buttons(const uint8_t data[6]) {
  const uint16_t pressed = static_cast<uint16_t>(
      (static_cast<uint16_t>(data[4] ^ 0xFFU) << 8U) |
      static_cast<uint16_t>(data[5] ^ 0xFFU));
  uint8_t input = 0U;
  if (pressed & kUp) input |= GBEMU_INPUT_UP;
  if (pressed & kDown) input |= GBEMU_INPUT_DOWN;
  if (pressed & kLeft) input |= GBEMU_INPUT_LEFT;
  if (pressed & kRight) input |= GBEMU_INPUT_RIGHT;
  if (pressed & (kA | kX)) input |= GBEMU_INPUT_A;
  if (pressed & (kB | kY)) input |= GBEMU_INPUT_B;
  if (pressed & kStart) input |= GBEMU_INPUT_START;
  if (pressed & kSelect) input |= GBEMU_INPUT_SELECT;
  return input;
}

void poll_controller(uint32_t now) {
  if (!g_initialized) {
    if (static_cast<int32_t>(now - g_next_probe_ms) < 0 ||
        !probe_and_initialize(now)) {
      return;
    }
  }
  if ((now - g_last_poll_ms) < kPollIntervalMs) {
    return;
  }
  g_last_poll_ms = now;

  const uint32_t started = millis();
  Wire.beginTransmission(kAddress);
  Wire.write(0x00U);
  const uint8_t request_result = Wire.endTransmission();
  if (request_result != 0U) {
    disconnect(millis(), "report request", request_result, millis() - started);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(kConversionDelayMs));

  uint8_t data[6] = {};
  const uint32_t read_started = millis();
  const size_t count = Wire.requestFrom(static_cast<int>(kAddress), 6);
  if (count != sizeof(data)) {
    // Only drain data produced by this read; no more transactions this cycle.
    while (Wire.available()) (void)Wire.read();
    disconnect(millis(), "short report", static_cast<int>(count),
               millis() - read_started);
    return;
  }
  for (uint8_t &byte : data) {
    if (!Wire.available()) {
      disconnect(millis(), "incomplete report", -2,
                 millis() - read_started);
      return;
    }
    byte = static_cast<uint8_t>(Wire.read());
  }
  if ((data[4] & 0x01U) == 0U) {
    disconnect(millis(), "invalid report flags", -3,
               millis() - read_started);
    return;
  }
  g_buttons.store(decode_buttons(data), std::memory_order_relaxed);
  if (!g_connected) {
    ESP_LOGI(kTag, "controller connected at 0x%02X", kAddress);
    g_connected = true;
  }
}

void controller_worker(void *unused) {
  (void)unused;
  ESP_LOGI(kTag, "I2C worker started on core %d; frame reads are nonblocking",
           xPortGetCoreID());
  for (;;) {
    poll_controller(millis());
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

uint8_t snes_mini_controller_buttons() {
  // Start the worker lazily after main.cpp has initialized Wire, the panel,
  // touch and battery. No controller I2C operation ever runs on this caller.
  bool expected = false;
  if (g_worker_started.compare_exchange_strong(
          expected, true, std::memory_order_relaxed)) {
    // Correct API: Stream::setTimeout() does not constrain I2C bus operations.
    Wire.setTimeOut(kBusTimeoutMs);
    const BaseType_t result = xTaskCreatePinnedToCore(
        controller_worker, "snes_i2c", 4096, nullptr, 1, nullptr, 1);
    if (result != pdPASS) {
      g_worker_started.store(false, std::memory_order_relaxed);
      g_buttons.store(0U, std::memory_order_relaxed);
      const uint32_t now = millis();
      if (g_last_start_failure_ms == 0U ||
          (now - g_last_start_failure_ms) >= kDiagnosticIntervalMs) {
        ESP_LOGE(kTag, "could not create controller worker; touchscreen remains active");
        g_last_start_failure_ms = now == 0U ? 1U : now;
      }
    }
  }
  return g_buttons.load(std::memory_order_relaxed);
}
