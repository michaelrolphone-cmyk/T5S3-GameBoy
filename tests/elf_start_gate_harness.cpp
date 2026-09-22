// Run the real staged console entry with a virtual-time notification wait.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using TaskHandle_t = void *;
constexpr bool pdTRUE = true;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGE(...) ((void)0)
static const char *kTag __attribute__((unused)) = "test";
static TaskHandle_t s_elf_owner_task;
static uint32_t elapsed, provider_delay;
static bool owner_signalled, setup_ran, done, deleted;
static uint32_t ulTaskNotifyTake(bool, uint32_t wait) {
  assert(!deleted && !setup_ran && !done);
  if (wait < provider_delay - elapsed) {
    elapsed += wait;
    return 0;
  }
  elapsed = provider_delay;
  owner_signalled = true;
  return 1;
}
static void setup() { assert(owner_signalled && !deleted); setup_ran = true; }
static void paperboy_storage_owner_note_console_done() { done = true; }
static void xTaskNotifyGive(TaskHandle_t owner) { assert(owner && setup_ran && done); }
static void vTaskDelete(TaskHandle_t) { deleted = true; }
// STAGED_WORKER
int main() {
  for (uint32_t delay : {0U, 100U, 14999U, 15001U, 60000U}) {
    elapsed = 0; provider_delay = delay;
    owner_signalled = setup_ran = done = deleted = false;
    int owner;
    s_elf_owner_task = &owner;
    paperboy_elf_console_task(nullptr);
    assert(owner_signalled && setup_ran && done && deleted);
    assert(elapsed == delay && s_elf_owner_task == nullptr);
  }
  puts("Staged worker: alive through provider startup beyond 15s, setup and exit ordered: PASS");
}
