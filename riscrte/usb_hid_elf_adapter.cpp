// ELF-only input backend. RiscRTE owns USB, its PHY and VBUS; never import
// usb_host_* or initialize a second host from an unloadable app.
#include "usb_hid_gamepad.h"

#include <Arduino.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include <T5ProviderCapabilityApi.h>
#include <RiscUsbHidV1.h>
#include <RiscUsbInterruptV1.h>
#include "RiscUsbGamepadDiagnosticsV1.h"
#include "RiscUsbDiscoveryDiagnosticsV1.h"

#include "gbemu.h"
#include "snes_mini_controller.h"
#include "usb_hid_keyboard.h"
#include "usb_hid_report.h"

void paperboy_storage_hid_diagnostic(const char *message);

namespace {
constexpr char kTag[] = "gameboy_hid";
constexpr uint32_t kTurboHalfMs = 50;
constexpr size_t kSnapshotCapacity = 4;

// Provider calls, grants and subscriptions belong exclusively to the ELF
// owner task. The console task only reads copied state under this lock.
const t5_provider_capability_api_v1 *g_provider = nullptr;
constexpr uint32_t kAcquireRetryMs = 5000;
uint32_t g_last_acquire_ms = 0;
bool g_owner_active = false;
bool g_keyboard_poll_failed = false;
bool g_keyboard_key_seen = false;
bool g_keyboard_acquire_failed = false;
bool g_gamepad_acquire_failed = false;
bool g_host_probe_attempted = false;
bool g_diagnostic_active = false;
const risc_usb_host_interrupt_v1 *g_host_api = nullptr;
t5_provider_capability_lease_t g_host_lease = 0;
uint8_t g_configuration[RISC_USB_CONFIG_LIMIT];
void acquire_error(const char *capability) {
  paperboy_storage_hid_diagnostic(capability);
}
const risc_usb_keyboard_api_v1 *g_keyboard_api = nullptr;
t5_provider_capability_lease_t g_keyboard_lease = 0;
uint64_t g_keyboard_subscription = 0;
struct GamepadProvider {
  constexpr GamepadProvider(const char *name) : capability(name) {}
  const char *capability;
  const risc_usb_gamepad_api_v1 *api = nullptr;
  t5_provider_capability_lease_t lease = 0;
  bool poll_failed = false;
  risc_usb_gamepad_state_v1 state{};
};
GamepadProvider g_gamepads[] = {{"usb.hid.gamepad"}, {"usb.xinput.gamepad"}};
GamepadProvider *g_active_gamepad = nullptr;
portMUX_TYPE g_input_lock = portMUX_INITIALIZER_UNLOCKED;
UsbGamepadTestStatus g_test = {};
void diagnostic_event(const char *message) {
  portENTER_CRITICAL(&g_input_lock);
  memcpy(g_test.events[0], g_test.events[1], sizeof(g_test.events[0]));
  memcpy(g_test.events[1], g_test.events[2], sizeof(g_test.events[1]));
  snprintf(g_test.events[2], sizeof(g_test.events[2]), "%s", message);
  portEXIT_CRITICAL(&g_input_lock);
  paperboy_storage_hid_diagnostic(message);
}
void diagnostic_stage(const char *stage) {
  char bounded[sizeof(g_test.stage)] = {};
  snprintf(bounded, sizeof(bounded), "%s", stage);
  bool changed;
  portENTER_CRITICAL(&g_input_lock);
  changed = strcmp(g_test.stage, bounded) != 0;
  if (changed) memcpy(g_test.stage, bounded, sizeof(bounded));
  portEXIT_CRITICAL(&g_input_lock);
  if (changed) diagnostic_event(stage);
}
void gamepad_error(const char *message) {
  portENTER_CRITICAL(&g_input_lock);
  snprintf(g_test.error, sizeof(g_test.error), "%s", message);
  portEXIT_CRITICAL(&g_input_lock);
  diagnostic_event(message);
}
void capability_error(const char *fallback) {
  char reason[80] = {};
  if (g_provider && g_provider->struct_size >=
          offsetof(t5_provider_capability_api_v1, last_error) + sizeof(g_provider->last_error) &&
      g_provider->last_error && g_provider->last_error(reason, sizeof(reason)) && reason[0])
    gamepad_error(reason);
  else gamepad_error(fallback);
}

template <size_t N>
bool diagnostic_contains(const char *reason, size_t capacity, const char (&marker)[N]) {
  // strstr is not part of RiscRTE's exported ELF ABI. Keep the diagnostic
  // scan bounded and use the already supported strncmp import.
  for (size_t at = 0; at + N <= capacity && reason[at]; ++at)
    if (strncmp(reason + at, marker, N - 1) == 0) return true;
  return false;
}

// Read the already-polling host's snapshot. Never consume USB events, claim an
// interface, or perform a transfer from this diagnostic path.
void probe_usb_discovery() {
  if (!g_host_api) { diagnostic_stage("USB HOST DIAGNOSTIC UNAVAILABLE"); return; }
  const auto &host = g_host_api->discovery;
  uint64_t devices[RISC_USB_HOST_MAX_DEVICES] = {};
  size_t count = RISC_USB_HOST_MAX_DEVICES;
  if (!host.devices(host.host.context, devices, &count) || count > RISC_USB_HOST_MAX_DEVICES) {
    diagnostic_stage("USB DEVICE SNAPSHOT FAILED"); return;
  }
  portENTER_CRITICAL(&g_input_lock);
  const uint16_t previous_vid = g_test.vid, previous_pid = g_test.pid;
  const uint8_t previous_devices = g_test.usb_devices;
  g_test.usb_devices = static_cast<uint8_t>(count);
  g_test.hid_interfaces = 0;
  g_test.hid_protocol = 0;
  g_test.vid = g_test.pid = 0;
  portEXIT_CRITICAL(&g_input_lock);
  if (!count) {
    if (host.host.struct_size >= sizeof(risc_usb_host_diagnostics_v1)) {
      const auto *extended = reinterpret_cast<const risc_usb_host_diagnostics_v1 *>(g_host_api);
      char reason[sizeof(g_test.error)] = {};
      if (extended->diagnostic &&
          extended->diagnostic(host.host.context, reason, sizeof(reason)) && reason[0]) {
        diagnostic_stage(reason);
        if (diagnostic_contains(reason, sizeof(reason), "ENUM FAIL:")) {
          portENTER_CRITICAL(&g_input_lock);
          snprintf(g_test.error, sizeof(g_test.error), "%s", reason);
          portEXIT_CRITICAL(&g_input_lock);
        }
        return;
      }
    }
    diagnostic_stage("NO USB DEVICE ENUMERATED"); return;
  }
  size_t length = sizeof(g_configuration);
  uint16_t vid = 0, pid = 0;
  if (!host.host.configuration(host.host.context, devices[0], g_configuration,
                               &length, &vid, &pid)) {
    diagnostic_stage("USB CONFIGURATION UNAVAILABLE"); return;
  }
  uint8_t hid_count = 0, protocol = 0;
  bool xinput = false;
  if (length >= 9 && length <= sizeof(g_configuration)) {
    for (size_t at = 0; at + 2 <= length;) {
      const uint8_t size = g_configuration[at];
      if (size < 2 || size > length - at) break;
      if (g_configuration[at + 1] == 4 && size >= 9 && g_configuration[at + 5] == 3) {
        if (!hid_count) protocol = g_configuration[at + 7];
        if (hid_count != UINT8_MAX) ++hid_count;
      }
      if (g_configuration[at + 1] == 4 && size >= 9 &&
          g_configuration[at + 5] == 0xff && g_configuration[at + 6] == 0x5d &&
          (g_configuration[at + 7] == 1 || g_configuration[at + 7] == 0x81)) xinput = true;
      at += size;
    }
  }
  const bool identity_changed = !previous_devices || previous_vid != vid || previous_pid != pid;
  portENTER_CRITICAL(&g_input_lock);
  g_test.vid = vid; g_test.pid = pid;
  g_test.hid_interfaces = hid_count;
  g_test.hid_protocol = protocol;
  // A readable configuration proves enumeration has since succeeded. Keep
  // class/poll failures, but do not display a prior detach/reset failure as
  // the current error beside a successfully connected gamepad.
  if (diagnostic_contains(g_test.error, sizeof(g_test.error), "ENUM FAIL:"))
    g_test.error[0] = 0;
  portEXIT_CRITICAL(&g_input_lock);
  if (identity_changed) {
    char line[64];
    snprintf(line, sizeof(line), "USB VID:%04X PID:%04X HID:%u PROTO:%u",
             unsigned(vid), unsigned(pid), unsigned(hid_count), unsigned(protocol));
    diagnostic_event(line);
  }
  // New driver packages expose the actual class-discovery failure. The v1
  // prefix remains compatible with installed older drivers and host SDKs.
  const auto *gamepad_api = g_gamepads[xinput ? 1 : 0].api;
  if ((hid_count || xinput) && gamepad_api &&
      gamepad_api->struct_size >= sizeof(risc_usb_gamepad_diagnostics_v1)) {
    const auto *extended = reinterpret_cast<const risc_usb_gamepad_diagnostics_v1 *>(gamepad_api);
    char reason[48] = {};
    if (extended->diagnostic &&
        extended->diagnostic(gamepad_api->context, reason, sizeof(reason)) && reason[0]) {
      diagnostic_stage(reason);
      if (diagnostic_contains(reason, sizeof(reason), "FAILED") ||
          diagnostic_contains(reason, sizeof(reason), "UNSUPPORTED") ||
          diagnostic_contains(reason, sizeof(reason), "EXHAUSTED")) {
        portENTER_CRITICAL(&g_input_lock);
        snprintf(g_test.error, sizeof(g_test.error), "%s", reason);
        portEXIT_CRITICAL(&g_input_lock);
      }
      return;
    }
  }
  diagnostic_stage(xinput ? (gamepad_api ? "XINPUT WAITING FOR REPORT" : "XINPUT DRIVER UNAVAILABLE") :
                     hid_count ? (g_test.connected ? "GAMEPAD REPORT CONNECTED" :
                     "USB HID SEEN; WAITING FOR GAMEPAD") :
                     "USB DEVICE HAS NO HID INTERFACE");
}
UsbHidKeyboardKeys g_keys;
constexpr size_t kKeyboardQueueCapacity = 32;
UsbHidKeyboardKeys g_keyboard_queue[kKeyboardQueueCapacity];
UsbHidKeyboardKeys g_sampled_keys;
size_t g_keyboard_head = 0, g_keyboard_count = 0;
UsbHidGamepadState g_gamepad;
// Gamepad reports are full state snapshots. The console samples the newest
// state, like the I2C and standalone USB backends; it must not replay a FIFO
// of historical holds after their releases have already arrived.
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
bool g_refresh_chord = false;
bool g_select_consumed = false;
bool g_start_consumed = false;

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

void map_gamepad(const risc_usb_gamepad_state_v1 &state, bool synchronize = false,
                 bool xinput = false) {
  UsbHidGamepadState pad{};
  if (state.connected) {
    const uint8_t hat = state.hat;
    if (hat < 8) {
      pad.up = hat == 0 || hat == 1 || hat == 7;
      pad.right = hat >= 1 && hat <= 3;
      pad.down = hat >= 3 && hat <= 5;
      pad.left = hat >= 5 && hat <= 7;
    }
    if (hat >= 8 || xinput) {
      constexpr int16_t threshold = 16384; // Same outer-quarter dead zone as native HID.
      pad.left |= state.x < -threshold;
      pad.right |= state.x > threshold;
      pad.up |= state.y < -threshold;
      pad.down |= state.y > threshold;
    }
    // Raw receiver face buttons differ from XInput's normalized layout.
    pad.a = (state.buttons & (xinput ? 0x02UL : 0x01UL)) != 0;
    pad.b = (state.buttons & (xinput ? 0x01UL : 0x02UL)) != 0;
    pad.x = (state.buttons & (xinput ? 0x08UL : 0x04UL)) != 0;
    pad.y = (state.buttons & (xinput ? 0x04UL : 0x08UL)) != 0;
    pad.l = (state.buttons & (1UL << 4)) != 0;
    pad.r = (state.buttons & (1UL << 5)) != 0;
    // HID exposes raw Button usages. The tested eight-button receiver has
    // Start=0x80, Select=0x40. XInput's normalized contract uses 0x200/0x100;
    // its 0x40/0x80 bits are triggers and must not become modifier buttons.
    pad.start = (state.buttons & (xinput ? 0x200UL : 0x80UL)) != 0;
    pad.select = (state.buttons & (xinput ? 0x100UL : 0x40UL)) != 0;
  }
  portENTER_CRITICAL(&g_input_lock);
  g_test.connected = state.connected;
  g_test.buttons = state.connected ? state.buttons : 0;
  g_test.compact_buttons = !xinput;
  g_test.x = state.connected ? state.x : 0;
  g_test.y = state.connected ? state.y : 0;
  g_test.rx = state.connected ? state.rx : 0;
  g_test.ry = state.connected ? state.ry : 0;
  g_test.hat = state.connected ? state.hat : 8;
  g_test.report_id = state.connected ? state.report_id : 0;
  if (!synchronize && state.connected && g_test.reports != UINT32_MAX) ++g_test.reports;
  g_gamepad = pad;
  portEXIT_CRITICAL(&g_input_lock);
}

void accept_gamepad(GamepadProvider &source, const risc_usb_gamepad_state_v1 &state,
                    bool synchronize = false) {
  // Select one physical controller for this single-player app. Events from
  // the other protocol must never release the active controller's buttons.
  if (source.state.connected && state.device && state.device != source.state.device) return;
  source.state = state;
  if (g_active_gamepad && g_active_gamepad->state.connected) {
    if (g_active_gamepad == &source) map_gamepad(state, synchronize, &source == &g_gamepads[1]);
    return;
  }
  g_active_gamepad = nullptr;
  for (auto &candidate : g_gamepads) {
    if (!candidate.state.connected) continue;
    g_active_gamepad = &candidate;
    map_gamepad(candidate.state, true, &candidate == &g_gamepads[1]);
    return;
  }
  map_gamepad(risc_usb_gamepad_state_v1{}, true);
}

bool same_gamepad_state(const risc_usb_gamepad_state_v1 &a,
                        const risc_usb_gamepad_state_v1 &b) {
  return a.device == b.device && a.connected == b.connected && a.buttons == b.buttons &&
      a.x == b.x && a.y == b.y && a.z == b.z && a.rx == b.rx && a.ry == b.ry &&
      a.rz == b.rz && a.hat == b.hat && a.report_id == b.report_id;
}

void gamepad_snapshot(GamepadProvider &source) {
  if (!source.api) return;
  risc_usb_gamepad_state_v1 states[kSnapshotCapacity] = {};
  risc_usb_gamepad_state_v1 current{};
  size_t count = kSnapshotCapacity;
  if (source.api->snapshot(source.api->context, states, &count) && count <= kSnapshotCapacity) {
    for (size_t i = 0; i < count; ++i) {
      if (!states[i].connected) continue;
      if (!current.connected || states[i].device == source.state.device) current = states[i];
      if (states[i].device == source.state.device) break;
    }
  }
  const bool changed = !same_gamepad_state(source.state, current);
  if (source.state.connected != current.connected)
    paperboy_storage_hid_diagnostic(current.connected ? "Gamepad connected" : "Gamepad disconnected");
  source.state = {}; // Permit switching when the previously selected device disappeared.
  accept_gamepad(source, current, !changed);
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
    g_refresh_chord = false;
    g_actions = SNES_ACTION_SETTINGS;
  }
  if (g_settings_chord) {
    g_previous = pressed;
    g_select_consumed = (pressed & select) != 0;
    g_start_consumed = (pressed & start) != 0;
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
  // Start+Select performs one hardware-style panel clear. A later complete
  // Settings chord can still take priority; suppress partial release actions.
  constexpr uint16_t refresh = start | select;
  if ((pressed & settings) == refresh && (rising & refresh) && !g_refresh_chord) {
    g_refresh_chord = true;
    g_actions = SNES_ACTION_REFRESH;
  }
  if (g_refresh_chord) {
    g_previous = pressed;
    g_select_consumed = (pressed & select) != 0;
    g_start_consumed = (pressed & start) != 0;
    if (!(pressed & settings)) g_refresh_chord = false;
    g_buttons = 0;
    return;
  }
  if (rising & 0x0008U) g_turbo_a_started = now;
  if (rising & 0x0020U) g_turbo_b_started = now;
  if (!(pressed & select)) g_select_consumed = false;
  if (!(pressed & start)) g_start_consumed = false;
  if ((pressed & (start | select)) != (start | select) &&
      (pressed & select) && (pressed & (left_shoulder | right_shoulder))) {
    g_select_consumed = true;
    if ((pressed & (left_shoulder | right_shoulder)) == left_shoulder &&
        (rising & (left_shoulder | select))) g_actions |= SNES_ACTION_DIM;
    if ((pressed & (left_shoulder | right_shoulder)) == right_shoulder &&
        (rising & (right_shoulder | select))) g_actions |= SNES_ACTION_BRIGHTEN;
  } else if ((pressed & start) && !(pressed & select)) {
    if (pressed & (left_shoulder | right_shoulder)) g_start_consumed = true;
    if ((pressed & (left_shoulder | right_shoulder)) == left_shoulder &&
        (rising & (left_shoulder | start))) g_actions |= SNES_ACTION_LOAD;
    if ((pressed & (left_shoulder | right_shoulder)) == right_shoulder &&
        (rising & (right_shoulder | start))) g_actions |= SNES_ACTION_SAVE;
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
  if ((pressed & start) && !g_start_consumed) g_buttons |= GBEMU_INPUT_START;
  if ((pressed & select) && !g_select_consumed) g_buttons |= GBEMU_INPUT_SELECT;
}

}  // namespace

// Both entry and exit are invoked on the app's original RiscRTE owner task.
void paperboy_usb_owner_begin() {
  g_owner_active = true;
  g_last_acquire_ms = millis();
  paperboy_storage_hid_diagnostic("USB provider acquisition started");
  if (!g_provider) g_provider = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
  if (!g_provider || g_provider->api_version != T5_PROVIDER_CAPABILITY_API_VERSION ||
      g_provider->struct_size < offsetof(t5_provider_capability_api_v1, release) + sizeof(g_provider->release) ||
      !g_provider->acquire || !g_provider->release) {
    ESP_LOGW(kTag, "RiscRTE provider capability API unavailable");
    g_provider = nullptr;
    paperboy_storage_hid_diagnostic("Provider API unavailable; retry scheduled");
    return;
  }
  const void *iface = nullptr;
  if (!g_keyboard_lease && g_provider->acquire("usb.hid.keyboard", RISC_USB_KEYBOARD_API_V1,
                          &g_keyboard_lease, &iface)) {
    const auto *api = static_cast<const risc_usb_keyboard_api_v1 *>(iface);
    if (api && api->api_version == RISC_USB_KEYBOARD_API_V1 &&
        api->struct_size >= sizeof(*api) && api->subscribe && api->unsubscribe &&
        api->poll && api->next && api->snapshot) {
      g_keyboard_api = api;
      g_keyboard_subscription = api->subscribe(api->context, 0);
      if (g_keyboard_subscription) paperboy_storage_hid_diagnostic("Keyboard subscribed; waiting for connection");
    }
    if (!g_keyboard_subscription) {
      (void)g_provider->release(g_keyboard_lease);
      g_keyboard_lease = 0;
      g_keyboard_api = nullptr;
    }
  }
  if (!g_keyboard_lease && !g_keyboard_acquire_failed)
    acquire_error("Keyboard acquisition failed; retry scheduled");
  g_keyboard_acquire_failed = !g_keyboard_lease;
  for (auto &source : g_gamepads) {
    iface = nullptr;
    if (source.lease || !g_provider->acquire(source.capability, RISC_USB_GAMEPAD_API_V1,
                                           &source.lease, &iface)) continue;
    const auto *api = static_cast<const risc_usb_gamepad_api_v1 *>(iface);
    if (api && api->api_version == RISC_USB_GAMEPAD_API_V1 &&
        api->struct_size >= sizeof(*api) && api->poll && api->snapshot) {
      source.api = api;
      gamepad_snapshot(source);
    }
    if (!source.api) {
      (void)g_provider->release(source.lease);
      source.lease = 0;
      source.api = nullptr;
    }
  }
  const bool ready = g_gamepads[0].api || g_gamepads[1].api;
  if (!ready && !g_gamepad_acquire_failed)
    capability_error("Gamepad driver acquisition failed");
  portENTER_CRITICAL(&g_input_lock);
  if (ready && !g_test.provider_ready) g_test.error[0] = 0;
  g_test.provider_ready = ready;
  portEXIT_CRITICAL(&g_input_lock);
  g_gamepad_acquire_failed = !ready;
  g_last_acquire_ms = millis(); // Back off from completion, including slow failed loads.
  ESP_LOGI(kTag, "RiscRTE HID grants keyboard=%u gamepad=%u",
           static_cast<unsigned>(g_keyboard_lease != 0),
           static_cast<unsigned>(ready));
  char grants[80];
  snprintf(grants, sizeof(grants), "Input grants keyboard=%u hid=%u xinput=%u",
           unsigned(g_keyboard_lease != 0), unsigned(g_gamepads[0].lease != 0),
           unsigned(g_gamepads[1].lease != 0));
  paperboy_storage_hid_diagnostic(grants);
}

void paperboy_usb_owner_poll() {
  // Retry only missing capabilities; never release a working subscription or
  // power-cycle a live controller because the other optional provider failed.
  if (g_owner_active && (!g_keyboard_lease || !g_gamepads[0].lease || !g_gamepads[1].lease) &&
      uint32_t(millis() - g_last_acquire_ms) >= kAcquireRetryMs)
    paperboy_usb_owner_begin();
  if (g_keyboard_api && g_keyboard_subscription) {
    const bool ok = g_keyboard_api->poll(g_keyboard_api->context, 4);
    if (ok == g_keyboard_poll_failed) {
      paperboy_storage_hid_diagnostic(ok ? "Keyboard polling recovered" : "Keyboard provider poll failed");
      g_keyboard_poll_failed = !ok;
    }
    // Poll can publish events before a later interface fails. Consume the
    // bounded event queue on both paths, including pending disconnects.
    for (unsigned n = 0; n < 32; ++n) {
      risc_usb_keyboard_event_v1 event{};
      const int32_t rc = g_keyboard_api->next(g_keyboard_api->context,
                                             g_keyboard_subscription, &event);
      if (!rc) break;
      if (rc < 0 || event.kind == 5) { keyboard_snapshot(); break; }
      if (event.kind == 1) { paperboy_storage_hid_diagnostic("Keyboard connected"); continue; }
      if (event.kind == 2) {
        paperboy_storage_hid_diagnostic("Keyboard disconnected");
        clear_keyboard(); g_keyboard_key_seen = false; continue;
      }
      if (event.kind == 3 && !g_keyboard_key_seen) {
        paperboy_storage_hid_diagnostic("Keyboard key event received");
        g_keyboard_key_seen = true;
      }
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
  for (auto &source : g_gamepads) {
    if (!source.api || !source.lease) continue;
    const bool ok = source.api->poll(source.api->context, 16);
    if (ok == source.poll_failed) {
      source.poll_failed = !ok;
      if (!ok) gamepad_error("Gamepad provider poll failed");
      else {
        paperboy_storage_hid_diagnostic("Gamepad polling recovered");
        portENTER_CRITICAL(&g_input_lock);
        if (!g_gamepads[0].poll_failed && !g_gamepads[1].poll_failed) g_test.error[0] = 0;
        portEXIT_CRITICAL(&g_input_lock);
      }
      portENTER_CRITICAL(&g_input_lock);
      g_test.poll_failed = g_gamepads[0].poll_failed || g_gamepads[1].poll_failed;
      portEXIT_CRITICAL(&g_input_lock);
    }
    // The consumer sees only the current controller snapshot. No gamepad
    // subscription, event cursor, or replay of intermediate button states.
    // A failed poll may have updated disconnect state; snapshot it as well.
    gamepad_snapshot(source);
  }
  // One bounded configuration read per second; HID/gamepad providers already
  // advance enumeration during their ordinary poll above.
  static uint32_t last_probe_ms = 0;
  if (g_provider && !g_host_probe_attempted) {
    g_host_probe_attempted = true;
    const void *iface = nullptr;
    if (g_provider->acquire("usb.host", RISC_USB_HOST_API_V1, &g_host_lease, &iface)) {
      const auto *api = static_cast<const risc_usb_host_interrupt_v1 *>(iface);
      if (api && api->discovery.host.api_version == RISC_USB_HOST_API_V1 &&
          api->discovery.host.struct_size >=
              offsetof(risc_usb_host_discovery_v1, devices) + sizeof(api->discovery.devices) &&
          api->discovery.devices && api->discovery.host.configuration) {
        g_host_api = api;
        diagnostic_event("USB host snapshot available");
      } else {
        (void)g_provider->release(g_host_lease);
        g_host_lease = 0;
      }
    }
    if (!g_host_api) {
      capability_error("USB host diagnostic acquisition failed");
      diagnostic_stage("USB HOST DIAGNOSTIC UNAVAILABLE");
    }
  }
  if (uint32_t(millis() - last_probe_ms) >= 1000U) {
    last_probe_ms = millis();
    probe_usb_discovery();
  }
  static uint32_t last_heartbeat_ms = 0;
  if (uint32_t(millis() - last_heartbeat_ms) >= 10000U) {
    last_heartbeat_ms = millis();
    UsbGamepadTestStatus state = usb_hid_gamepad_test_status();
    char line[128];
    snprintf(line, sizeof(line), "USB status stage=%s devices=%u HID=%u VID=%04X PID=%04X reports=%lu",
             state.stage, unsigned(state.usb_devices), unsigned(state.hid_interfaces),
             unsigned(state.vid), unsigned(state.pid), static_cast<unsigned long>(state.reports));
    paperboy_storage_hid_diagnostic(line);
  }
}

void paperboy_usb_owner_end() {
  g_owner_active = false;
  if (g_keyboard_api && g_keyboard_subscription)
    (void)g_keyboard_api->unsubscribe(g_keyboard_api->context, g_keyboard_subscription);
  for (auto &source : g_gamepads) {
    if (g_provider && source.lease) (void)g_provider->release(source.lease);
    const char *capability = source.capability;
    source = GamepadProvider{capability};
  }
  g_active_gamepad = nullptr;
  g_keyboard_subscription = 0;
  g_keyboard_api = nullptr;
  if (g_provider && g_keyboard_lease) (void)g_provider->release(g_keyboard_lease);
  if (g_provider && g_host_lease) (void)g_provider->release(g_host_lease);
  g_keyboard_lease = 0;
  g_host_lease = 0;
  g_host_api = nullptr;
  g_host_probe_attempted = false;
  g_diagnostic_active = false;
  g_provider = nullptr;
  portENTER_CRITICAL(&g_input_lock);
  g_test = {};
  portEXIT_CRITICAL(&g_input_lock);
  g_keyboard_acquire_failed = g_gamepad_acquire_failed = false;
  portENTER_CRITICAL(&g_input_lock);
  g_keys = {};
  g_sampled_keys = {};
  g_keyboard_head = g_keyboard_count = 0;
  g_gamepad = {};
  g_pending_keyboard_actions = 0;
  portEXIT_CRITICAL(&g_input_lock);
  g_buttons = g_navigation = g_actions = 0;
  g_previous = 0;
  g_settings_chord = g_rotate_chord = g_refresh_chord = false;
  g_select_consumed = g_start_consumed = false;
}

void usb_hid_gamepad_begin() {
  // No worker-task provider calls. Owner acquires and polls the capability.
}

uint8_t usb_hid_gamepad_buttons() {
  UsbHidGamepadState pad;
  UsbHidKeyboardKeys keys;
  uint8_t pending;
  portENTER_CRITICAL(&g_input_lock);
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

UsbGamepadTestStatus usb_hid_gamepad_test_status() {
  portENTER_CRITICAL(&g_input_lock);
  UsbGamepadTestStatus result = g_test;
  portEXIT_CRITICAL(&g_input_lock);
  return result;
}
void usb_hid_gamepad_test_active(bool active) {
  portENTER_CRITICAL(&g_input_lock);
  g_diagnostic_active = active;
  portEXIT_CRITICAL(&g_input_lock);
}
