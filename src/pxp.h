#ifndef BRAIN_PXP_H
#define BRAIN_PXP_H

#include <stdint.h>

#include "raster.h"

int brain_pxp_init(void);
int brain_pxp_available(void);
int brain_pxp_transpose(uint32_t src_phys, int32_t src_stride_px,
                        const raster_rect *rect, uint32_t dst_phys, int angle, int mirror);
void brain_pxp_path_counts(uint32_t *irq, uint32_t *spin);

#endif
