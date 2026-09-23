// Optional SDK extension, vendored to keep older host SDK builds compatible.
#pragma once
#include "RiscUsbInterruptV1.h"

/* Optional read-only suffixes, checked using the existing base struct_size.
 * Calls copy live root-port state plus retained enumeration diagnostics into
 * a bounded NUL-terminated buffer. They never consume events or perform I/O.
 * The host forwards its controller's snapshot without interpreting it. */
typedef struct {
    risc_usb_controller_interrupt_v1 base;
    bool (*diagnostic)(void *context, char *out, size_t capacity);
} risc_usb_controller_diagnostics_v1;
typedef struct {
    risc_usb_host_interrupt_v1 base;
    bool (*diagnostic)(void *context, char *out, size_t capacity);
} risc_usb_host_diagnostics_v1;
