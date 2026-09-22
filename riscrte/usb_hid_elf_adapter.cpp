// ELF-only input backend. RiscRTE owns USB, its PHY and VBUS; never import
// usb_host_* or initialize a second host from an unloadable app.
#include "usb_hid_gamepad.h"

#include <Arduino.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <string.h>

#include <T5ProviderCapabilityApi.h>
#include <RiscUsbHidV1.h>

#include "gbemu.h"
#include "snes_mini_controller.h"
#include "usb_hid_keyboard.h"
#include "usb_hid_report.h"

namespace {
constexpr char kTag[] = "gameboy_hid";
constexpr uint32_t kTurboHalfMs = 50;
constexpr size_t kSnapshotCapacity = 4;

// Provider calls, grants and subscriptions belong exclusively to the ELF
// owner task. The console task only reads copied state under this lock.
const t5_provider_capability_api_v1 *g_provider = nullptr;
const risc_usb_keyboard_api_v1 *g_keyboard_api = nullptr;
const risc_usb_gamepad_api_v1 *g_gamepad_api = nullptr;
t5_provider_capability_lease_t g_keyboard_lease = 0;
t5_provider_capability_lease_t g_gamepad_lease = 0;
uint64_t g_keyboard_subscription = 0;
uint64_t g_gamepad_subscription = 0;
portMUX_TYPE g_input_lock = portMUX_INITIALIZER_UNLOCKED;
UsbHidKeyboardKeys g_keys;
constexpr size_t kKeyboardQueueCapacity = 32;
UsbHidKeyboardKeys g_keyboard_queue[kKeyboardQueueCapacity];
UsbHidKeyboardKeys g_sampled_keys;
size_t g_keyboard_head = 0, g_keyboard_count = 0;
UsbHidGamepadState g_gamepad;
// Keep digital transitions across owner-poll bursts until the console samples
// them. Analog changes inside the same mapped direction consume no queue slots.
constexpr size_t kGamepadQueueCapacity = 32;
UsbHidGamepadState g_gamepad_queue[kGamepadQueueCapacity];
size_t g_gamepad_head = 0, g_gamepad_count = 0;
uint32_t g_gamepad_overflows = 0;
uint16_t pressed_mask(const UsbHidGamepadState &s);
uint8_t g_pending_keyboard_actions = 0;
uint8_t g_buttons = 0;
uint8_t g_navigation = 0;
uint8_t g_actions = 0;
uint16_t g_previous = 0;
uint32_t g_turbo_a_started = 0;
uint32_t g_turbo_b_started = 0;
bool g_settings_chord = false;
bool g_rotate_chord = false;
bool g_select_consumed = false;

void set_key(UsbHidKeyboardKeys &keys, uint8_t usage, bool pressed) {
  const uint32_t bit = uint32_t(1) << (usage & 31U);
  if (pressed) keys.usages[usage >> 5U] |= bit;
  else keys.usages[usage >> 5U] &= ~bit;
}

void accept_keyboard(const UsbHidKeyboardKeys &next, bool emit_actions) {
  portENTER_CRITICAL(&g_input_lock);
  if (emit_actions) {
    const UsbHidKeyboardMapping mapped = usb_hid_map_keyboard(next, g_keys);
    g_pending_keyboard_actions |= mapped.actions;
  }
  if (!emit_actions || g_keyboard_count == kKeyboardQueueCapacity) {
    // Disconnect/GAP or overflow synchronizes to live state, never stale presses.
    g_keyboard_head = g_keyboard_count = 0;
    g_sampled_keys = next;
  } else if (memcmp(&g_keys, &next, sizeof(next)) != 0) {
    g_keyboard_queue[(g_keyboard_head + g_keyboard_count++) % kKeyboardQueueCapacity] = next;
  }
  g_keys = next;
  portEXIT_CRITICAL(&g_input_lock);
}

void clear_keyboard() { accept_keyboard(UsbHidKeyboardKeys{}, false); }

void keyboard_snapshot() {
  if (!g_keyboard_api) return;
  risc_usb_keyboard_state_v1 states[kSnapshotCapacity] = {};
  size_t count = kSnapshotCapacity;
  UsbHidKeyboardKeys keys{};
  if (g_keyboard_api->snapshot(g_keyboard_api->context, states, &count)) {
    for (size_t i = 0; i < count && i < kSnapshotCapacity; ++i) {
      if (!states[i].connected) continue;
      for (uint8_t bit = 0; bit < 8; ++bit)
        set_key(keys, uint8_t(0xe0U + bit), (states[i].modifiers & (1U << bit)) != 0);
      for (uint8_t usage : states[i].keys)
        if (usage >= 4 && usage < 0xe0) set_key(keys, usage, true);
    }
  }
  // GAP recovery is a synchronization, not a synthetic key press/action.
  accept_keyboard(keys, false);
}

void gamepad_snapshot();

void map_gamepad(const risc_usb_gamepad_state_v1 &state, bool synchronize = false) {
  UsbHidGamepadState pad{};
  if (state.connected) {
    const uint8_t hat = state.hat;
    if (hat < 8) {
      pad.up = hat == 0 || hat == 1 || hat == 7;
      pad.right = hat >= 1 && hat <= 3;
      pad.down = hat >= 3 && hat <= 5;
      pad.left = hat >= 5 && hat <= 7;
    } else {
      constexpr int16_t threshold = 16384; // Same outer-quarter dead zone as native HID.
      pad.left = state.x < -threshold;
      pad.right = state.x > threshold;
      pad.up = state.y < -threshold;
      pad.down = state.y > threshold;
    }
    pad.b = (state.buttons & (1UL << 0)) != 0;
    pad.a = (state.buttons & (1UL << 1)) != 0;
    pad.y = (state.buttons & (1UL << 2)) != 0;
    pad.x = (state.buttons & (1UL << 3)) != 0;
    pad.l = (state.buttons & (1UL << 4)) != 0;
    pad.r = (state.buttons & (1UL << 5)) != 0;
    pad.select = (state.buttons & (1UL << 8)) != 0;
    pad.start = (state.buttons & (1UL << 9)) != 0;
  }
  portENTER_CRITICAL(&g_input_lock);
  bool overflow = false;
  if (synchronize || !state.connected) {
    // A disconnect/GAP must clear stale presses, not replay them later.
    g_gamepad_head = g_gamepad_count = 0;
    g_gamepad = pad;
  } else {
    const auto &last = g_gamepad_count
        ? g_gamepad_queue[(g_gamepad_head + g_gamepad_count - 1) % kGamepadQueueCapacity]
        : g_gamepad;
    if (pressed_mask(last) != pressed_mask(pad)) {
      if (g_gamepad_count == kGamepadQueueCapacity) {
        // Bound latency and recover to actual state if the console stalls.
        g_gamepad_head = g_gamepad_count = 0;
        g_gamepad = pad;
        overflow = (++g_gamepad_overflows == 1);
      } else {
        g_gamepad_queue[(g_gamepad_head + g_gamepad_count++) % kGamepadQueueCapacity] = pad;
      }
    }
  }
  portEXIT_CRITICAL(&g_input_lock);
  if (overflow) ESP_LOGW(kTag, "gamepad input queue overflow; synchronized to latest state");
}

void gamepad_snapshot() {
  if (!g_gamepad_api) return;
  risc_usb_gamepad_state_v1 states[kSnapshotCapacity] = {};
  size_t count = kSnapshotCapacity;
  if (g_gamepad_api->snapshot(g_gamepad_api->context, states, &count)) {
    for (size_t i = 0; i < count && i < kSnapshotCapacity; ++i) {
      if (states[i].connected) { map_gamepad(states[i], true); return; }
    }
  }
  map_gamepad(risc_usb_gamepad_state_v1{});
}

uint16_t pressed_mask(const UsbHidGamepadState &s) {
  return (s.up ? 0x0001U : 0U) | (s.left ? 0x0002U : 0U) |
      (s.x ? 0x0008U : 0U) | (s.a ? 0x0010U : 0U) |
      (s.y ? 0x0020U : 0U) | (s.b ? 0x0040U : 0U) |
      (s.r ? 0x0200U : 0U) | (s.start ? 0x0400U : 0U) |
      (s.select ? 0x1000U : 0U) | (s.l ? 0x2000U : 0U) |
      (s.down ? 0x4000U : 0U) | (s.right ? 0x8000U : 0U);
}

// Preserve native GameBoy's chord semantics, turbo cadence and UI navigation.
void decode_actions(uint16_t pressed, uint32_t now) {
  g_actions = 0;
  g_navigation = 0;
  const uint16_t rising = pressed & ~g_previous;
  constexpr uint16_t left_shoulder = 0x2000U, right_shoulder = 0x0200U;
  constexpr uint16_t start = 0x0400U, select = 0x1000U, right = 0x8000U;
  constexpr uint16_t settings = left_shoulder | right_shoulder | start | select;
  constexpr uint16_t rotate = left_shoulder | right_shoulder | right;
  if ((pressed & settings) == settings && !g_settings_chord) {
    g_settings_chord = true;
    g_actions = SNES_ACTION_SETTINGS;
  }
  if (g_settings_chord) {
    g_previous = pressed;
    g_select_consumed = true;
    if (!(pressed & settings)) g_settings_chord = false;
    g_buttons = 0;
    return;
  }
  if ((pressed & rotate) == rotate && (!g_rotate_chord || (rising & right))) {
    g_rotate_chord = true;
    g_actions = SNES_ACTION_ROTATE;
  }
  if (g_rotate_chord) {
    g_previous = pressed;
    if (!(pressed & rotate)) g_rotate_chord = false;
    g_buttons = 0;
    return;
  }
  if (rising & 0x0008U) g_turbo_a_started = now;
  if (rising & 0x0020U) g_turbo_b_started = now;
  if (!(pressed & select)) g_select_consumed = false;
  if ((pressed & (start | select)) != (start | select) &&
      (pressed & select) && (pressed & (left_shoulder | right_shoulder))) {
    g_select_consumed = true;
    if ((pressed & (left_shoulder | right_shoulder)) == left_shoulder &&
        (rising & left_shoulder)) g_actions |= SNES_ACTION_DIM;
    if ((pressed & (left_shoulder | right_shoulder)) == right_shoulder &&
        (rising & right_shoulder)) g_actions |= SNES_ACTION_BRIGHTEN;
  } else if (!(pressed & select)) {
    if ((pressed & (left_shoulder | right_shoulder)) == left_shoulder &&
        (rising & left_shoulder)) g_actions |= SNES_ACTION_LOAD;
    if ((pressed & (left_shoulder | right_shoulder)) == right_shoulder &&
        (rising & right_shoulder)) g_actions |= SNES_ACTION_SAVE;
  }
  g_previous = pressed;
  if (pressed & 0x0001U) g_navigation |= GBEMU_INPUT_UP;
  if (pressed & 0x4000U) g_navigation |= GBEMU_INPUT_DOWN;
  if (pressed & 0x0002U) g_navigation |= GBEMU_INPUT_LEFT;
  if (pressed & 0x8000U) g_navigation |= GBEMU_INPUT_RIGHT;
  if (pressed & 0x0010U) g_navigation |= GBEMU_INPUT_A;
  if (pressed & 0x0040U) g_navigation |= GBEMU_INPUT_B;
  g_buttons = g_navigation;
  if ((pressed & 0x0008U) && ((now - g_turbo_a_started) / kTurboHalfMs) % 2U == 0)
    g_buttons |= GBEMU_INPUT_A;
  if ((pressed & 0x0020U) && ((now - g_turbo_b_started) / kTurboHalfMs) % 2U == 0)
    g_buttons |= GBEMU_INPUT_B;
  if (pressed & start) g_buttons |= GBEMU_INPUT_START;
  if ((pressed & select) && !g_select_consumed) g_buttons |= GBEMU_INPUT_SELECT;
}

}  // namespace

// Both entry and exit are invoked on the app's original RiscRTE owner task.
void paperboy_usb_owner_begin() {
  if (g_provider) return;
  g_provider = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
  if (!g_provider || g_provider->api_version != T5_PROVIDER_CAPABILITY_API_VERSION ||
      g_provider->struct_size < sizeof(*g_provider) ||
      !g_provider->acquire || !g_provider->release) {
    ESP_LOGW(kTag, "RiscRTE provider capability API unavailable");
    g_provider = nullptr;
    return;
  }
  const void *iface = nullptr;
  if (g_provider->acquire("usb.hid.keyboard", RISC_USB_KEYBOARD_API_V1,
                          &g_keyboard_lease, &iface)) {
    const auto *api = static_cast<const risc_usb_keyboard_api_v1 *>(iface);
    if (api && api->api_version == RISC_USB_KEYBOARD_API_V1 &&
        api->struct_size >= sizeof(*api) && api->subscribe && api->unsubscribe &&
        api->poll && api->next && api->snapshot) {
      g_keyboard_api = api;
      g_keyboard_subscription = api->subscribe(api->context, 0);
    }
    if (!g_keyboard_subscription) {
      (void)g_provider->release(g_keyboard_lease);
      g_keyboard_lease = 0;
      g_keyboard_api = nullptr;
    }
  }
  iface = nullptr;
  if (g_provider->acquire("usb.hid.gamepad", RISC_USB_GAMEPAD_API_V1,
                          &g_gamepad_lease, &iface)) {
    const auto *api = static_cast<const risc_usb_gamepad_api_v1 *>(iface);
    if (api && api->api_version == RISC_USB_GAMEPAD_API_V1 &&
        api->struct_size >= sizeof(*api) && api->subscribe && api->unsubscribe &&
        api->poll && api->next && api->snapshot) {
      g_gamepad_api = api;
      g_gamepad_subscription = api->subscribe(api->context, 0);
    }
    if (!g_gamepad_subscription) {
      (void)g_provider->release(g_gamepad_lease);
      g_gamepad_lease = 0;
      g_gamepad_api = nullptr;
    }
  }
  ESP_LOGI(kTag, "RiscRTE HID grants keyboard=%u gamepad=%u",
           static_cast<unsigned>(g_keyboard_lease != 0),
           static_cast<unsigned>(g_gamepad_lease != 0));
}

void paperboy_usb_owner_poll() {
  if (g_keyboard_api && g_keyboard_subscription &&
      g_keyboard_api->poll(g_keyboard_api->context, 4)) {
    for (unsigned n = 0; n < 32; ++n) {
      risc_usb_keyboard_event_v1 event{};
      const int32_t rc = g_keyboard_api->next(g_keyboard_api->context,
                                             g_keyboard_subscription, &event);
      if (!rc) break;
      if (rc < 0 || event.kind == 5) { keyboard_snapshot(); break; }
      if (event.kind == 2) { clear_keyboard(); continue; }
      if (event.kind != 3 && event.kind != 4) continue;
      UsbHidKeyboardKeys keys;
      portENTER_CRITICAL(&g_input_lock);
      keys = g_keys;
      portEXIT_CRITICAL(&g_input_lock);
      if (event.usage != 0) set_key(keys, event.usage, event.kind == 3);
      for (uint8_t bit = 0; bit < 8; ++bit)
        set_key(keys, uint8_t(0xe0U + bit), (event.modifiers & (1U << bit)) != 0);
      accept_keyboard(keys, true);
    }
  }
  if (g_gamepad_api && g_gamepad_subscription &&
      g_gamepad_api->poll(g_gamepad_api->context, 4)) {
    for (unsigned n = 0; n < 32; ++n) {
      risc_usb_gamepad_event_v1 event{};
      const int32_t rc = g_gamepad_api->next(g_gamepad_api->context,
                                            g_gamepad_subscription, &event);
      if (!rc) break;
      if (rc < 0 || event.kind == 5) { gamepad_snapshot(); break; }
      if (event.kind == 2) { map_gamepad(risc_usb_gamepad_state_v1{}); continue; }
      if (event.kind == 1 || event.kind == 3) map_gamepad(event.state);
    }
  }
}

void paperboy_usb_owner_end() {
  if (g_keyboard_api && g_keyboard_subscription)
    (void)g_keyboard_api->unsubscribe(g_keyboard_api->context, g_keyboard_subscription);
  if (g_gamepad_api && g_gamepad_subscription)
    (void)g_gamepad_api->unsubscribe(g_gamepad_api->context, g_gamepad_subscription);
  g_keyboard_subscription = g_gamepad_subscription = 0;
  g_keyboard_api = nullptr;
  g_gamepad_api = nullptr;
  if (g_provider && g_keyboard_lease) (void)g_provider->release(g_keyboard_lease);
  if (g_provider && g_gamepad_lease) (void)g_provider->release(g_gamepad_lease);
  g_keyboard_lease = g_gamepad_lease = 0;
  g_provider = nullptr;
  portENTER_CRITICAL(&g_input_lock);
  g_keys = {};
  g_sampled_keys = {};
  g_keyboard_head = g_keyboard_count = 0;
  g_gamepad = {};
  g_gamepad_head = g_gamepad_count = 0;
  g_gamepad_overflows = 0;
  g_pending_keyboard_actions = 0;
  portEXIT_CRITICAL(&g_input_lock);
  g_buttons = g_navigation = g_actions = 0;
  g_previous = 0;
  g_settings_chord = g_rotate_chord = g_select_consumed = false;
}

void usb_hid_gamepad_begin() {
  // No worker-task provider calls. Owner acquires and polls the capability.
}

uint8_t usb_hid_gamepad_buttons() {
  UsbHidGamepadState pad;
  UsbHidKeyboardKeys keys;
  uint8_t pending;
  portENTER_CRITICAL(&g_input_lock);
  if (g_gamepad_count) {
    g_gamepad = g_gamepad_queue[g_gamepad_head];
    g_gamepad_head = (g_gamepad_head + 1) % kGamepadQueueCapacity;
    --g_gamepad_count;
  }
  pad = g_gamepad;
  if (g_keyboard_count) {
    g_sampled_keys = g_keyboard_queue[g_keyboard_head];
    g_keyboard_head = (g_keyboard_head + 1) % kKeyboardQueueCapacity;
    --g_keyboard_count;
  }
  keys = g_sampled_keys;
  pending = g_pending_keyboard_actions;
  g_pending_keyboard_actions = 0;
  portEXIT_CRITICAL(&g_input_lock);
  const UsbHidKeyboardMapping mapped = usb_hid_map_keyboard(keys, keys);
  pad.up |= mapped.gamepad.up;
  pad.down |= mapped.gamepad.down;
  pad.left |= mapped.gamepad.left;
  pad.right |= mapped.gamepad.right;
  pad.a |= mapped.gamepad.a;
  pad.b |= mapped.gamepad.b;
  pad.x |= mapped.gamepad.x;
  pad.y |= mapped.gamepad.y;
  pad.l |= mapped.gamepad.l;
  pad.r |= mapped.gamepad.r;
  pad.start |= mapped.gamepad.start;
  pad.select |= mapped.gamepad.select;
  decode_actions(pressed_mask(pad), millis());
  g_actions |= pending;
  return g_buttons;
}

uint8_t usb_hid_gamepad_navigation_buttons() { return g_navigation; }
uint8_t usb_hid_gamepad_take_actions() {
  const uint8_t actions = g_actions;
  g_actions = 0;
  return actions;
}
