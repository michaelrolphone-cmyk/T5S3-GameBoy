#include "epd_video.h"
#include "elf_lifecycle.h"
#include <RiscDisplayOutputV1.h>
#include <T5ProviderCapabilityApi.h>
#include <Arduino.h>

/* GameBoy renders into the provider's borrowed framebuffer. Every capability
 * operation runs on the app owner through the existing bounded owner RPC; the
 * console task only writes pixels while its frame remains acquired. */
namespace {
const t5_provider_capability_api_v1 *host = nullptr;
const risc_display_output_api_v1 *display = nullptr;
t5_provider_capability_lease_t lease = 0;
risc_display_surface_v1 surface{};
risc_display_present_token_v1 last_token = 0;
uint32_t scan_epoch = 0;
bool ready = false;

bool acquire_frame() {
  if (!ready) return false;
  if (surface.frame) return true;
  if (!display->acquire(display->context, RISC_DISPLAY_FORMAT_MONO1, &surface))
    return false;
  if (surface.frame && surface.pixels && surface.width == 960 &&
      surface.height == 540 && surface.stride_bytes == 120 &&
      surface.size_bytes >= 64800)
    return true;
  if (surface.frame) display->release(display->context, surface.frame);
  surface = {};
  return false;
}

void end_on_owner(void *) {
  if (display && surface.frame) display->release(display->context, surface.frame);
  surface = {};
  display = nullptr;
  ready = false;
  if (host && lease) (void)host->release(lease);
  host = nullptr;
  lease = 0;
}
} // namespace

bool paperboy_display_owner_begin() {
  host = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
  const void *interface = nullptr;
  if (!host || host->struct_size < sizeof(*host) || !host->acquire || !host->release ||
      !host->acquire(RISC_DISPLAY_OUTPUT_CAPABILITY, RISC_DISPLAY_OUTPUT_API_V1,
                     &lease, &interface) || !lease || !interface) {
    end_on_owner(nullptr);
    return false;
  }
  display = static_cast<const risc_display_output_api_v1 *>(interface);
  risc_display_info_v1 info{};
  if (display->api_version != RISC_DISPLAY_OUTPUT_API_V1 ||
      display->struct_size < sizeof(*display) || !display->get_info ||
      !display->acquire || !display->release || !display->submit ||
      !display->present_status || !display->get_info(display->context, &info) ||
      info.width != 960 || info.height != 540 ||
      !(info.supported_formats & RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_MONO1))) {
    end_on_owner(nullptr);
    return false;
  }
  ready = true;
  scan_epoch = millis();
  if (!acquire_frame()) { end_on_owner(nullptr); return false; }
  return true;
}

void paperboy_display_owner_end() { paperboy_owner_call(end_on_owner, nullptr); }

bool paperboy_elf_prepare_display_bus() { return ready; }

bool epd_video_init(Pca9535Min &) { return ready; }
bool epd_video_power_on() { return ready; }
bool epd_video_start() { return ready; }

uint8_t *epd_video_get_backbuffer() {
  for (unsigned tries = 0; tries < 2000 && ready; ++tries) {
    bool ok = false;
    paperboy_owner_call([](void *arg) { *static_cast<bool *>(arg) = acquire_frame(); }, &ok);
    if (ok) return static_cast<uint8_t *>(surface.pixels);
    vTaskDelay(1);
  }
  return nullptr;
}
size_t epd_video_get_backbuffer_size() { return ready ? 64800 : 0; }

bool epd_video_submit(uint16_t y, uint16_t height) {
  struct Request { uint16_t y, height; bool ok; } request{y, height, false};
  paperboy_owner_call([](void *arg) {
    auto &r = *static_cast<Request *>(arg);
    if (!ready || !surface.frame || r.y >= surface.height ||
        r.height > surface.height - r.y) return;
    const risc_display_rect_v1 damage{0, r.y, surface.width, r.height};
    r.ok = display->submit(display->context, surface.frame,
                           r.height ? &damage : nullptr, r.height ? 1u : 0u,
                           nullptr, &last_token);
    if (r.ok) surface = {};
  }, &request);
  return request.ok;
}

void epd_video_flip(uint16_t y, uint16_t height) {
  for (unsigned tries = 0; tries < 2000 && !epd_video_submit(y, height); ++tries)
    vTaskDelay(1);
}

bool epd_video_can_submit() {
  bool ok = false;
  paperboy_owner_call([](void *arg) {
    *static_cast<bool *>(arg) = acquire_frame();
  }, &ok);
  return ok;
}

bool epd_video_submit_pending() {
  bool pending = false;
  paperboy_owner_call([](void *arg) {
    bool &value = *static_cast<bool *>(arg);
    risc_display_present_status_v1 status{};
    value = ready && last_token &&
        (!display->present_status(display->context, last_token, &status) ||
         (status.state != RISC_DISPLAY_PRESENT_COMPLETE &&
          status.state != RISC_DISPLAY_PRESENT_SUPERSEDED));
  }, &pending);
  return pending;
}

uint32_t epd_video_get_vsync_count() {
  return static_cast<uint32_t>((millis() - scan_epoch) * 24ULL / 1000ULL);
}

void epd_video_shutdown() { paperboy_display_owner_end(); }
