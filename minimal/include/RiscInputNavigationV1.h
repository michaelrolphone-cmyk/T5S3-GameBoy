#pragma once
/* Transport-neutral navigation capability. Calls execute on the serialized
 * UI/provider owner task. No device descriptors or transport operations cross
 * this interface. Providers must not retain the foreground array or strings. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RISC_INPUT_NAVIGATION_API_V1 1u
#define RISC_INPUT_NAVIGATION_MAX_FOREGROUND 4u
enum {
    RISC_NAV_BACK = 1u << 0, RISC_NAV_CONFIRM = 1u << 1,
    RISC_NAV_LEFT = 1u << 2, RISC_NAV_RIGHT = 1u << 3,
    RISC_NAV_UP = 1u << 4, RISC_NAV_DOWN = 1u << 5,
    RISC_NAV_PAGE_BACK = 1u << 6, RISC_NAV_PAGE_FORWARD = 1u << 7,
    RISC_NAV_HOME = 1u << 8
};
typedef struct {
    const char *capability;
    uint32_t api_version;
} risc_input_foreground_v1;
typedef struct {
    uint32_t buttons, pressed, released;
} risc_input_navigation_frame_v1;
typedef struct {
    uint32_t api_version, struct_size;
    void *context;
    /* Bounded ready work only; the caller supplies scheduler cooperation. */
    bool (*poll)(void *context, risc_input_navigation_frame_v1 *out);
    /* Replace foreground claims atomically from the consumer's perspective.
     * Stop consuming overlapping sources before returning true. On failure
     * stay suppressed; caller must roll back its attempted foreground grant. */
    bool (*foreground)(void *context, const risc_input_foreground_v1 *claims,
                       size_t count);
    /* UI context boundary: discard navigation history and require neutral
     * input before rearming. Does not consume foreground-owned input. */
    bool (*reset)(void *context);
} risc_input_navigation_api_v1;

/* Optional tagged v1 suffix. Older providers retain the unchanged API prefix
 * and are semantic by default. Only the selected provider may declare that
 * LEFT/RIGHT represent its physical page-key pair; this is not a board flag. */
#define RISC_INPUT_NAVIGATION_TRAITS_TAG 0x4e415431u /* NAT1 */
#define RISC_INPUT_NAVIGATION_TRAITS_VERSION 1u
#define RISC_INPUT_NAVIGATION_PHYSICAL_PAGE_PAIR 1u
typedef struct {
    risc_input_navigation_api_v1 base;
    uint32_t traits_tag;
    uint32_t traits_version;
    uint32_t traits;
} risc_input_navigation_traits_v1;
static inline bool risc_input_navigation_has_physical_page_pair(const risc_input_navigation_api_v1 *api) {
    if (!api || api->api_version != RISC_INPUT_NAVIGATION_API_V1 ||
        api->struct_size < sizeof(risc_input_navigation_traits_v1)) return false;
    const risc_input_navigation_traits_v1 *extended = (const risc_input_navigation_traits_v1 *)api;
    return extended->traits_tag == RISC_INPUT_NAVIGATION_TRAITS_TAG &&
        extended->traits_version == RISC_INPUT_NAVIGATION_TRAITS_VERSION &&
        (extended->traits & RISC_INPUT_NAVIGATION_PHYSICAL_PAGE_PAIR) != 0u;
}
#ifdef __cplusplus
}
#endif
