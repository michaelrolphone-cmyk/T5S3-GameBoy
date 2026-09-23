#include "../riscrte/usb_hid_elf_adapter.cpp"
#include <cassert>
#include <initializer_list>
#include <cstdio>
#include <algorithm>
void paperboy_storage_hid_diagnostic(const char *) {}
uint32_t test_now;
static bool api_present;
static bool driver_diagnostic(void *, char *out, size_t capacity) {
  snprintf(out, capacity, "HID REPORT DESCRIPTOR READ FAILED");
  return true;
}
static const char *enumeration_reason = "ATTACHED; ENUM FAIL: Bad transfer status -1: GET_SHORT_DEV_DESC";
static bool host_diagnostic(void *, char *out, size_t capacity) {
  snprintf(out, capacity, "%s", enumeration_reason);
  return true;
}
static unsigned attempts, releases, subscriptions, unsubscriptions;
static bool allow_keyboard, allow_host, attached, fail_poll;
static bool allow_hid, allow_xinput, xbox_device, xinput_poll_fail, xbox_clone;
static unsigned xinput_subscriptions, xinput_unsubscriptions, xinput_cursor, xinput_count;
static risc_usb_gamepad_event_v1 xinput_events[4];
static risc_usb_gamepad_state_v1 xinput_state;
static uint64_t subscribe_xinput(void *, uint64_t filter) {
  assert(filter == 0); ++xinput_subscriptions; return 27;
}
static bool unsubscribe_xinput(void *, uint64_t id) {
  assert(id == 27); ++xinput_unsubscriptions; return true;
}
static bool poll_xinput(void *, size_t budget) {
  assert(budget == 16);
  while (xinput_cursor != xinput_count) xinput_state = xinput_events[xinput_cursor++].state;
  return !xinput_poll_fail;
}
static int32_t next_xinput(void *, uint64_t, risc_usb_gamepad_event_v1 *) {
  assert(!"Gameboy must sample the snapshot, never consume gamepad history");
  return -1;
}
static bool snapshot_xinput(void *, risc_usb_gamepad_state_v1 *out, size_t *count) {
  assert(*count >= 1); *out = xinput_state; *count = 1; return true;
}
static bool xinput_diagnostic(void *, char *out, size_t capacity) {
  snprintf(out, capacity, "%s", xinput_state.connected ? "XINPUT GAMEPAD CONNECTED" : "XINPUT WAITING FOR REPORT");
  return true;
}
static const risc_usb_gamepad_diagnostics_v1 xinput_api = {
  {RISC_USB_GAMEPAD_API_V1, sizeof(risc_usb_gamepad_diagnostics_v1), nullptr,
   subscribe_xinput, unsubscribe_xinput, poll_xinput, next_xinput, snapshot_xinput},
  xinput_diagnostic
};
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
  if (xbox_device) { *vid = 0x045e; *pid = 0x028e; out[14] = 0xff; out[15] = 0x5d; out[16] = 1; }
  if (xbox_clone) { *vid = 0x1234; *pid = 0x9876; out[16] = 0x81; }
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
  if (allow_hid && !strcmp(name, "usb.hid.gamepad")) {
    *lease = 11; *iface = &xinput_api; return true; // Shared fake snapshot transport.
  }
  if (allow_xinput && !strcmp(name, "usb.xinput.gamepad")) {
    *lease = 10; *iface = &xinput_api; return true;
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
static void reported_receiver_test() {
  // Raw masks read by the user from the actual receiver in Gamepad Test.
  // Exercise acquisition -> provider snapshot -> console -> action, not a
  // pre-decoded pad or the old assumed Start/Select numbers.
  api_present = allow_hid = true;
  xinput_state = {}; xinput_state.device = 77; xinput_state.connected = 1;
  xinput_state.hat = 8;
  paperboy_usb_owner_begin();
  auto sample = [](uint32_t mask, uint8_t expected_buttons, uint8_t expected_actions,
                   uint8_t hat = 8) {
    xinput_state.buttons = mask; xinput_state.hat = hat;
    paperboy_usb_owner_poll();
    assert(usb_hid_gamepad_buttons() == expected_buttons);
    assert(usb_hid_gamepad_take_actions() == expected_actions);
    assert(usb_hid_gamepad_test_status().buttons == mask); // Raw display stays raw.
    assert(usb_hid_gamepad_test_status().compact_buttons);
  };
  sample(0x80, GBEMU_INPUT_START, 0);
  sample(0, 0, 0);
  sample(0x40, GBEMU_INPUT_SELECT, 0);
  sample(0x60, 0, SNES_ACTION_BRIGHTEN);
  sample(0x60, 0, 0); // No repeated brightness step while held.
  sample(0x40, 0, 0);
  sample(0x50, 0, SNES_ACTION_DIM);
  sample(0, 0, 0);
  sample(0x10, 0, 0); sample(0x20, 0, 0); sample(0, 0, 0);
  sample(0x80, GBEMU_INPUT_START, 0);
  sample(0xA0, 0, SNES_ACTION_SAVE); sample(0xA0, 0, 0);
  sample(0x80, 0, 0);
  sample(0x90, 0, SNES_ACTION_LOAD); sample(0, 0, 0);
  sample(0xC0, GBEMU_INPUT_START | GBEMU_INPUT_SELECT, 0);
  sample(0xD0, GBEMU_INPUT_START | GBEMU_INPUT_SELECT, 0);
  sample(0xF0, 0, SNES_ACTION_SETTINGS); sample(0xF0, 0, 0);
  sample(0xB0, 0, 0); sample(0x90, 0, 0); sample(0x10, 0, 0); sample(0, 0, 0);
  sample(0x80, GBEMU_INPUT_START, 0); sample(0, 0, 0);
  sample(0x01, GBEMU_INPUT_A, 0);
  assert(usb_hid_gamepad_navigation_buttons() == GBEMU_INPUT_A);
  sample(0, 0, 0);
  sample(0x02, GBEMU_INPUT_B, 0);
  assert(usb_hid_gamepad_navigation_buttons() == GBEMU_INPUT_B);
  sample(0, 0, 0);
  sample(0x04, GBEMU_INPUT_A, 0); // X = turbo A, without menu navigation.
  assert(usb_hid_gamepad_navigation_buttons() == 0);
  sample(0, 0, 0);
  sample(0x08, GBEMU_INPUT_B, 0); // Y = turbo B, without menu navigation.
  assert(usb_hid_gamepad_navigation_buttons() == 0);
  sample(0, 0, 0);
  sample(0x30, 0, SNES_ACTION_ROTATE, 2); sample(0, 0, 0);
  paperboy_usb_owner_end();
  allow_hid = false; xinput_state = {};
  puts("Corrected receiver A/B, X/Y and 80/40 Start/Select: snapshots and all shortcuts: PASS");
}
int main() {
  reported_receiver_test();
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
    assert(attempts == (available ? 4u : 0u)); // Keyboard, HID, XInput and automatic host diagnostics.
    assert(releases == 0);
  }
  // Actual provider path: older valid API prefix, denied launch, retry,
  // late attach, menu events, disconnect/reconnect, and only-on-exit release.
  api_present = true; attempts = releases = 0;
  api.struct_size = offsetof(t5_provider_capability_api_v1, release) + sizeof(api.release);
  test_now = 0; paperboy_usb_owner_begin(); assert(attempts == 3);
  usb_hid_gamepad_test_active(true);
  test_now = 1000; paperboy_usb_owner_poll();
  assert(attempts == 4);
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
  // Full gamepad states must be sampled like I2C and standalone USB: newest
  // state wins. A press whose release has arrived cannot replay next frame.
  risc_usb_gamepad_state_v1 state{};
  state.connected = 1; state.hat = 8;
  for (unsigned i = 0; i < 512; ++i) {
    state.buttons = 1; map_gamepad(state);
    state.buttons = 0; map_gamepad(state);
  }
  assert(usb_hid_gamepad_buttons() == 0);
  assert(usb_hid_gamepad_navigation_buttons() == 0);
  assert(usb_hid_gamepad_take_actions() == 0);
  state.buttons = 1; state.hat = 2; map_gamepad(state);
  assert(usb_hid_gamepad_buttons() == (GBEMU_INPUT_A | GBEMU_INPUT_RIGHT));
  test_now += 10000; // A held snapshot does not expire just because time passes.
  assert(usb_hid_gamepad_buttons() == (GBEMU_INPUT_A | GBEMU_INPUT_RIGHT));
  state.buttons = 0; state.hat = 8; map_gamepad(state);
  assert(usb_hid_gamepad_buttons() == 0); // Release is visible immediately.
  // Changes while a frame was busy cannot manufacture delayed shoulder actions.
  state.buttons = 16; map_gamepad(state);
  state.buttons = 0; map_gamepad(state);
  assert(usb_hid_gamepad_buttons() == 0 && usb_hid_gamepad_take_actions() == 0);
  state.buttons = 16; map_gamepad(state);
  assert(usb_hid_gamepad_buttons() == 0 && usb_hid_gamepad_take_actions() == 0);
  state.buttons = 16 | 128; map_gamepad(state);
  assert(usb_hid_gamepad_buttons() == 0 && usb_hid_gamepad_take_actions() == SNES_ACTION_LOAD);
  assert(usb_hid_gamepad_buttons() == 0 && usb_hid_gamepad_take_actions() == 0);
  state.buttons = 1; map_gamepad(state);
  map_gamepad(risc_usb_gamepad_state_v1{});
  assert(usb_hid_gamepad_buttons() == 0);
  paperboy_usb_owner_end();
  // Exercise actual mapped HID and XInput snapshots, with each modifier
  // arriving before OR after its bumper, repeated taps, and staggered release.
  for (bool xinput : {false, true}) {
    const unsigned select_mask = xinput ? 0x100u : 0x40u;
    const unsigned start_mask = xinput ? 0x200u : 0x80u;
    for (unsigned modifier : {select_mask, start_mask}) {
      for (unsigned shoulder : {16u, 32u}) {
        for (bool modifier_first : {false, true}) {
          paperboy_usb_owner_end();
          state = {}; state.connected = 1; state.hat = 8;
          auto sample = [&](unsigned mask) {
            state.buttons = mask; map_gamepad(state, false, xinput);
            return usb_hid_gamepad_buttons();
          };
          const uint8_t action = modifier == select_mask
              ? (shoulder == 16 ? SNES_ACTION_DIM : SNES_ACTION_BRIGHTEN)
              : (shoulder == 16 ? SNES_ACTION_LOAD : SNES_ACTION_SAVE);
          sample(modifier_first ? modifier : shoulder);
          assert(usb_hid_gamepad_take_actions() == 0);
          assert(sample(modifier | shoulder) == 0);
          assert(usb_hid_gamepad_take_actions() == action);
          test_now += 1000;
          assert(sample(modifier | shoulder) == 0);
          assert(usb_hid_gamepad_take_actions() == 0); // Holding never repeats.
          assert(sample(modifier) == 0); // Consumed modifier cannot pause/select.
          assert(usb_hid_gamepad_take_actions() == 0);
          assert(sample(modifier | shoulder) == 0);
          assert(usb_hid_gamepad_take_actions() == action);
          assert(sample(shoulder) == 0); // Releasing modifier first is harmless.
          assert(usb_hid_gamepad_take_actions() == 0);
          assert(sample(0) == 0);
        }
      }
    }
    // Complete settings wins over all shoulder commands and consumes every
    // release order; ordinary gameplay and shortcuts work again afterwards.
    unsigned release_order[] = {16, 32, start_mask, select_mask};
    std::sort(release_order, release_order + 4);
    do {
      paperboy_usb_owner_end();
      state = {}; state.connected = 1; state.hat = 8;
      unsigned mask = 16 | 32 | start_mask | select_mask;
      state.buttons = mask; map_gamepad(state, false, xinput);
      assert(usb_hid_gamepad_buttons() == 0);
      assert(usb_hid_gamepad_take_actions() == SNES_ACTION_SETTINGS);
      assert(usb_hid_gamepad_buttons() == 0 && usb_hid_gamepad_take_actions() == 0);
      for (unsigned key : release_order) {
        mask &= ~key; state.buttons = mask; map_gamepad(state, false, xinput);
        assert(usb_hid_gamepad_buttons() == 0);
        assert(usb_hid_gamepad_navigation_buttons() == 0);
        assert(usb_hid_gamepad_take_actions() == 0);
      }
      state.buttons = start_mask; map_gamepad(state, false, xinput);
      assert(usb_hid_gamepad_buttons() == GBEMU_INPUT_START);
      state.buttons = select_mask; map_gamepad(state, false, xinput);
      assert(usb_hid_gamepad_buttons() == GBEMU_INPUT_SELECT);
    } while (std::next_permutation(release_order, release_order + 4));
    paperboy_usb_owner_end();
    // Holding Start+Select reserves shoulders while assembling Settings.
    const unsigned modifiers = start_mask | select_mask;
    for (unsigned mask : {modifiers, modifiers | 16u, modifiers | 48u}) {
      state.buttons = mask; map_gamepad(state, false, xinput);
      usb_hid_gamepad_buttons();
      assert(usb_hid_gamepad_take_actions() == (mask == (modifiers | 48u) ? SNES_ACTION_SETTINGS : 0));
    }
    paperboy_usb_owner_end();
  }
  // XInput publishes analog triggers in the compact receiver's modifier bits.
  // Those triggers, alone or with bumpers, must never open Settings or save.
  state = {}; state.connected = 1; state.hat = 8;
  state.buttons = 0xF0; map_gamepad(state, false, true);
  assert(usb_hid_gamepad_buttons() == 0 && usb_hid_gamepad_take_actions() == 0);
  assert(!usb_hid_gamepad_test_status().compact_buttons);
  map_gamepad(risc_usb_gamepad_state_v1{}); usb_hid_gamepad_buttons();
  paperboy_usb_owner_end();
  puts("HID/XInput modifier shortcuts, settings priority and 24 release orders: PASS");
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
  test_now += 1000; paperboy_usb_owner_poll();
  auto unchanged = usb_hid_gamepad_test_status();
  assert(!memcmp(detected.events, unchanged.events, sizeof(detected.events)));
  // Older drivers expose only the original prefix; never read a missing tail.
  risc_usb_gamepad_api_v1 old_gamepad{};
  old_gamepad.struct_size = sizeof(old_gamepad);
  g_gamepads[0].api = &old_gamepad;
  test_now += 1000; paperboy_usb_owner_poll();
  assert(!strcmp(usb_hid_gamepad_test_status().stage, "USB HID SEEN; WAITING FOR GAMEPAD"));
  risc_usb_gamepad_diagnostics_v1 diagnostic_gamepad{};
  diagnostic_gamepad.base.struct_size = sizeof(diagnostic_gamepad);
  diagnostic_gamepad.diagnostic = driver_diagnostic;
  g_gamepads[0].api = &diagnostic_gamepad.base;
  test_now += 1000; paperboy_usb_owner_poll();
  const auto failure = usb_hid_gamepad_test_status();
  assert(!strcmp(failure.stage, "HID REPORT DESCRIPTOR READ FAILED"));
  assert(!strcmp(failure.error, failure.stage));
  assert(!strcmp(failure.events[2], failure.stage));
  test_now += 1000; paperboy_usb_owner_poll();
  unchanged = usb_hid_gamepad_test_status();
  assert(!memcmp(failure.events, unchanged.events, sizeof(failure.events)));
  // A bus snapshot with zero devices can still report physical attachment
  // and the exact enumeration error through the optional host suffix.
  attached = false;
  g_gamepads[0].api = nullptr;
  risc_usb_host_diagnostics_v1 diagnostic_host{};
  diagnostic_host.base = host_api;
  diagnostic_host.base.discovery.host.struct_size = sizeof(diagnostic_host);
  diagnostic_host.diagnostic = host_diagnostic;
  g_host_api = &diagnostic_host.base;
  test_now += 1000; paperboy_usb_owner_poll();
  const auto enumeration_failure = usb_hid_gamepad_test_status();
  assert(enumeration_failure.usb_devices == 0 && enumeration_failure.hid_interfaces == 0);
  assert(!strcmp(enumeration_failure.error, enumeration_reason));
  assert(!strncmp(enumeration_failure.stage, enumeration_reason, sizeof(enumeration_failure.stage) - 1));
  test_now += 1000; paperboy_usb_owner_poll();
  unchanged = usb_hid_gamepad_test_status();
  assert(!memcmp(enumeration_failure.events, unchanged.events, sizeof(unchanged.events)));
  enumeration_reason = "NO ATTACH; NO ENUM EVENT";
  test_now += 1000; paperboy_usb_owner_poll();
  assert(!strcmp(usb_hid_gamepad_test_status().stage, enumeration_reason));
  // Reproduce the reported reconnect: reset fails while detached, then
  // the HID receiver enumerates. The old bus error must not survive success.
  enumeration_reason = "NO ATTACH; ENUM FAIL: Root port reset failed";
  probe_usb_discovery();
  assert(!strcmp(usb_hid_gamepad_test_status().error, enumeration_reason));
  attached = true;
  probe_usb_discovery();
  assert(usb_hid_gamepad_test_status().usb_devices == 1);
  assert(usb_hid_gamepad_test_status().hid_interfaces == 1);
  assert(!usb_hid_gamepad_test_status().error[0]);
  // Reading a configuration must not erase a still-relevant class failure.
  g_gamepads[0].api = &diagnostic_gamepad.base;
  probe_usb_discovery();
  assert(!strcmp(usb_hid_gamepad_test_status().error, "HID REPORT DESCRIPTOR READ FAILED"));
  probe_usb_discovery();
  assert(!strcmp(usb_hid_gamepad_test_status().error, "HID REPORT DESCRIPTOR READ FAILED"));
  g_gamepads[0].api = nullptr;
  attached = false;
  diagnostic_host.diagnostic = nullptr;
  test_now += 1000; paperboy_usb_owner_poll();
  assert(!strcmp(usb_hid_gamepad_test_status().stage, "NO USB DEVICE ENUMERATED"));
  paperboy_usb_owner_end();
  // The real optional-capability path must work with XInput alone. Failed HID
  // acquisition cannot mask readiness, input, or the vendor-interface status.
  allow_keyboard = false; allow_xinput = xbox_device = attached = true;
  releases = 0; test_now += 5000;
  paperboy_usb_owner_begin();
  assert(xinput_subscriptions == 0 && usb_hid_gamepad_test_status().provider_ready);
  usb_hid_gamepad_test_active(true);
  xinput_events[0] = {}; xinput_events[0].kind = 1;
  xinput_events[0].state.device = 55; xinput_events[0].state.connected = 1;
  xinput_events[0].state.hat = 8;
  xinput_events[1] = xinput_events[0]; xinput_events[1].kind = 3; xinput_events[1].state.buttons = 2;
  xinput_events[2] = xinput_events[1]; xinput_events[2].state.buttons = 0;
  xinput_cursor = 0; xinput_count = 3;
  paperboy_usb_owner_poll();
  const auto xbox = usb_hid_gamepad_test_status();
  assert(xbox.vid == 0x045e && xbox.pid == 0x028e && xbox.hid_interfaces == 0);
  assert(!strcmp(xbox.stage, "XINPUT GAMEPAD CONNECTED"));
  assert(usb_hid_gamepad_buttons() == 0); // Provider already delivered the release.
  assert(usb_hid_gamepad_navigation_buttons() == 0);
  xbox_clone = true; test_now += 1000; paperboy_usb_owner_poll();
  const auto clone = usb_hid_gamepad_test_status();
  assert(clone.vid == 0x1234 && clone.pid == 0x9876 && clone.hid_interfaces == 0);
  assert(!strcmp(clone.stage, "XINPUT GAMEPAD CONNECTED"));
  xbox_clone = false;
  xinput_events[0] = xinput_events[1]; xinput_events[0].state.buttons = 1;
  xinput_cursor = 0; xinput_count = 1; paperboy_usb_owner_poll();
  // A different HID device connecting/disconnecting cannot erase held Xbox input.
  risc_usb_gamepad_state_v1 other{}; other.connected = 1; other.device = 99; other.hat = 8;
  accept_gamepad(g_gamepads[0], other, true);
  other.connected = 0; accept_gamepad(g_gamepads[0], other, true);
  assert(usb_hid_gamepad_buttons() & GBEMU_INPUT_B);
  xinput_poll_fail = true;
  xinput_events[0].kind = 2; xinput_events[0].state.connected = 0;
  xinput_events[0].state.buttons = 0; xinput_cursor = 0;
  paperboy_usb_owner_poll();
  assert(usb_hid_gamepad_buttons() == 0 && !usb_hid_gamepad_test_status().connected);
  xinput_poll_fail = false;
  xinput_events[0].kind = 1; xinput_events[0].state.connected = 1;
  xinput_events[1] = xinput_events[0]; xinput_events[1].kind = 3; xinput_events[1].state.hat = 2;
  xinput_events[1].state.y = -20000; // D-pad right plus stick up, as in standalone XInput.
  xinput_cursor = 0; xinput_count = 2; paperboy_usb_owner_poll();
  const uint8_t diagonal = GBEMU_INPUT_RIGHT | GBEMU_INPUT_UP;
  assert((usb_hid_gamepad_buttons() & diagonal) == diagonal);
  // Generic HID retains its existing D-pad priority over analog movement.
  map_gamepad(xinput_events[1].state, true);
  assert(usb_hid_gamepad_buttons() == GBEMU_INPUT_RIGHT);
  paperboy_usb_owner_end(); paperboy_usb_owner_end();
  assert(xinput_subscriptions == 0 && xinput_unsubscriptions == 0 && releases == 2);
  puts("XInput-only grants, current snapshots, HID coexistence, error disconnect and reconnect: PASS");
  puts("On-screen USB discovery, VID/PID, HID interface and stage: PASS");
  puts("Gamepad latest state, immediate releases, long holds and no historical replay: PASS");
  puts("ELF absent API/denied optional HID and repeated teardown: PASS");
}
