#pragma once
#include <stdint.h>
/* Implemented by the ELF app from RiscRTE's exported monotonic milliseconds.
 * No esp_timer symbol is imported from firmware. */
int64_t riscrte_gameboy_timer_us(void);
static inline int64_t esp_timer_get_time(void) {
    return riscrte_gameboy_timer_us();
}
