#pragma once
/* Vendored copy of RiscRTE input.touch.raw@1. Keep byte-for-byte ABI
 * compatibility with T5S3-Reader sdk/driver/RiscTouchV1.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RISC_TOUCH_API_V1 1u
#define RISC_TOUCH_MAX_CONTACTS 5u
#define RISC_TOUCH_MAX_SUBSCRIBERS 4u
#define RISC_TOUCH_QUEUE_LENGTH 32u
#define RISC_TOUCH_BUTTON_PRIMARY (1u << 0)

enum {
    RISC_TOUCH_EVENT_DOWN = 1u,
    RISC_TOUCH_EVENT_MOVE = 2u,
    RISC_TOUCH_EVENT_UP = 3u,
    RISC_TOUCH_EVENT_BUTTON_DOWN = 4u,
    RISC_TOUCH_EVENT_BUTTON_UP = 5u
};

typedef struct {
    uint8_t id;
    uint8_t reserved;
    uint16_t x;
    uint16_t y;
} risc_touch_contact_v1;

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ms;
    uint8_t kind;
    uint8_t id;
    uint16_t x;
    uint16_t y;
} risc_touch_event_v1;

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ms;
    uint16_t width;
    uint16_t height;
    uint8_t contact_count;
    uint8_t reserved[3];
    uint32_t buttons;
    risc_touch_contact_v1 contacts[RISC_TOUCH_MAX_CONTACTS];
} risc_touch_snapshot_v1;

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint64_t (*subscribe)(void *context);
    bool (*unsubscribe)(void *context, uint64_t subscription);
    bool (*poll)(void *context, size_t max_reports);
    int32_t (*next)(void *context, uint64_t subscription,
                    risc_touch_event_v1 *out);
    bool (*snapshot)(void *context, risc_touch_snapshot_v1 *out);
} risc_touch_api_v1;

#ifdef __cplusplus
}
#endif
