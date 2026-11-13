/*
 * raster.h - Rasteriser surface and rectangle types and the drawing entry points.
 */
#ifndef BRAIN_RASTER_H
#define BRAIN_RASTER_H

#include <stddef.h>
#include <stdint.h>

enum raster_format {
    RASTER_FORMAT_1BPP = 1,
    RASTER_FORMAT_4BPP = 2,
    RASTER_FORMAT_8BPP = 3,
    RASTER_FORMAT_16BPP = 4,
    RASTER_FORMAT_24BPP = 5,
    RASTER_FORMAT_32BPP = 6,
    RASTER_FORMAT_2BPP = 9,
    RASTER_FORMAT_16BPP_555 = 20
};

typedef struct {
    void *bits;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
} raster_surface;

typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} raster_rect;

enum raster_xlate_kind {
    RASTER_XLATE_TRIVIAL = 0,
    RASTER_XLATE_TABLE = 1,
    RASTER_XLATE_RGB = 2,
    RASTER_XLATE_RGB_TO_INDEX = 3
};

typedef struct {
    int32_t kind;
    const uint32_t *table;
    uint32_t table_count;
    uint32_t src_red;
    uint32_t src_green;
    uint32_t src_blue;
    uint32_t dst_red;
    uint32_t dst_green;
    uint32_t dst_blue;
    int32_t prepared;
    uint8_t src_shift[3];
    uint8_t src_width[3];
    uint8_t dst_shift[3];
    uint8_t dst_width[3];
} raster_xlate;

typedef struct {
    int32_t is_pattern;
    uint32_t solid;
    raster_surface pattern;
    int32_t org_x;
    int32_t org_y;
} raster_brush;

enum raster_clip_kind {
    RASTER_CLIP_NONE = 0,
    RASTER_CLIP_RECT = 1,
    RASTER_CLIP_COMPLEX = 3
};

typedef struct {
    int32_t kind;
    raster_rect bounds;
    const raster_rect *rects;
    uint32_t rect_count;
    int non_overlapping;
} raster_clip;

uint32_t raster_get_pixel(const raster_surface *surface, int32_t x, int32_t y);
void raster_put_pixel(const raster_surface *surface, int32_t x, int32_t y, uint32_t value);
uint32_t raster_rop3(uint8_t rop, uint32_t dst, uint32_t src, uint32_t pattern);
uint32_t raster_rop2(uint8_t code, uint32_t dst, uint32_t pen);
uint32_t raster_translate(const raster_xlate *xlate, uint32_t src);
void raster_xlate_prepare(raster_xlate *xlate);
int raster_clip_contains(const raster_clip *clip, int32_t x, int32_t y);

void raster_line(const raster_surface *dst, const raster_clip *clip,
                 int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 uint32_t pen, uint8_t rop2);

void raster_anyblt(const raster_surface *dst, const raster_surface *src, const raster_surface *mask,
                   const raster_clip *clip, const raster_xlate *xlate,
                   const raster_rect *dst_rect, const raster_rect *src_rect,
                   int32_t mask_x, int32_t mask_y,
                   const raster_brush *brush, uint16_t rop4);

typedef struct {
    int32_t x;
    int32_t y;
} raster_point;

void raster_fill_polygon(const raster_surface *dst, const raster_clip *clip,
                         const raster_point *points, const int32_t *subpath_lengths,
                         int32_t subpath_count, const raster_brush *brush,
                         uint8_t rop2, int winding);

void raster_transparentblt(const raster_surface *dst, const raster_surface *src,
                           const raster_clip *clip, const raster_xlate *xlate,
                           const raster_rect *dst_rect, const raster_rect *src_rect,
                           uint32_t trans_colour);

void raster_normalise_rect(raster_rect *rect);

void raster_bitblt(const raster_surface *dst,
                   const raster_surface *src,
                   const raster_surface *mask,
                   const raster_clip *clip,
                   const raster_xlate *xlate,
                   const raster_rect *dst_rect,
                   int32_t src_x, int32_t src_y,
                   int32_t mask_x, int32_t mask_y,
                   const raster_brush *brush,
                   uint16_t rop4,
                   const uint32_t *dst_masks);

#endif
