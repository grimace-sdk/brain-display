#include "raster_ref.h"

#include <stdlib.h>

static int32_t clamp_int(int32_t value, int32_t low, int32_t high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static int ref_clip_contains(const raster_clip *clip, int32_t x, int32_t y) {
    if (clip == NULL || clip->kind == RASTER_CLIP_NONE) {
        return 1;
    }
    if (x < clip->bounds.left || x >= clip->bounds.right ||
        y < clip->bounds.top || y >= clip->bounds.bottom) {
        return 0;
    }
    if (clip->kind == RASTER_CLIP_RECT) {
        return 1;
    }
    for (uint32_t index = 0; index < clip->rect_count; index++) {
        const raster_rect *r = &clip->rects[index];
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom) {
            return 1;
        }
    }
    return 0;
}

static uint32_t ref_brush_pixel(const raster_brush *brush, int32_t x, int32_t y) {
    if (brush == NULL) {
        return 0;
    }
    if (!brush->is_pattern) {
        return brush->solid;
    }
    int32_t px = (x - brush->org_x) % brush->pattern.width;
    int32_t py = (y - brush->org_y) % brush->pattern.height;
    if (px < 0) {
        px += brush->pattern.width;
    }
    if (py < 0) {
        py += brush->pattern.height;
    }
    return raster_get_pixel(&brush->pattern, px, py);
}

void raster_ref_bitblt(const raster_surface *dst,
                       const raster_surface *src,
                       const raster_surface *mask,
                       const raster_clip *clip,
                       const raster_xlate *xlate,
                       const raster_rect *dst_rect,
                       int32_t src_x, int32_t src_y,
                       int32_t mask_x, int32_t mask_y,
                       const raster_brush *brush,
                       uint16_t rop4) {
    uint8_t rop_foreground = (uint8_t)(rop4 & 0xFF);
    uint8_t rop_background = (uint8_t)(rop4 >> 8);

    raster_rect rect = *dst_rect;
    raster_normalise_rect(&rect);
    int32_t left = clamp_int(rect.left, 0, dst->width);
    int32_t top = clamp_int(rect.top, 0, dst->height);
    int32_t right = clamp_int(rect.right, 0, dst->width);
    int32_t bottom = clamp_int(rect.bottom, 0, dst->height);
    if (left >= right || top >= bottom) {
        return;
    }
    src_x += left - rect.left;
    src_y += top - rect.top;
    mask_x += left - rect.left;
    mask_y += top - rect.top;

    int32_t width = right - left;
    int32_t height = bottom - top;
    uint32_t *source_snapshot = malloc((size_t)width * height * sizeof(uint32_t));
    uint8_t *mask_snapshot = malloc((size_t)width * height);
    for (int32_t dy = 0; dy < height; dy++) {
        for (int32_t dx = 0; dx < width; dx++) {
            size_t index = (size_t)dy * width + dx;
            if (src != NULL) {
                source_snapshot[index] = raster_translate(xlate, raster_get_pixel(src, src_x + dx, src_y + dy));
            } else {
                source_snapshot[index] = 0;
            }
            if (mask != NULL) {
                mask_snapshot[index] = raster_get_pixel(mask, mask_x + dx, mask_y + dy) ? 1 : 0;
            } else {
                mask_snapshot[index] = 1;
            }
        }
    }

    for (int32_t dy = 0; dy < height; dy++) {
        for (int32_t dx = 0; dx < width; dx++) {
            int32_t x = left + dx;
            int32_t y = top + dy;
            if (!ref_clip_contains(clip, x, y)) {
                continue;
            }
            size_t index = (size_t)dy * width + dx;
            uint8_t rop = mask_snapshot[index] ? rop_foreground : rop_background;
            uint32_t dest_value = raster_get_pixel(dst, x, y);
            uint32_t pattern_value = ref_brush_pixel(brush, x, y);
            raster_put_pixel(dst, x, y, raster_rop3(rop, dest_value, source_snapshot[index], pattern_value));
        }
    }

    free(source_snapshot);
    free(mask_snapshot);
}
