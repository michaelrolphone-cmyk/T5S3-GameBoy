#include "../riscrte/usb_hid_elf_adapter.cpp"
#include <cassert>
#include <initializer_list>
#include <cstdio>
void paperboy_storage_hid_diagnostic(const char *) {}
uint32_t test_now;
static bool api_present;
static unsigned attempts, releases, subscriptions, unsubscriptions;
static bool allow_keyboard, allow_host, attached, fail_poll;
static bool host_devices(void *, uint64_t *out, size_t *count) {
  const size_t n = attached ? 1u : 0u;
  if (*count < n) return false;
  if (n) out[0] = 55;
  *count = n;
  return true;
}
static bool host_configuration(void *, uint64_t device, uint8_t *out, size_t *length,
                               uint16_t *vid, uint16_t *pid) {
  assert(device == 55 && *length >= 18);
  const uint8_t config[] = {9, 2, 18, 0, 1, 1, 0, 0x80, 50,
                            9, 4, 0, 0, 1, 3, 0, 0, 0};
  memcpy(out, config, sizeof(config));
  *length = sizeof(config);
  *vid = 0x1234; *pid = 0x5678;
  return true;
}
static risc_usb_host_interrupt_v1 host_api = [] {
  risc_usb_host_interrupt_v1 result{};
  result.discovery.host.api_version = RISC_USB_HOST_API_V1;
  result.discovery.host.struct_size = sizeof(result);
  result.discovery.host.configuration = host_configuration;
  result.discovery.devices = host_devices;
  return result;
}();
static risc_usb_keyboard_event_v1 events[4];
static unsigned event_count, event_cursor;
static uint64_t subscribe_keyboard(void *, uint64_t filter) { assert(filter == 0); ++subscriptions; return 17; }
static bool unsubscribe_keyboard(void *, uint64_t id) { assert(id == 17); ++unsubscriptions; return true; }
static bool poll_keyboard(void *, size_t budget) { assert(budget == 4); return !fail_poll; }
static int32_t next_keyboard(void *, uint64_t id, risc_usb_keyboard_event_v1 *out) {
  assert(id == 17);
  if (event_cursor == event_count) return 0;
  *out = events[event_cursor++]; return 1;
}
static bool snapshot_keyboard(void *, risc_usb_keyboard_state_v1 *, size_t *count) { *count = 0; return true; }
static const risc_usb_keyboard_api_v1 keyboard_api = {
 RISC_USB_KEYBOARD_API_V1, sizeof(risc_usb_keyboard_api_v1), nullptr,
 subscribe_keyboard, unsubscribe_keyboard, poll_keyboard, next_keyboard, snapshot_keyboard
};
static bool deny(const char *name, uint32_t, t5_provider_capability_lease_t *lease, const void **iface) {
  ++attempts; *lease = 0; *iface = nullptr;
  if (allow_keyboard && !strcmp(name, "usb.hid.keyboard")) {
    *lease = 8; *iface = &keyboard_api; return true;
  }
  if (allow_host && !strcmp(name, "usb.host")) {
    *lease = 9; *iface = &host_api; return true;
  }
  return false;
}
static bool release(t5_provider_capability_lease_t) { ++releases; return true; }
static t5_provider_capability_api_v1 api = [] {
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
  // Actual provider path: older valid API prefix, denied launch, retry,
  // late attach, menu events, disconnect/reconnect, and only-on-exit release.
  api_present = true; attempts = releases = 0;
  api.struct_size = offsetof(t5_provider_capability_api_v1, release) + sizeof(api.release);
  test_now = 0; paperboy_usb_owner_begin(); assert(attempts == 2);
  usb_hid_gamepad_test_active(true);
  test_now = 1000; paperboy_usb_owner_poll();
  assert(attempts == 3);
  assert(!strcmp(usb_hid_gamepad_test_status().stage, "USB HOST DIAGNOSTIC UNAVAILABLE"));
  usb_hid_gamepad_test_active(false);
  allow_keyboard = true;
  test_now = 4999; paperboy_usb_owner_poll(); assert(subscriptions == 0);
  test_now = 5000; paperboy_usb_owner_poll(); assert(subscriptions == 1 && releases == 0);
  events[0] = {}; events[0].kind = 1;
  events[1] = {}; events[1].kind = 3; events[1].usage = 0x28;
  events[2] = {}; events[2].kind = 4; events[2].usage = 0x28;
  event_cursor = 0; event_count = 3;
  fail_poll = true; // Events published before a later interface fails still count.
  paperboy_usb_owner_poll();
  assert(usb_hid_gamepad_buttons() & GBEMU_INPUT_A);
  assert(usb_hid_gamepad_navigation_buttons() & GBEMU_INPUT_A);
  assert(usb_hid_gamepad_buttons() == 0);
  fail_poll = false;
  events[0].kind = 2; event_cursor = 0; event_count = 1;
  paperboy_usb_owner_poll(); assert(usb_hid_gamepad_buttons() == 0 && releases == 0);
  events[0].kind = 1; events[1].usage = 0x51; events[2].usage = 0x51;
  event_cursor = 0; event_count = 3;
  test_now = 10000; paperboy_usb_owner_poll();
  assert(subscriptions == 1 && releases == 0); // Missing gamepad doesn't reset keyboard.
  assert(usb_hid_gamepad_buttons() & GBEMU_INPUT_DOWN);
  assert(usb_hid_gamepad_buttons() == 0);
  paperboy_usb_owner_end(); assert(releases == 1 && unsubscriptions == 1);
  test_now = 20000; paperboy_usb_owner_poll(); assert(subscriptions == 1);
  puts("Provider ABI prefix, delayed acquisition, hotplug/reconnect and menu event delivery: PASS");
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
  allow_host = true;
  test_now = 30000;
  paperboy_usb_owner_begin();
  usb_hid_gamepad_test_active(true);
  paperboy_usb_owner_poll();
  assert(usb_hid_gamepad_test_status().usb_devices == 0);
  assert(!strcmp(usb_hid_gamepad_test_status().stage, "NO USB DEVICE ENUMERATED"));
  attached = true; test_now += 1000;
  paperboy_usb_owner_poll();
  const auto detected = usb_hid_gamepad_test_status();
  assert(detected.usb_devices == 1 && detected.hid_interfaces == 1);
  assert(detected.vid == 0x1234 && detected.pid == 0x5678);
  assert(!strcmp(detected.stage, "USB HID SEEN; WAITING FOR GAMEPAD"));
  paperboy_usb_owner_end();
  puts("On-screen USB discovery, VID/PID, HID interface and stage: PASS");
  puts("Gamepad bursts, analog coalescing, disconnect and overflow recovery: PASS");
  puts("ELF absent API/denied optional HID and repeated teardown: PASS");
}
