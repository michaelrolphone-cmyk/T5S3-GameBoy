// Execute the staged pacer against a clock with real tick-boundary rounding.
#include <cassert>
#include <cstdint>
#include <cstdio>

static int64_t now;
static unsigned delays;
static int64_t esp_timer_get_time() { return now; }
static void vTaskDelay(unsigned ticks) {
  ++delays;
  now = (now / (TICK_MS * 1000) + ticks) * (TICK_MS * 1000);
}
static void delayMicroseconds(unsigned us) { now += us; }

// PACER_CONSTANTS
// PACER_STATE
// PACER_FUNCTIONS

static void start(GameFramePacer &p) {
  now = 0;
  delays = 0;
  reset_game_frame_pacer(p);
}

int main() {
  GameFramePacer p{};
  start(p);
  // Normal 1 ms work keeps the original DMG rate with either tick size.
  for (unsigned i = 0; i < 1000; ++i) { now += 1000; pace_game_frame(p); }
  const int64_t target = kGameFramePeriodNumeratorUs * 1000 / kDmgClockHz;
  assert(now >= target && now - target < TICK_MS * 1000);

  start(p);
  now = 6000;
  pace_game_frame(p);
  if (TICK_MS == 10) assert(delays == 2); // No extra sleep after overshooting.
  const unsigned before = delays;
  now += 17000;
  pace_game_frame(p);
  assert(delays == before); // The recent normal pacing wait already cooperated.

  start(p);
  // A loaded emulator needs 17 ms of useful work per frame. The old ELF
  // patch slept after every frame, reducing throughput further.
  for (unsigned i = 0; i < 1000; ++i) {
    now += 17000;
    pace_game_frame(p);
  }
  if (ELF_PACER) {
    assert(delays >= 250 && delays <= 350);
    assert(now <= 17000000 + 350 * TICK_MS * 1000);
  } else {
    assert(delays == 0 && now == 17000000);
  }
  printf("%s, %d ms tick: 1,000 loaded frames, %u forced waits, %lld us\n",
         ELF_PACER ? "ELF" : "standalone", TICK_MS, delays, (long long)now);

  start(p);
  // Alternating expensive draws and cheap render skips must also catch up.
  for (unsigned i = 0; i < 1000; ++i) {
    now += i % 2 ? 12000 : 22000;
    pace_game_frame(p);
  }
  assert(now < 17500000 + (ELF_PACER ? 350 * TICK_MS * 1000 : 0));

  if (ELF_PACER) {
    start(p);
    // Frames with only a short spin remaining also need occasional idle time.
    for (unsigned i = 0; i < 120; ++i) { now += 16000; pace_game_frame(p); }
    assert(delays >= 20);
    start(p);
    // A frame longer than the cooperation window yields before catch-up reset.
    for (unsigned i = 0; i < 10; ++i) { now += 100000; pace_game_frame(p); }
    assert(delays == 10);
  }
}
