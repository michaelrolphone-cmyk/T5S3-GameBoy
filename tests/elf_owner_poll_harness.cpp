// Run the real storage/USB owner loop, including early notifications, slow
// providers, continuous storage requests and millis() wraparound.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

struct OwnerRequest {
  void (*execute)(void *);
  void *context;
  bool done;
};
static OwnerRequest *g_owner_request;
static bool g_console_done;
static uint32_t origin, elapsed, poll_cost, requests;
static bool early_notifications;
static std::vector<uint32_t> polls;
static uint32_t millis() { return origin + elapsed; }
#define pdTRUE true
#define pdMS_TO_TICKS(ms) ((ms) / TICK_MS)
static void advance(uint32_t ms) {
  elapsed += ms;
  if (elapsed >= 1000) g_console_done = true;
}
static unsigned ulTaskNotifyTake(bool, unsigned ticks) {
  assert(ticks > 0);
  advance(early_notifications ? 1 : ticks * TICK_MS);
  return early_notifications ? 1 : 0;
}
static void paperboy_usb_owner_poll() {
  polls.push_back(elapsed);
  advance(poll_cost);
}
static void paperboy_touch_owner_poll() {}
static void serial_flush(bool = false) {}
static void serial_append(const char *) {}

// OWNER_LOOP

static void execute_request(void *context) {
  ++requests;
  advance(1);
  if (!g_console_done) g_owner_request = static_cast<OwnerRequest *>(context);
}

static void run(uint32_t start, bool notified, bool busy, uint32_t cost) {
  origin = start; elapsed = requests = 0; polls.clear();
  g_console_done = false; early_notifications = notified;
  poll_cost = cost;
  OwnerRequest request{execute_request, nullptr, false};
  request.context = &request;
  g_owner_request = busy ? &request : nullptr;
  paperboy_storage_owner_wait();
  assert(!polls.empty() && polls.front() <= 1);
  assert(polls.size() <= 251); // At most 250 Hz even when requests keep waking us.
  for (size_t i = 1; i < polls.size(); ++i) {
    assert(polls[i] - polls[i - 1] >= 4);
    assert(polls[i] - polls[i - 1] <= 4 + cost + TICK_MS);
  }
  if (!cost && (TICK_MS == 1 || notified || busy)) assert(polls.size() == 250);
  if (busy) assert(requests > 0 && request.done);
  printf("Owner, %d ms tick: %zu polls, %u storage requests, early wake=%u, poll cost=%u\n",
         TICK_MS, polls.size(), requests, unsigned(notified), cost);
}

int main() {
  run(0, false, false, 0);
  run(0, true, false, 0);
  run(0, false, true, 0);
  run(UINT32_MAX - 100, true, false, 0);
  run(0, false, false, 7);
}
