/*
 * timebase.h - Microsecond timebase (brain_us) backed by the DIGCTL counter.
 */
#ifndef BRAIN_TIMEBASE_H
#define BRAIN_TIMEBASE_H

#include <stdint.h>

int brain_timebase_init(void);
uint32_t brain_us(void);

#endif
