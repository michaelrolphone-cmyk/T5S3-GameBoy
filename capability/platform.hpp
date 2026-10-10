#pragma once
#include <stddef.h>
#include <stdint.h>
#include <RiscStorageVolumeV1.h>

/* Original Paperboy runs on the capability-owning invocation. No worker task,
 * Arduino global, physical bus or pin is exposed to the application. */
bool cap_ready();
bool cap_retained();
bool cap_running();
void cap_hold(const char *operation);
void cap_fail(const char *operation, const char *detail);
void cap_log(const char *operation, const char *result, const char *detail);
void cap_logf(const char *severity, const char *format, ...);
void cap_rom_begin(const char *path);
void cap_rom_ready();
void cap_rom_frame(bool rendered);
void cap_rom_scene();
void cap_yield();
void cap_delay(uint32_t milliseconds);
uint32_t cap_millis();
int64_t cap_micros();
uint32_t cap_epoch();
void cap_clock_text(uint32_t epoch, char *out, size_t capacity);
const char *cap_device_label();
const char *cap_display_label();
bool cap_file_source(char *out,size_t capacity);
void cap_exit();
void *cap_alloc(size_t size);
void cap_free(void *memory);
bool cap_display_init();
void cap_service_display();
void cap_wait_display(uint8_t hold_frames);
bool cap_clean_display();
const risc_storage_volume_api_v1 *cap_storage();
void cap_console_setup();
void cap_console_cleanup();
void paperboy_owner_call(void (*function)(void *), void *context);
void paperboy_storage_bind_host();
void paperboy_serial_log(const char *message);

#define IRAM_ATTR
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(ms) cap_delay(ms)
#define delay(ms) cap_delay(ms)
#define delayMicroseconds(us) cap_delay(((us)+999u)/1000u)
#define millis() cap_millis()
#define esp_timer_get_time() cap_micros()
#define heap_caps_free(ptr) cap_free(ptr)
#define ESP_LOGI(tag,...) cap_logf("info",__VA_ARGS__)
#define ESP_LOGW(tag,...) cap_logf("warning",__VA_ARGS__)
#define ESP_LOGE(tag,...) cap_logf("error",__VA_ARGS__)
#define ESP_LOGD(tag,...) ((void)0)
