#pragma once

#include <stdint.h>

// Must match the merged public T5HardwareTakeover.h ABI. The handoff is
// generic: the host looks up app_hardware_takeover() before entering any ELF.
#ifndef T5_HARDWARE_TAKEOVER_DISPLAY
#define T5_HARDWARE_TAKEOVER_DISPLAY (1u << 0)
#endif

void paperboy_elf_request_exit();
bool paperboy_elf_exit_requested();
void paperboy_elf_note_boot_interrupt_attached();
void paperboy_storage_bind_host();
// Run host dir/stream calls on the session owner (loopTask). Blocks until
// the console worker requests exit.
void paperboy_storage_owner_wait();
void paperboy_storage_owner_note_console_done();

// RiscRTE provider grants and subscriptions are task-owned. All HID provider
// calls run here on the storage/ELF owner, never on the console worker.
void paperboy_usb_owner_begin();
void paperboy_usb_owner_poll();
void paperboy_usb_owner_end();

// Called on the console core before USB startup; setup reuses the bus.
bool paperboy_elf_prepare_display_bus();
