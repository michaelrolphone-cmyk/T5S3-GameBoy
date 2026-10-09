#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RISC_STREAM_PROVIDER_API_V1 1u
/* Handles, kinds, rights and result values match T5StreamApi byte-v1/record-v2.
 * The loader supplies this table to one mapped provider generation. context is
 * an opaque authority, never a caller-selected owner ID. No ELF callbacks or
 * data pointers are retained. Every transfer is synchronous and bounded to
 * 512 bytes; AGAIN is backpressure, not permission to spin. Worker tasks must
 * yield and finish before quiesce returns true. This ABI grants no device I/O.
 */
typedef struct {
    uint32_t struct_size, kind, rights, byte_capacity;
    const char *schema;
    uint32_t max_record, record_capacity;
} risc_stream_endpoint_v1;
typedef struct {
    uint32_t api_version, struct_size;
    uint64_t context;
    int32_t (*publish)(uint64_t, const risc_stream_endpoint_v1 *, uint32_t *);
    int32_t (*produce)(uint64_t, uint32_t, const void *, uint32_t, uint32_t *);
    int32_t (*consume)(uint64_t, uint32_t, void *, uint32_t, uint32_t *);
    int32_t (*produce_record)(uint64_t, uint32_t, const void *, uint32_t);
    int32_t (*consume_record)(uint64_t, uint32_t, void *, uint32_t, uint32_t *);
    int32_t (*finish)(uint64_t, uint32_t, int32_t);
    int32_t (*close)(uint64_t, uint32_t);
} risc_stream_provider_v1;
#ifdef __cplusplus
}
#endif
