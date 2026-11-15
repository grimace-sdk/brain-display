/*
 * pxp.h - PXP transpose interface and the overlay set type.
 */
#ifndef BRAIN_PXP_H
#define BRAIN_PXP_H

#include <stdint.h>

#include "raster.h"

#define BRAIN_PXP_MAX_OVERLAYS 8

typedef struct {
    uint32_t physical;
    uint32_t size;
    uint32_t parameters;
} brain_pxp_overlay;

typedef struct {
    uint32_t count;
    uint32_t colour_key_low;
    uint32_t colour_key_high;
    brain_pxp_overlay overlay[BRAIN_PXP_MAX_OVERLAYS];
} brain_pxp_overlay_set;

int brain_pxp_init(void);
void brain_pxp_resume(void);
int brain_pxp_available(void);
int brain_pxp_transpose(uint32_t src_phys, int32_t src_stride_px,
                        const raster_rect *rect, uint32_t dst_phys, int angle, int mirror);
int brain_pxp_transpose_overlays(uint32_t src_phys, int32_t src_stride_px,
                                 const raster_rect *rect, uint32_t dst_phys, int angle, int mirror,
                                 const brain_pxp_overlay_set *overlays);
void brain_pxp_path_counts(uint32_t *irq, uint32_t *spin);
void brain_pxp_set_flip_override(uint32_t value);

#endif
