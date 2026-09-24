#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint32_t test_now;
inline uint32_t millis() { return test_now; }
inline void delay(unsigned) {}
struct TestESP {
  uint32_t getFlashChipSize() const { return 8U * 1024U * 1024U; }
  uint32_t getPsramSize() const { return 8U * 1024U * 1024U; }
};
static const TestESP ESP{};
