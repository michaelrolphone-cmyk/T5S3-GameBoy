#pragma once
/* Vendored optional RiscRTE driver contract. Keeps this app buildable against
 * host firmware SDKs predating the independently installable 0.1.2 driver. */
#include "RiscUsbHidV1.h"

/* Optional append-only extension of usb.hid.gamepad@1 and the same semantic
 * input API used by usb.xinput.gamepad@1. The base struct_size
 * covers this entire table. Old consumers keep using the unchanged prefix;
 * new consumers check the size before reading this callback. No USB I/O is
 * performed: diagnostic copies the most recent discovery/poll state, bounded
 * by capacity and always NUL terminated. A true result need not be an error. */
typedef struct {
    risc_usb_gamepad_api_v1 base;
    bool (*diagnostic)(void *context, char *out, size_t capacity);
} risc_usb_gamepad_diagnostics_v1;
