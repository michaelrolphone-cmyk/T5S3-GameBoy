#include "touch_gt911.h"
#include "touch_provider_adapter.h"

#include <Arduino.h>
#include <RiscTouchV1.h>
#include <T5ProviderCapabilityApi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <stddef.h>
#include <string.h>

namespace {
constexpr const char *kTag = "touch_provider";
constexpr uint32_t kAcquireRetryMs = 1000u;
constexpr uint16_t kFallbackRawWidth = 540u;
constexpr uint16_t kFallbackRawHeight = 960u;

const t5_provider_capability_api_v1 *g_provider = nullptr;
t5_provider_capability_lease_t g_touch_lease = 0;
const risc_touch_api_v1 *g_touch_api = nullptr;
bool g_owner_active = false;
uint32_t g_last_acquire_ms = 0;

portMUX_TYPE g_touch_lock = portMUX_INITIALIZER_UNLOCKED;
risc_touch_snapshot_v1 g_snapshot = {};
bool g_snapshot_valid = false;
int g_rotation = 0;

void clear_snapshot() {
  portENTER_CRITICAL(&g_touch_lock);
  g_snapshot = {};
  g_snapshot.width = kFallbackRawWidth;
  g_snapshot.height = kFallbackRawHeight;
  g_snapshot_valid = false;
  portEXIT_CRITICAL(&g_touch_lock);
}

void publish_snapshot(const risc_touch_snapshot_v1 &snapshot) {
  portENTER_CRITICAL(&g_touch_lock);
  g_snapshot = snapshot;
  g_snapshot_valid = true;
  portEXIT_CRITICAL(&g_touch_lock);
}

bool valid_api(const risc_touch_api_v1 *api) {
  return api && api->api_version == RISC_TOUCH_API_V1 &&
      api->struct_size >= sizeof(*api) && api->poll && api->snapshot;
}

void transform_point(uint16_t raw_x, uint16_t raw_y,
                     uint16_t raw_width, uint16_t raw_height,
                     int rotation, uint16_t *out_x, uint16_t *out_y,
                     bool *valid) {
  int32_t x = raw_x;
  int32_t y = raw_y;
  int32_t span_x = raw_width ? raw_width : kFallbackRawWidth;
  int32_t span_y = raw_height ? raw_height : kFallbackRawHeight;

#if TOUCH_SWAP_XY
  {
    const int32_t swap_value = x;
    x = y;
    y = swap_value;
    const int32_t swap_span = span_x;
    span_x = span_y;
    span_y = swap_span;
  }
#endif
#if TOUCH_INVERT_X
  x = span_x - 1 - x;
#endif
#if TOUCH_INVERT_Y
  y = span_y - 1 - y;
#endif

  switch (rotation & 3) {
    case 1: {
      const int32_t nx = span_y - 1 - y;
      y = x;
      x = nx;
      const int32_t swap_span = span_x;
      span_x = span_y;
      span_y = swap_span;
      break;
    }
    case 2:
      x = span_x - 1 - x;
      y = span_y - 1 - y;
      break;
    case 3: {
      const int32_t ny = span_x - 1 - x;
      x = y;
      y = ny;
      const int32_t swap_span = span_x;
      span_x = span_y;
      span_y = swap_span;
      break;
    }
    default:
      break;
  }

  *valid = x >= 0 && y >= 0 && x < span_x && y < span_y;
  if (*valid) {
    *out_x = static_cast<uint16_t>(x);
    *out_y = static_cast<uint16_t>(y);
  }
}

void try_acquire() {
  if (!g_owner_active || g_touch_api || g_touch_lease) return;
  g_last_acquire_ms = millis();
  if (!g_provider)
    g_provider = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
  if (!g_provider || g_provider->api_version != T5_PROVIDER_CAPABILITY_API_VERSION ||
      g_provider->struct_size <
          offsetof(t5_provider_capability_api_v1, release) + sizeof(g_provider->release) ||
      !g_provider->acquire || !g_provider->release) {
    g_provider = nullptr;
    return;
  }

  const void *iface = nullptr;
  if (!g_provider->acquire("input.touch.raw", RISC_TOUCH_API_V1,
                           &g_touch_lease, &iface)) {
    g_touch_lease = 0;
    return;
  }
  const auto *candidate = static_cast<const risc_touch_api_v1 *>(iface);
  if (!valid_api(candidate)) {
    (void)g_provider->release(g_touch_lease);
    g_touch_lease = 0;
    return;
  }
  g_touch_api = candidate;

  risc_touch_snapshot_v1 snapshot{};
  if (g_touch_api->snapshot(g_touch_api->context, &snapshot) &&
      snapshot.contact_count <= RISC_TOUCH_MAX_CONTACTS) {
    publish_snapshot(snapshot);
  } else {
    clear_snapshot();
  }
  ESP_LOGI(kTag, "RiscRTE input.touch.raw acquired");
}
}  // namespace

void paperboy_touch_owner_begin() {
  g_owner_active = true;
  g_last_acquire_ms = millis() - kAcquireRetryMs;
  clear_snapshot();
  try_acquire();
}

void paperboy_touch_owner_poll() {
  if (!g_owner_active) return;
  if (!g_touch_api) {
    if (uint32_t(millis() - g_last_acquire_ms) >= kAcquireRetryMs) try_acquire();
    return;
  }

  const bool poll_ok = g_touch_api->poll(g_touch_api->context, 16u);
  risc_touch_snapshot_v1 snapshot{};
  if (g_touch_api->snapshot(g_touch_api->context, &snapshot) &&
      snapshot.contact_count <= RISC_TOUCH_MAX_CONTACTS) {
    publish_snapshot(snapshot);
  } else if (!poll_ok) {
    // Never preserve a stale held contact after a provider failure.
    clear_snapshot();
  }
}

void paperboy_touch_owner_end() {
  g_owner_active = false;
  clear_snapshot();
  g_touch_api = nullptr;
  if (g_provider && g_touch_lease) (void)g_provider->release(g_touch_lease);
  g_touch_lease = 0;
  g_provider = nullptr;
}

bool touch_init(void) {
  // The owner can acquire the optional provider after the console starts.
  // Keep the original caller enabled so late provider availability is usable.
  return true;
}

bool touch_read(touch_state_t *out_state) {
  if (!out_state) return false;
  memset(out_state, 0, sizeof(*out_state));

  risc_touch_snapshot_v1 snapshot{};
  bool valid_snapshot = false;
  int rotation = 0;
  portENTER_CRITICAL(&g_touch_lock);
  snapshot = g_snapshot;
  valid_snapshot = g_snapshot_valid;
  rotation = g_rotation;
  portEXIT_CRITICAL(&g_touch_lock);

  if (!valid_snapshot) return true;
  out_state->home_pressed =
      (snapshot.buttons & RISC_TOUCH_BUTTON_PRIMARY) != 0u;

  for (uint8_t i = 0;
       i < snapshot.contact_count && out_state->points < 5u; ++i) {
    uint16_t x = 0, y = 0;
    bool point_valid = false;
    transform_point(snapshot.contacts[i].x, snapshot.contacts[i].y,
                    snapshot.width, snapshot.height, rotation,
                    &x, &y, &point_valid);
    if (!point_valid) continue;
    const uint8_t at = out_state->points++;
    out_state->id[at] = snapshot.contacts[i].id;
    out_state->x[at] = x;
    out_state->y[at] = y;
  }
  out_state->touched = out_state->points > 0u;
  return true;
}

void touch_set_rotation(int rotation) {
  portENTER_CRITICAL(&g_touch_lock);
  g_rotation = rotation & 3;
  portEXIT_CRITICAL(&g_touch_lock);
}

void touch_debug_dump_once_per_second(void) {
#if TOUCH_CALIBRATION_MODE
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (uint32_t(now - last) < 1000u) return;
  last = now;
  risc_touch_snapshot_v1 snapshot{};
  portENTER_CRITICAL(&g_touch_lock);
  snapshot = g_snapshot;
  portEXIT_CRITICAL(&g_touch_lock);
  if (snapshot.contact_count)
    ESP_LOGI(kTag, "provider touch count=%u first=%u,%u",
             unsigned(snapshot.contact_count),
             unsigned(snapshot.contacts[0].x), unsigned(snapshot.contacts[0].y));
#endif
}
