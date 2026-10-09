#pragma once
#include <stdint.h>
int64_t minimal_clock_us(void);
/* Cartridge/CPU operands may be odd-addressed. ESP32-S3 must not receive an
 * unaligned native halfword access for an emulated Game Boy read/write. */
static inline uint16_t minimal_read16(const void* p){const uint8_t* b=p;return (uint16_t)(b[0]|((uint16_t)b[1]<<8));}
static inline void minimal_write16(void* p,uint16_t v){uint8_t* b=p;b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);}
