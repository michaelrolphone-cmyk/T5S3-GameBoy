#pragma once
/* Transport-neutral raw touch surface capability. A physical touch provider
 * is the sole hardware owner and fans copied events out to independent
 * subscribers. Consumers use snapshot() as authoritative current state after
 * startup or an event gap so missed queue entries can never latch a contact. */
#include "RiscProviderV2.h"
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
    /* Contact ID for DOWN/MOVE/UP; button bit index for BUTTON_DOWN/UP. */
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
    /* A new subscriber starts without historical events. Call snapshot()
     * immediately after subscribe() to establish authoritative current state. */
    uint64_t (*subscribe)(void *context);
    bool (*unsubscribe)(void *context, uint64_t subscription);
    /* Service up to max_reports controller reports. Calls are serialized by
     * the provider itself across the ENTIRE report and state update, not only
     * individual bus transfers. Consumers may call from different tasks while
     * holding a live provider grant. Every subscriber gets its own event stream.
     * False can follow partial progress: drain next() even on poll failure.
     * Transient I/O failures retain unread data; discarded reports must signal
     * GAP. Lifecycle start/stop runs only on the grant-owning executor. */
    bool (*poll)(void *context, size_t max_reports);
    /* Returns 1 for one event, 0 when empty, -1 for stale handle or queue GAP,
     * and -2 for provider fault. On -1, discard derived state and snapshot(). */
    int32_t (*next)(void *context, uint64_t subscription,
                    risc_touch_event_v1 *out);
    bool (*snapshot)(void *context, risc_touch_snapshot_v1 *out);
} risc_touch_api_v1;

#ifdef __cplusplus
}
#endif
