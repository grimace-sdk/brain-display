#ifndef BRAIN_RASTER_REF_H
#define BRAIN_RASTER_REF_H

#include "../src/raster.h"

void raster_ref_bitblt(const raster_surface *dst,
                       const raster_surface *src,
                       const raster_surface *mask,
                       const raster_clip *clip,
                       const raster_xlate *xlate,
                       const raster_rect *dst_rect,
                       int32_t src_x, int32_t src_y,
                       int32_t mask_x, int32_t mask_y,
                       const raster_brush *brush,
                       uint16_t rop4);

#endif
