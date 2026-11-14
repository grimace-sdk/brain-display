#ifndef BRAIN_PRESENT_INTERNAL_H
#define BRAIN_PRESENT_INTERNAL_H

#include <windows.h>
#include <stdint.h>
#include "../abi/brain_present.h"

typedef struct {
    uint32_t physical[BRAIN_PRESENT_MAX_BUFFERS];
    uint32_t buffers;
    uint32_t size;
    uint32_t parameters;
} brain_present_overlay;

typedef struct {
    uint32_t physical[BRAIN_PRESENT_MAX_BUFFERS];
    void *kernel[BRAIN_PRESENT_MAX_BUFFERS];
    uint32_t buffers;
    uint32_t width;
    uint32_t height;
    uint32_t background;
    uint32_t flags;
    uint32_t overlays;
    brain_present_overlay overlay[BRAIN_PRESENT_MAX_OVERLAYS];
    uint32_t colour_key_low;
    uint32_t colour_key_high;
} brain_present_source;

void brain_present_init(void);
void brain_present_hold_cpu(int hold);
int brain_present_supported(ULONG code);
ULONG brain_present_escape(ULONG code, ULONG cjIn, PVOID pvIn, ULONG cjOut, PVOID pvOut);

int brain_refresh_begin_present(const brain_present_source *source, uint32_t owner_id);
void brain_refresh_end_present(void);
int brain_refresh_presenting(void);
int brain_refresh_capture_present(uint16_t *dest, int32_t width, int32_t height, int32_t stride_bytes);
int brain_refresh_queue_present(uint32_t index, const uint32_t *overlays, uint32_t *replaced);
int brain_refresh_wait_present(uint32_t index);
int brain_refresh_wait_overlay(uint32_t overlay, uint32_t buffer);
void brain_refresh_add_clean_time(uint32_t microseconds);
void brain_refresh_get_present_stats(BrainPresentStats *stats, int reset);

#endif
