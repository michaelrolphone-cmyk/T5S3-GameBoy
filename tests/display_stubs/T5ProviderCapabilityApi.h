#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define T5_PROVIDER_CAPABILITY_API_VERSION 1u
typedef uint32_t t5_provider_capability_lease_t;
#define T5_PROVIDER_CAPABILITY_LEASE_INVALID 0u

/* A generic runtime lease. No transport IDs, driver names, or physical I/O
 * operations are interpreted by this API. Returned interfaces are borrowed:
 * valid only on the owning app task until release or application termination.
 * The app's validated manifest must explicitly declare the capability in its
 * requires or optional list. A declaration does not automatically grant access;
 * requires additionally participates in mandatory launch gating.
 * This development ABI is not a substitute for future signed app admission. */
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    bool (*acquire)(const char *capability, uint32_t version,
                    t5_provider_capability_lease_t *lease,
                    const void **interface_out);
    bool (*release)(t5_provider_capability_lease_t lease);
    /* Append-only: copy the last acquire failure on the owning app task.
     * Check struct_size before use. No permissions are granted by diagnostics. */
    bool (*last_error)(char *out, size_t capacity);
} t5_provider_capability_api_v1;

const t5_provider_capability_api_v1 *t5_provider_capability_get_api(uint32_t version);
#ifdef __cplusplus
}
#endif
