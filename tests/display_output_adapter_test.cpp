#include <RiscDisplayOutputV1.h>
#include <T5ProviderCapabilityApi.h>
#include "epd_video.h"
#include "elf_lifecycle.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
bool owner = true;
uint8_t pixels[64800];
uint32_t format = RISC_DISPLAY_FORMAT_MONO1;
uint8_t present_state = RISC_DISPLAY_PRESENT_QUEUED;
bool submit_ok = true, status_ok = true;
unsigned released_frames = 0, released_leases = 0, status_calls = 0;
risc_display_frame_v1 next_frame = 1;

bool get_info(void *, risc_display_info_v1 *out) {
  assert(owner);
  *out = {};
  out->api_version = 1;
  out->struct_size = sizeof(*out);
  out->width = 960; out->height = 540;
  out->supported_formats = RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_MONO1);
  return true;
}
bool acquire(void *, uint32_t requested, risc_display_surface_v1 *out) {
  assert(owner && requested == RISC_DISPLAY_FORMAT_MONO1);
  *out = {next_frame++, pixels, 960, 540, 120, sizeof(pixels), format};
  return true;
}
void release(void *, risc_display_frame_v1 frame) {
  assert(owner && frame); ++released_frames;
}
bool submit(void *, risc_display_frame_v1 frame, const risc_display_rect_v1 *damage,
            size_t count, const risc_display_present_options_v1 *,
            risc_display_present_token_v1 *token) {
  assert(owner && frame);
  if (count) assert(count == 1 && damage && damage->width == 960);
  if (submit_ok) *token = 77;
  return submit_ok;
}
bool status(void *, risc_display_present_token_v1 token, risc_display_present_status_v1 *out) {
  assert(owner && token == 77); ++status_calls;
  *out = {}; out->state = present_state; return status_ok;
}
risc_display_output_api_v1 display = {
  1, sizeof(display), nullptr, get_info, acquire, release, submit, status, nullptr, nullptr
};
bool lease_acquire(const char *name, uint32_t version,
                   t5_provider_capability_lease_t *lease, const void **out) {
  assert(owner && !std::strcmp(name, "display.output") && version == 1);
  *lease = 1; *out = &display; return true;
}
bool lease_release(t5_provider_capability_lease_t lease) {
  assert(owner && lease == 1); ++released_leases; return true;
}
t5_provider_capability_api_v1 host = {1, sizeof(host), lease_acquire, lease_release, nullptr};
}

extern "C" const t5_provider_capability_api_v1 *t5_provider_capability_get_api(uint32_t version) {
  assert(owner && version == 1); return &host;
}
uint32_t millis() { return 1000; }
void vTaskDelay(unsigned) {}
void paperboy_owner_call(void (*fn)(void *), void *context) {
  const bool prior = owner; owner = true; fn(context); owner = prior;
}

int main(int argc, char **argv) {
  const bool bad_format = argc > 1 && !std::strcmp(argv[1], "format");
  if (bad_format) {
    format = RISC_DISPLAY_FORMAT_GRAY2;
    assert(!paperboy_display_owner_begin());
    assert(released_frames == 1 && released_leases == 1);
    assert(!epd_video_get_backbuffer() && !epd_video_submit_pending());
    std::puts("Wrong returned pixel format is rejected and released: PASS");
    return 0;
  }
  assert(paperboy_display_owner_begin());
  owner = false;
  assert(epd_video_get_backbuffer() == pixels);
  assert(epd_video_get_backbuffer_size() == sizeof(pixels));
  assert(!epd_video_submit_pending());
  submit_ok = false;
  assert(!epd_video_submit(0, 540));
  assert(epd_video_get_backbuffer() == pixels); // rejected submit retains frame
  submit_ok = true;
  assert(epd_video_submit(0, 540));
  for (uint8_t state : {uint8_t(RISC_DISPLAY_PRESENT_QUEUED), uint8_t(RISC_DISPLAY_PRESENT_ACTIVE)}) {
    present_state = state; assert(epd_video_submit_pending());
  }
  status_ok = false;
  assert(epd_video_submit_pending()); // unknown status is not fabricated success
  status_ok = true;
  for (uint8_t state : {uint8_t(RISC_DISPLAY_PRESENT_COMPLETE),
                        uint8_t(RISC_DISPLAY_PRESENT_SUPERSEDED), uint8_t(RISC_DISPLAY_PRESENT_FAILED)}) {
    present_state = state; assert(!epd_video_submit_pending());
  }
  assert(epd_video_can_submit());
  paperboy_display_owner_end();
  assert(released_frames == 1 && released_leases == 1);
  assert(!epd_video_submit_pending());
  owner = true;
  assert(paperboy_display_owner_begin());
  const unsigned calls = status_calls;
  owner = false;
  assert(!epd_video_submit_pending()); // no stale token across a new session
  assert(status_calls == calls);
  paperboy_display_owner_end();
  std::puts("Presentation states, owner calls, retained frames and restart: PASS");
}
