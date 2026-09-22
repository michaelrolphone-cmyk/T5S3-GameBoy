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
static const t5_provider_capability_api_v1 api = [] {
  t5_provider_capability_api_v1 result{};
  result.api_version = T5_PROVIDER_CAPABILITY_API_VERSION;
  result.struct_size = sizeof(result);
  result.acquire = deny;
  result.release = release;
  return result;
}();
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
  // A complete quick tap received during one owner poll survives until the
  // emulator samples input; repeated analog noise does not create a backlog.
  risc_usb_gamepad_state_v1 state{};
  state.connected = 1; state.hat = 8; state.buttons = 2;
  map_gamepad(state);
  for (int i = 0; i < 100; ++i) { state.x = i; map_gamepad(state); }
  state.buttons = 0; map_gamepad(state);
  assert(g_gamepad_count == 2);
  assert(usb_hid_gamepad_buttons() & GBEMU_INPUT_A);
  assert(!(usb_hid_gamepad_buttons() & GBEMU_INPUT_A));
  // Separate taps remain separate, rather than collapsing into one held key.
  for (int i = 0; i < 2; ++i) {
    state.buttons = 2; map_gamepad(state);
    state.buttons = 0; map_gamepad(state);
  }
  for (int i = 0; i < 2; ++i) {
    assert(usb_hid_gamepad_buttons() & GBEMU_INPUT_A);
    assert(usb_hid_gamepad_buttons() == 0);
  }
  state.buttons = 2; map_gamepad(state);
  map_gamepad(risc_usb_gamepad_state_v1{});
  assert(usb_hid_gamepad_buttons() == 0 && g_gamepad_count == 0);
  for (size_t i = 0; i <= kGamepadQueueCapacity; ++i) {
    state.buttons = (i % 2 == 0) ? 2 : 0; map_gamepad(state);
  }
  assert(g_gamepad_overflows == 1 && g_gamepad_count == 0);
  assert(usb_hid_gamepad_buttons() & GBEMU_INPUT_A);
  state.buttons = 0; map_gamepad(state, true);
  assert(usb_hid_gamepad_buttons() == 0);
  paperboy_usb_owner_end();
  // Arrow/Enter taps drained in one owner poll must reach menu navigation.
  for (auto usage : {0x51, 0x52, 0x28, 0x29}) {
    const uint8_t expected = usage == 0x51 ? GBEMU_INPUT_DOWN :
        usage == 0x52 ? GBEMU_INPUT_UP : usage == 0x28 ? GBEMU_INPUT_A : GBEMU_INPUT_B;
    for (unsigned tap = 0; tap < 2; ++tap) {
      UsbHidKeyboardKeys keys{}; set_key(keys, usage, true);
      accept_keyboard(keys, true); accept_keyboard(UsbHidKeyboardKeys{}, true);
    }
    for (unsigned tap = 0; tap < 2; ++tap) {
      assert(usb_hid_gamepad_buttons() & expected);
      assert(usb_hid_gamepad_navigation_buttons() & expected);
      assert(usb_hid_gamepad_buttons() == 0);
    }
  }
  UsbHidKeyboardKeys held{}; set_key(held, 0x51, true);
  accept_keyboard(held, true); clear_keyboard();
  assert(usb_hid_gamepad_buttons() == 0);
  for (size_t i = 0; i <= kKeyboardQueueCapacity; ++i)
    accept_keyboard(i % 2 ? UsbHidKeyboardKeys{} : held, true);
  assert(g_keyboard_count == 0);
  assert(usb_hid_gamepad_buttons() & GBEMU_INPUT_DOWN);
  clear_keyboard();
  puts("Keyboard quick menu taps, disconnect and overflow: PASS");
  puts("Gamepad bursts, analog coalescing, disconnect and overflow recovery: PASS");
  puts("ELF absent API/denied optional HID and repeated teardown: PASS");
}
