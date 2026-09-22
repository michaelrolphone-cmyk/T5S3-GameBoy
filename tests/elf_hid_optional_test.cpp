#include "../riscrte/usb_hid_elf_adapter.cpp"
#include <cassert>
#include <initializer_list>
#include <cstdio>
uint32_t test_now;
static bool api_present;
static unsigned attempts, releases;
static bool deny(const char *, uint32_t, t5_provider_capability_lease_t *lease, const void **iface) {
  ++attempts; *lease = 0; *iface = nullptr; return false;
}
static bool release(t5_provider_capability_lease_t) { ++releases; return true; }
static const t5_provider_capability_api_v1 api = {
  T5_PROVIDER_CAPABILITY_API_VERSION, sizeof(t5_provider_capability_api_v1), deny, release
};
extern "C" const t5_provider_capability_api_v1 *t5_provider_capability_get_api(uint32_t) {
  return api_present ? &api : nullptr;
}
int main() {
  for (bool available : {false, true}) {
    api_present = available; attempts = releases = 0;
    paperboy_usb_owner_begin();
    for (unsigned i = 0; i < 10; ++i) {
      paperboy_usb_owner_poll();
      assert(usb_hid_gamepad_buttons() == 0);
      assert(usb_hid_gamepad_navigation_buttons() == 0);
      assert(usb_hid_gamepad_take_actions() == 0);
    }
    paperboy_usb_owner_end(); paperboy_usb_owner_end();
    assert(attempts == (available ? 2u : 0u));
    assert(releases == 0);
  }
  puts("ELF absent API/denied optional HID and repeated teardown: PASS");
}
