/*
 * timebase.c - Free-running microsecond timebase. Maps the i.MX28 DIGCTL
 * microseconds counter and reads it with a single load, in place of the OAL's
 * busy-looping QueryPerformanceCounter.
 */
#include <windows.h>
#include <grimace/extras.h>
#include <stdint.h>

#include "timebase.h"
#include "log.h"

#define DIGCTL_PHYS_BASE 0x8001C000u
#define DIGCTL_MAP_SIZE  0x2000u
#define DIGCTL_MICROSECONDS 0xC0

static volatile uint32_t *g_digctl;

int brain_timebase_init(void) {
    void *base = VirtualAlloc(NULL, DIGCTL_MAP_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (base == NULL) {
        brain_log("timebase: VirtualAlloc failed");
        return 0;
    }
    if (!VirtualCopy(base, (LPVOID)(DIGCTL_PHYS_BASE >> 8), DIGCTL_MAP_SIZE,
                     PAGE_READWRITE | PAGE_PHYSICAL | PAGE_NOCACHE)) {
        brain_log("timebase: VirtualCopy failed");
        return 0;
    }
    g_digctl = (volatile uint32_t *)base;
    return 1;
}

uint32_t brain_us(void) {
    return g_digctl ? g_digctl[DIGCTL_MICROSECONDS / 4] : 0;
}
