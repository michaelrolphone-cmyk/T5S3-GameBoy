#include "touch_gt911.h"
#include "elf_lifecycle.h"

#include <Arduino.h>
#include <RiscTouchV1.h>
#include <T5ProviderCapabilityApi.h>
#include <esp_log.h>
#include <string.h>

namespace {
constexpr const char *kTag = "touch_provider";

const t5_provider_capability_api_v1 *s_capability_host = nullptr;
const risc_touch_api_v1 *s_touch = nullptr;
t5_provider_capability_lease_t s_lease = T5_PROVIDER_CAPABILITY_LEASE_INVALID;
uint64_t s_subscription = 0;
portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
touch_state_t s_state = {};
bool s_available = false;
uint32_t s_failures = 0;
uint32_t s_last_debug_ms = 0;

void publish_empty() {
  portENTER_CRITICAL(&s_state_mux);
  memset(&s_state, 0, sizeof(s_state));
  portEXIT_CRITICAL(&s_state_mux);
}

bool publish_snapshot() {
  if (!s_touch) return false;
  risc_touch_snapshot_v1 snapshot{};
  if (!s_touch->snapshot(s_touch->context, &snapshot) ||
      snapshot.contact_count > RISC_TOUCH_MAX_CONTACTS) {
    publish_empty();
    return false;
  }

  touch_state_t next{};
  next.points = snapshot.contact_count;
  next.touched = next.points != 0;
  next.home_pressed = (snapshot.buttons & RISC_TOUCH_BUTTON_PRIMARY) != 0;
  for (uint8_t i = 0; i < next.points; ++i) {
    next.id[i] = snapshot.contacts[i].id;
    next.x[i] = snapshot.contacts[i].x;
    next.y[i] = snapshot.contacts[i].y;
  }

  portENTER_CRITICAL(&s_state_mux);
  s_state = next;
  portEXIT_CRITICAL(&s_state_mux);
  return true;
}

void release_provider() {
  if (s_touch && s_subscription) {
    (void)s_touch->unsubscribe(s_touch->context, s_subscription);
  }
  s_subscription = 0;
  s_touch = nullptr;
  if (s_capability_host && s_lease != T5_PROVIDER_CAPABILITY_LEASE_INVALID) {
    (void)s_capability_host->release(s_lease);
  }
  s_lease = T5_PROVIDER_CAPABILITY_LEASE_INVALID;
  s_capability_host = nullptr;
  portENTER_CRITICAL(&s_state_mux);
  s_available = false;
  memset(&s_state, 0, sizeof(s_state));
  portEXIT_CRITICAL(&s_state_mux);
}
}  // namespace

void paperboy_touch_owner_begin() {
  release_provider();
  s_capability_host = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
  if (!s_capability_host ||
      s_capability_host->struct_size < sizeof(t5_provider_capability_api_v1) ||
      !s_capability_host->acquire || !s_capability_host->release) {
    ESP_LOGE(kTag, "RiscRTE provider capability API unavailable");
    release_provider();
    return;
  }

  const void *interface_ptr = nullptr;
  if (!s_capability_host->acquire("input.touch.raw", RISC_TOUCH_API_V1,
                                  &s_lease, &interface_ptr) ||
      !interface_ptr || s_lease == T5_PROVIDER_CAPABILITY_LEASE_INVALID) {
    char detail[128] = {};
    if (s_capability_host->last_error)
      (void)s_capability_host->last_error(detail, sizeof(detail));
    ESP_LOGE(kTag, "input.touch.raw acquire failed: %s",
             detail[0] ? detail : "unknown");
    release_provider();
    return;
  }

  s_touch = static_cast<const risc_touch_api_v1 *>(interface_ptr);
  if (s_touch->api_version != RISC_TOUCH_API_V1 ||
      s_touch->struct_size < sizeof(risc_touch_api_v1) ||
      !s_touch->subscribe || !s_touch->unsubscribe ||
      !s_touch->poll || !s_touch->next || !s_touch->snapshot) {
    ESP_LOGE(kTag, "input.touch.raw API invalid");
    release_provider();
    return;
  }

  s_subscription = s_touch->subscribe(s_touch->context);
  if (!s_subscription || !publish_snapshot()) {
    ESP_LOGE(kTag, "input.touch.raw subscription/snapshot failed");
    release_provider();
    return;
  }

  s_failures = 0;
  portENTER_CRITICAL(&s_state_mux);
  s_available = true;
  portEXIT_CRITICAL(&s_state_mux);
  ESP_LOGI(kTag, "GameBoy using RiscRTE input.touch.raw provider");
}

void paperboy_touch_owner_poll() {
  if (!s_available || !s_touch) return;

  bool healthy = s_touch->poll(s_touch->context, 16u);
  bool gap = false;
  for (unsigned i = 0; i < RISC_TOUCH_QUEUE_LENGTH; ++i) {
    risc_touch_event_v1 event{};
    const int32_t rc = s_touch->next(s_touch->context, s_subscription, &event);
    if (rc == 0) break;
    if (rc < 0) {
      gap = true;
      break;
    }
  }

  // GameBoy needs authoritative current contact state, not gesture history.
  // Snapshot every service pass so a missed UP can never latch a virtual key.
  if (!publish_snapshot()) healthy = false;
  if (!healthy || gap) {
    ++s_failures;
    // A queue gap is harmless after snapshot recovery. A provider failure also
    // clears state immediately so gameplay never retains a stale held button.
    if (!healthy) publish_empty();
  }
}

void paperboy_touch_owner_end() {
  release_provider();
  ESP_LOGI(kTag, "GameBoy released RiscRTE touch provider");
}

bool touch_init(void) {
  // Provider acquisition is performed by app_main on the RiscRTE owner task
  // before the emulator worker starts.
  portENTER_CRITICAL(&s_state_mux);
  const bool available = s_available;
  portEXIT_CRITICAL(&s_state_mux);
  return available;
}

bool touch_read(touch_state_t *out_state) {
  if (!out_state) return false;
  portENTER_CRITICAL(&s_state_mux);
  const bool available = s_available;
  if (available) *out_state = s_state;
  portEXIT_CRITICAL(&s_state_mux);
  return available;
}

void touch_set_rotation(int rotation) {
  // input.touch.raw reports the panel's native portrait coordinate space.
  // GameBoy's UI already performs its own portrait/landscape hit mapping.
  (void)rotation;
}

void touch_debug_dump_once_per_second(void) {
  const uint32_t now = millis();
  if (!s_available || static_cast<uint32_t>(now - s_last_debug_ms) < 1000u) return;
  s_last_debug_ms = now;
  touch_state_t state{};
  (void)touch_read(&state);
  ESP_LOGI(kTag, "provider points=%u home=%u failures=%lu first=%u,%u",
           (unsigned)state.points, state.home_pressed ? 1u : 0u,
           (unsigned long)s_failures,
           state.points ? (unsigned)state.x[0] : 0u,
           state.points ? (unsigned)state.y[0] : 0u);
}
