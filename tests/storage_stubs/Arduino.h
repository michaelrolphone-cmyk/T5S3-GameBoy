#pragma once
#include <thread>
#include <chrono>
using TaskHandle_t = void *;
inline TaskHandle_t xTaskGetCurrentTaskHandle() { static thread_local int task; return &task; }
inline void vTaskDelay(unsigned) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
inline void yield() { std::this_thread::yield(); }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(bool, unsigned) { vTaskDelay(1); return 1; }
#define pdTRUE true
#define pdMS_TO_TICKS(x) (x)
