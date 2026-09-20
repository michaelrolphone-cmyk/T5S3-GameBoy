#pragma once
/* The standalone ESP-IDF build supplies the real attributes. The ELF must
 * remain in loader-mappable .text/.data rather than board firmware sections. */
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_IRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
