#include "touch_gt911.h"
#include "touch_provider_adapter.h"

#include <RiscTouchV1.h>
#include <T5ProviderCapabilityApi.h>
#include <assert.h>
#include <string.h>

uint32_t test_now = 1000u;

namespace {
risc_touch_snapshot_v1 fake_snapshot{};
bool acquired = false;
bool released = false;

bool touch_poll(void*, size_t) { return true; }
bool touch_snapshot(void*, risc_touch_snapshot_v1* out) {
  if (!out) return false;
  *out = fake_snapshot;
  return true;
}

const risc_touch_api_v1 touch_api = {
    RISC_TOUCH_API_V1, sizeof(risc_touch_api_v1), nullptr,
    nullptr, nullptr, touch_poll, nullptr, touch_snapshot};

bool acquire_capability(const char* capability, uint32_t version,
                        t5_provider_capability_lease_t* lease,
                        const void** interface_out) {
  if (!capability || strcmp(capability, "input.touch.raw") != 0 ||
      version != RISC_TOUCH_API_V1 || !lease || !interface_out) return false;
  acquired = true;
  *lease = 7u;
  *interface_out = &touch_api;
  return true;
}
bool release_capability(t5_provider_capability_lease_t lease) {
  if (lease != 7u) return false;
  released = true;
  return true;
}
const t5_provider_capability_api_v1 provider_api = {
    T5_PROVIDER_CAPABILITY_API_VERSION,
    sizeof(t5_provider_capability_api_v1),
    acquire_capability, release_capability, nullptr};
}  // namespace

extern "C" const t5_provider_capability_api_v1*
t5_provider_capability_get_api(uint32_t version) {
  return version == T5_PROVIDER_CAPABILITY_API_VERSION ? &provider_api : nullptr;
}

int main() {
  fake_snapshot.width = 540;
  fake_snapshot.height = 960;

  paperboy_touch_owner_begin();
  assert(acquired);
  assert(touch_init());

  fake_snapshot.sequence = 1;
  fake_snapshot.timestamp_ms = 1010;
  fake_snapshot.contact_count = 1;
  fake_snapshot.contacts[0].id = 3;
  fake_snapshot.contacts[0].x = 120;
  fake_snapshot.contacts[0].y = 220;
  paperboy_touch_owner_poll();

  touch_state_t state{};
  assert(touch_read(&state));
  assert(state.touched && state.points == 1);
  assert(state.id[0] == 3 && state.x[0] == 120 && state.y[0] == 220);

  // The authoritative zero-contact snapshot is a release. The adapter must
  // not replay/carry forward the previous contact when no new action occurs.
  fake_snapshot.sequence = 2;
  fake_snapshot.timestamp_ms = 1020;
  fake_snapshot.contact_count = 0;
  paperboy_touch_owner_poll();
  memset(&state, 0xff, sizeof(state));
  assert(touch_read(&state));
  assert(!state.touched && state.points == 0);

  fake_snapshot.sequence = 3;
  fake_snapshot.buttons = RISC_TOUCH_BUTTON_PRIMARY;
  paperboy_touch_owner_poll();
  assert(touch_read(&state) && state.home_pressed);

  paperboy_touch_owner_end();
  assert(released);
  memset(&state, 0xff, sizeof(state));
  assert(touch_read(&state));
  assert(!state.touched && !state.home_pressed && state.points == 0);
  return 0;
}
