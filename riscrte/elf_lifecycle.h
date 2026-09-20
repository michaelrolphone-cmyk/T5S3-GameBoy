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
// Captures t5_app_get_api()/t5_storage_get_api() on the host session task.
// Do not scan here: the ROM catalog is too large for loopTask.
void paperboy_storage_bind_host();
