#include "usb_hid_gamepad.h"

#include <Arduino.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <usb/usb_host.h>

#include "gbemu.h"
#include "snes_mini_controller.h"
#include "usb_hid_keyboard.h"
#include "usb_hid_report.h"
#include "usb_xinput_report.h"

namespace {
constexpr char kTag[] = "usb_input";
constexpr uint8_t kMaxInterfaces = 8;
constexpr uint16_t kMaxDescriptor = 512;
constexpr uint32_t kTurboHalfMs = 50;

struct Candidate {
  uint8_t interface_number = 0;
  uint8_t alternate = 0;
  uint8_t endpoint = 0;
  uint16_t packet_size = 0;
  uint16_t descriptor_length = 0;
  bool boot_keyboard = false;
  bool xbox = false;
  bool wireless = false;
};

enum class ControlStage : uint8_t { Descriptor, BootProtocol };

portMUX_TYPE g_input_lock = portMUX_INITIALIZER_UNLOCKED;
UsbGamepadTestStatus g_test = {};
void test_error(const char *message) {
  portENTER_CRITICAL(&g_input_lock);
  snprintf(g_test.error, sizeof(g_test.error), "%s", message);
  portEXIT_CRITICAL(&g_input_lock);
}
UsbHidGamepadState g_raw;
UsbHidKeyboardKeys g_previous_keyboard;
uint8_t g_pending_keyboard_actions = 0;
usb_host_client_handle_t g_client = nullptr;
usb_device_handle_t g_device = nullptr;
usb_transfer_t *g_control = nullptr;
usb_transfer_t *g_interrupt = nullptr;
UsbHidGamepadReport g_layout;
UsbHidKeyboardReport g_keyboard_layout;
Candidate g_candidates[kMaxInterfaces];
ControlStage g_control_stage = ControlStage::Descriptor;
uint8_t g_candidates_count = 0;
uint8_t g_candidate = 0;
uint8_t g_pending_address = 0;
uint8_t g_active_interface = 0;
uint8_t g_endpoint = 0;
bool g_keyboard_active = false;
bool g_xbox_active = false;
bool g_xbox_wireless = false;
bool g_interface_claimed = false;
bool g_control_inflight = false;
bool g_control_done = false;
bool g_interrupt_inflight = false;
bool g_disconnect = false;
bool g_started = false;
uint8_t g_transfer_errors = 0;

uint8_t g_buttons = 0;
uint8_t g_navigation = 0;
uint8_t g_actions = 0;
uint16_t g_previous = 0;
uint32_t g_turbo_a_started = 0;
uint32_t g_turbo_b_started = 0;
bool g_settings_chord = false;
bool g_rotate_chord = false;
bool g_select_consumed = false;

void clear_live_keys() {
  portENTER_CRITICAL(&g_input_lock);
  g_raw = {};
  g_previous_keyboard = {};
  portEXIT_CRITICAL(&g_input_lock);
}

void clear_input() {
  portENTER_CRITICAL(&g_input_lock);
  g_raw = {};
  g_previous_keyboard = {};
  g_pending_keyboard_actions = 0;
  portEXIT_CRITICAL(&g_input_lock);
}

void library_task(void *) {
  for (;;) {
    uint32_t flags = 0;
    const esp_err_t rc = usb_host_lib_handle_events(pdMS_TO_TICKS(50), &flags);
    if (rc != ESP_OK && rc != ESP_ERR_TIMEOUT) {
      ESP_LOGW(kTag, "host library event error=%s", esp_err_to_name(rc));
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void on_client_event(const usb_host_client_event_msg_t *event, void *) {
  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    ESP_LOGI(kTag, "USB device enumerated address=%u", event->new_dev.address);
    portENTER_CRITICAL(&g_input_lock);
    g_test.usb_devices = 1;
    snprintf(g_test.stage, sizeof(g_test.stage), "USB DEVICE ENUMERATED");
    portEXIT_CRITICAL(&g_input_lock);
    if (!g_device && !g_pending_address) g_pending_address = event->new_dev.address;
  } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
             event->dev_gone.dev_hdl == g_device) {
    portENTER_CRITICAL(&g_input_lock);
    g_test.connected = false;
    g_test.buttons = 0;
    portEXIT_CRITICAL(&g_input_lock);
    ESP_LOGW(kTag, "USB device removed; releasing all buttons and keys");
    g_disconnect = true;
    portENTER_CRITICAL(&g_input_lock);
    g_test.usb_devices = 0;
    g_test.hid_interfaces = 0;
    snprintf(g_test.stage, sizeof(g_test.stage), "USB DEVICE REMOVED");
    portEXIT_CRITICAL(&g_input_lock);
    clear_input();
  }
}

// Keep boot keyboards behind gamepads, including vendor-class Xbox receivers.
void inspect_interfaces(const usb_config_desc_t *config) {
  g_candidates_count = 0;
  if (!config || config->wTotalLength < 9) return;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(config);
  const size_t length = config->wTotalLength;
  Candidate current = {};
  bool hid_interface = false;
  bool xbox_interface = false;
  bool have_endpoint = false;
  auto finish = [&]() {
    if ((hid_interface || xbox_interface) && have_endpoint &&
        (current.xbox || current.boot_keyboard || (current.descriptor_length &&
         current.descriptor_length <= kMaxDescriptor)) &&
        g_candidates_count < kMaxInterfaces) {
      g_candidates[g_candidates_count++] = current;
    }
  };
  for (size_t at = 0; at + 2 <= length;) {
    const uint8_t size = bytes[at];
    const uint8_t type = bytes[at + 1];
    if (size < 2 || at + size > length) break;
    if (type == 4 && size >= 9) {
      finish();
      current = {};
      hid_interface = bytes[at + 5] == 3;
      xbox_interface = usb_xinput_interface(bytes[at + 5], bytes[at + 6], bytes[at + 7]);
      have_endpoint = false;
      current.interface_number = bytes[at + 2];
      current.alternate = bytes[at + 3];
      current.boot_keyboard = bytes[at + 6] == 1 && bytes[at + 7] == 1;
      current.xbox = xbox_interface;
      current.wireless = xbox_interface && bytes[at + 7] == 0x81;
    } else if (hid_interface && type == 0x21 && size >= 9) {
      for (uint8_t i = 0; i < bytes[at + 5]; ++i) {
        const size_t off = at + 6U + size_t(i) * 3U;
        if (off + 3 > at + size) break;
        if (bytes[off] == 0x22) {
          current.descriptor_length = uint16_t(bytes[off + 1]) |
              (uint16_t(bytes[off + 2]) << 8U);
          break;
        }
      }
    } else if ((hid_interface || xbox_interface) && type == 5 && size >= 7 &&
               (bytes[at + 2] & 0x80U) && (bytes[at + 3] & 3U) == 3U &&
               !have_endpoint) {
      current.endpoint = bytes[at + 2];
      current.packet_size = uint16_t(bytes[at + 4]) |
          (uint16_t(bytes[at + 5]) << 8U);
      have_endpoint = current.packet_size >= 1 && current.packet_size <= 64;
    }
    at += size;
  }
  finish();
  for (uint8_t i = 0; i < g_candidates_count; ++i) {
    for (uint8_t j = uint8_t(i + 1); j < g_candidates_count; ++j) {
      if (g_candidates[i].boot_keyboard && !g_candidates[j].boot_keyboard) {
        const Candidate tmp = g_candidates[i];
        g_candidates[i] = g_candidates[j];
        g_candidates[j] = tmp;
      }
    }
  }
  ESP_LOGI(kTag, "USB input candidate interfaces=%u", g_candidates_count);
  portENTER_CRITICAL(&g_input_lock);
  g_test.hid_interfaces = 0;
  for (uint8_t i = 0; i < g_candidates_count; ++i)
    if (!g_candidates[i].xbox) ++g_test.hid_interfaces;
  snprintf(g_test.stage, sizeof(g_test.stage), "%s",
           g_candidates_count ? g_candidates[0].xbox ? "XBOX 360 USB INTERFACE FOUND" :
               "USB HID INTERFACE FOUND" : "NO SUPPORTED INPUT INTERFACE");
  portEXIT_CRITICAL(&g_input_lock);
}

void on_control(usb_transfer_t *) {
  g_control_inflight = false;
  g_control_done = true;
}

bool request_descriptor() {
  if (!g_device || g_candidate >= g_candidates_count) return false;
  const Candidate &candidate = g_candidates[g_candidate];
  if (!candidate.descriptor_length || candidate.descriptor_length > kMaxDescriptor) return false;
  const size_t size = 8U + candidate.descriptor_length;
  if (usb_host_transfer_alloc(size, 0, &g_control) != ESP_OK) return false;
  uint8_t *setup = g_control->data_buffer;
  setup[0] = 0x81; // Standard interface GET_DESCRIPTOR (report).
  setup[1] = 0x06;
  setup[2] = 0; setup[3] = 0x22;
  setup[4] = candidate.interface_number; setup[5] = 0;
  setup[6] = candidate.descriptor_length & 0xffU;
  setup[7] = candidate.descriptor_length >> 8U;
  g_control->device_handle = g_device;
  g_control->bEndpointAddress = 0;
  g_control->num_bytes = static_cast<int>(size);
  g_control->callback = on_control;
  g_control_stage = ControlStage::Descriptor;
  g_control_done = false;
  if (usb_host_transfer_submit_control(g_client, g_control) != ESP_OK) {
    usb_host_transfer_free(g_control);
    g_control = nullptr;
    return false;
  }
  g_control_inflight = true;
  return true;
}

bool request_boot_protocol() {
  if (!g_device || g_candidate >= g_candidates_count ||
      !g_candidates[g_candidate].boot_keyboard) return false;
  if (usb_host_transfer_alloc(8, 0, &g_control) != ESP_OK) return false;
  uint8_t *setup = g_control->data_buffer;
  setup[0] = 0x21; // Host-to-device, HID class, interface recipient.
  setup[1] = 0x0b; // SET_PROTOCOL(BOOT), mandatory for boot keyboards.
  setup[2] = 0; setup[3] = 0;
  setup[4] = g_candidates[g_candidate].interface_number; setup[5] = 0;
  setup[6] = 0; setup[7] = 0;
  g_control->device_handle = g_device;
  g_control->bEndpointAddress = 0;
  g_control->num_bytes = 8;
  g_control->callback = on_control;
  g_control_stage = ControlStage::BootProtocol;
  g_control_done = false;
  if (usb_host_transfer_submit_control(g_client, g_control) != ESP_OK) {
    usb_host_transfer_free(g_control);
    g_control = nullptr;
    return false;
  }
  g_control_inflight = true;
  return true;
}

bool activate_candidate();
bool start_candidate() {
  g_keyboard_active = false;
  g_xbox_active = false;
  if (g_candidate >= g_candidates_count) return false;
  if (g_candidates[g_candidate].xbox) return activate_candidate();
  if (g_candidates[g_candidate].boot_keyboard && request_boot_protocol()) return true;
  return request_descriptor();
}

void on_report(usb_transfer_t *transfer) {
  g_interrupt_inflight = false;
  if (g_disconnect || transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
    g_disconnect = true;
    clear_input();
    return;
  }
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    const size_t size = static_cast<size_t>(transfer->actual_num_bytes);
    if (g_xbox_active) {
      XboxInputReport decoded;
      const XboxReportKind kind = usb_xinput_decode(transfer->data_buffer, size,
                                                     g_xbox_wireless, decoded);
      if (kind == XboxReportKind::Disconnected) {
        clear_input();
        portENTER_CRITICAL(&g_input_lock);
        g_test.connected = false;
        g_test.buttons = 0;
        snprintf(g_test.stage, sizeof(g_test.stage), "XBOX RECEIVER; WAITING FOR PAD");
        portEXIT_CRITICAL(&g_input_lock);
      } else if (kind == XboxReportKind::Connected) {
        portENTER_CRITICAL(&g_input_lock);
        g_test.connected = true;
        snprintf(g_test.stage, sizeof(g_test.stage), "XBOX CONTROLLER PAIRED");
        portEXIT_CRITICAL(&g_input_lock);
      } else if (kind == XboxReportKind::Input) {
        portENTER_CRITICAL(&g_input_lock);
        g_raw = decoded.pad;
        g_test.connected = true;
        g_test.buttons = decoded.buttons;
        g_test.x = decoded.x; g_test.y = decoded.y;
        g_test.rx = decoded.rx; g_test.ry = decoded.ry;
        if (g_test.reports != UINT32_MAX) ++g_test.reports;
        snprintf(g_test.stage, sizeof(g_test.stage), "XBOX CONTROLLER REPORTS");
        portEXIT_CRITICAL(&g_input_lock);
      }
      g_transfer_errors = 0;
    } else if (g_keyboard_active) {
      // A report-ID keyboard may also send consumer-control reports. Ignore
      // their IDs rather than interpreting them as an empty keyboard.
      if (!g_keyboard_layout.report_id ||
          (size && transfer->data_buffer[0] == g_keyboard_layout.report_id)) {
        UsbHidKeyboardKeys keys = {};
        if (usb_hid_decode_keyboard(g_keyboard_layout, transfer->data_buffer,
                                    size, keys)) {
          const UsbHidKeyboardMapping mapped =
              usb_hid_map_keyboard(keys, g_previous_keyboard);
          portENTER_CRITICAL(&g_input_lock);
          g_previous_keyboard = keys;
          g_raw = mapped.gamepad;
          g_pending_keyboard_actions |= mapped.actions;
          portEXIT_CRITICAL(&g_input_lock);
          g_transfer_errors = 0;
        } else {
          // Short/rollover reports must not leave a movement or key latched.
          clear_live_keys();
        }
      }
    } else {
      UsbHidGamepadState decoded;
      if (usb_hid_decode_gamepad(g_layout, transfer->data_buffer, size, decoded)) {
        portENTER_CRITICAL(&g_input_lock);
        g_raw = decoded;
        g_test.buttons = (decoded.b ? 1U : 0U) | (decoded.a ? 2U : 0U) |
            (decoded.y ? 4U : 0U) | (decoded.x ? 8U : 0U) |
            (decoded.l ? 16U : 0U) | (decoded.r ? 32U : 0U) |
            (decoded.select ? 256U : 0U) | (decoded.start ? 512U : 0U);
        g_test.x = decoded.left ? -32767 : decoded.right ? 32767 : 0;
        g_test.y = decoded.up ? -32767 : decoded.down ? 32767 : 0;
        if (g_test.reports != UINT32_MAX) ++g_test.reports;
        portEXIT_CRITICAL(&g_input_lock);
        g_transfer_errors = 0;
      }
    }
  } else {
    ESP_LOGW(kTag, "USB interrupt IN status=%d", int(transfer->status));
    test_error("USB interrupt transfer failed");
    if (++g_transfer_errors >= 3) {
      g_disconnect = true;
      clear_input();
      return;
    }
    if (transfer->status == USB_TRANSFER_STATUS_STALL && g_device)
      (void)usb_host_endpoint_clear(g_device, g_endpoint);
  }
  if (usb_host_transfer_submit(transfer) == ESP_OK) {
    g_interrupt_inflight = true;
  } else {
    ESP_LOGW(kTag, "could not resubmit USB interrupt transfer");
    g_disconnect = true;
    clear_input();
  }
}

bool activate_candidate() {
  const Candidate &candidate = g_candidates[g_candidate];
  if (usb_host_interface_claim(g_client, g_device, candidate.interface_number,
                               candidate.alternate) != ESP_OK) return false;
  g_interface_claimed = true;
  g_active_interface = candidate.interface_number;
  g_endpoint = candidate.endpoint;
  g_xbox_active = candidate.xbox;
  g_xbox_wireless = candidate.wireless;
  const uint32_t bits = candidate.xbox ? 32U * 8U :
      (g_keyboard_active ? g_keyboard_layout.report_bits : g_layout.report_bits);
  const uint8_t id = candidate.xbox ? 0 :
      (g_keyboard_active ? g_keyboard_layout.report_id : g_layout.report_id);
  const uint32_t required = (bits + 7U) / 8U + (id ? 1U : 0U);
  const uint32_t size = ((required + candidate.packet_size - 1U) /
      candidate.packet_size) * candidate.packet_size;
  if (!size || size > 1024 ||
      usb_host_transfer_alloc(size, 0, &g_interrupt) != ESP_OK) return false;
  g_interrupt->device_handle = g_device;
  g_interrupt->bEndpointAddress = g_endpoint;
  g_interrupt->num_bytes = static_cast<int>(size);
  g_interrupt->callback = on_report;
  g_transfer_errors = 0;
  clear_input();
  if (usb_host_transfer_submit(g_interrupt) != ESP_OK) return false;
  g_interrupt_inflight = true;
  portENTER_CRITICAL(&g_input_lock);
  g_test.provider_ready = true;
  g_test.connected = !g_keyboard_active && !g_xbox_wireless;
  snprintf(g_test.stage, sizeof(g_test.stage), "%s",
           g_xbox_wireless ? "XBOX RECEIVER; WAITING FOR PAD" :
           g_xbox_active ? "XBOX CONTROLLER ACTIVE" :
           g_keyboard_active ? "USB KEYBOARD ACTIVE" : "GAMEPAD REPORT CONNECTED");
  g_test.report_id = id;
  g_test.hat = 8;
  g_test.error[0] = 0;
  portEXIT_CRITICAL(&g_input_lock);
  ESP_LOGI(kTag, "%s active interface=%u ep=0x%02x mps=%u report-id=%u bytes=%u",
           candidate.xbox ? "Xbox 360 receiver" :
               g_keyboard_active ? "USB keyboard" : "HID gamepad",
           candidate.interface_number, candidate.endpoint,
           candidate.packet_size, id, unsigned(required));
  return true;
}

void close_device() {
  clear_input();
  if (g_control_inflight || g_interrupt_inflight) return;
  if (g_control) {
    usb_host_transfer_free(g_control);
    g_control = nullptr;
  }
  if (g_interrupt) {
    usb_host_transfer_free(g_interrupt);
    g_interrupt = nullptr;
  }
  if (g_interface_claimed) {
    (void)usb_host_interface_release(g_client, g_device, g_active_interface);
    g_interface_claimed = false;
  }
  if (g_device) {
    (void)usb_host_device_close(g_client, g_device);
    g_device = nullptr;
  }
  g_candidates_count = 0;
  g_candidate = 0;
  g_pending_address = 0;
  g_control_done = false;
  g_keyboard_active = false;
  g_xbox_active = false;
  g_disconnect = false;
  portENTER_CRITICAL(&g_input_lock);
  g_test.connected = false;
  g_test.buttons = 0;
  portEXIT_CRITICAL(&g_input_lock);
}

void next_candidate() {
  if (g_interrupt) {
    (void)usb_host_transfer_free(g_interrupt);
    g_interrupt = nullptr;
  }
  if (g_interface_claimed) {
    (void)usb_host_interface_release(g_client, g_device, g_active_interface);
    g_interface_claimed = false;
  }
  for (++g_candidate; g_candidate < g_candidates_count; ++g_candidate) {
    if (start_candidate()) return;
    if (g_interrupt) { (void)usb_host_transfer_free(g_interrupt); g_interrupt = nullptr; }
    if (g_interface_claimed) {
      (void)usb_host_interface_release(g_client, g_device, g_active_interface);
      g_interface_claimed = false;
    }
  }
  g_disconnect = true;
}

void client_task(void *) {
  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 8;
  cfg.async.client_event_callback = on_client_event;
  if (usb_host_client_register(&cfg, &g_client) != ESP_OK) {
    ESP_LOGE(kTag, "could not register USB input client");
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(kTag, "USB host client ready; Xbox 360, HID gamepads and keyboards supported");
  portENTER_CRITICAL(&g_input_lock);
  g_test.provider_ready = true;
  portEXIT_CRITICAL(&g_input_lock);
  for (;;) {
    (void)usb_host_client_handle_events(g_client, pdMS_TO_TICKS(20));
    if (g_disconnect) {
      // Never free a submitted DMA transfer; await its completion first.
      close_device();
      continue;
    }
    if (g_control_done && g_control) {
      g_control_done = false;
      const Candidate &candidate = g_candidates[g_candidate];
      const bool completed = g_control->status == USB_TRANSFER_STATUS_COMPLETED;
      const ControlStage stage = g_control_stage;
      bool valid = false;
      if (stage == ControlStage::BootProtocol) {
        if (completed) {
          usb_hid_boot_keyboard_layout(g_keyboard_layout);
          g_keyboard_active = true;
          valid = true;
        }
      } else if (completed && g_control->actual_num_bytes >= 8) {
        const uint8_t *report = g_control->data_buffer + 8;
        const size_t bytes = static_cast<size_t>(g_control->actual_num_bytes - 8);
        if (usb_hid_parse_gamepad_descriptor(report, bytes, g_layout)) {
          g_keyboard_active = false;
          valid = true;
        } else if (usb_hid_parse_keyboard_descriptor(report, bytes, g_keyboard_layout)) {
          g_keyboard_active = true;
          valid = true;
        }
      }
      (void)usb_host_transfer_free(g_control);
      g_control = nullptr;
      if (valid && activate_candidate()) continue;
      if (stage == ControlStage::BootProtocol && !completed &&
          request_descriptor()) continue; // Noncompliant boot device fallback.
      ESP_LOGW(kTag, "interface %u is not a supported HID gamepad/keyboard",
               candidate.interface_number);
      next_candidate();
      continue;
    }
    if (!g_device && g_pending_address) {
      const uint8_t address = g_pending_address;
      g_pending_address = 0;
      if (usb_host_device_open(g_client, address, &g_device) != ESP_OK) continue;
      const usb_device_desc_t *descriptor = nullptr;
      const usb_config_desc_t *config = nullptr;
      if (usb_host_get_device_descriptor(g_device, &descriptor) == ESP_OK && descriptor) {
        ESP_LOGI(kTag, "USB VID=%04x PID=%04x", descriptor->idVendor,
                 descriptor->idProduct);
        portENTER_CRITICAL(&g_input_lock);
        g_test.vid = descriptor->idVendor;
        g_test.pid = descriptor->idProduct;
        portEXIT_CRITICAL(&g_input_lock);
      }
      if (usb_host_get_active_config_descriptor(g_device, &config) != ESP_OK || !config) {
        g_disconnect = true;
        continue;
      }
      inspect_interfaces(config);
      g_candidate = 0;
      if (!g_candidates_count) g_disconnect = true;
      if (!g_candidates_count) test_error("No supported HID or Xbox 360 interface");
      else if (!start_candidate()) {
        // The first candidate may fail allocation; try the remaining ones.
        next_candidate();
      }
    }
  }
}

uint16_t pressed_mask(const UsbHidGamepadState &s) {
  return (s.up ? 0x0001U : 0U) | (s.left ? 0x0002U : 0U) |
      (s.x ? 0x0008U : 0U) | (s.a ? 0x0010U : 0U) |
      (s.y ? 0x0020U : 0U) | (s.b ? 0x0040U : 0U) |
      (s.r ? 0x0200U : 0U) | (s.start ? 0x0400U : 0U) |
      (s.select ? 0x1000U : 0U) | (s.l ? 0x2000U : 0U) |
      (s.down ? 0x4000U : 0U) | (s.right ? 0x8000U : 0U);
}

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

void usb_hid_gamepad_begin() {
  if (g_started) return;
  g_started = true;
  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags = 0;
  const esp_err_t rc = usb_host_install(&cfg);
  if (rc != ESP_OK) {
    test_error("USB host install failed");
    ESP_LOGE(kTag, "USB OTG host install failed: %s (native USB CDC must be disabled)",
             esp_err_to_name(rc));
    return;
  }
  if (xTaskCreate(library_task, "usb_host_lib", 4096, nullptr, 5, nullptr) != pdPASS ||
      xTaskCreate(client_task, "usb_hid", 6144, nullptr, 4, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "USB host task creation failed");
  }
}

uint8_t usb_hid_gamepad_buttons() {
  usb_hid_gamepad_begin();
  UsbHidGamepadState current;
  uint8_t pending;
  portENTER_CRITICAL(&g_input_lock);
  current = g_raw;
  pending = g_pending_keyboard_actions;
  g_pending_keyboard_actions = 0;
  portEXIT_CRITICAL(&g_input_lock);
  decode_actions(pressed_mask(current), millis());
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
void usb_hid_gamepad_test_active(bool) {}
