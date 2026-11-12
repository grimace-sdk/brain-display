#include "raster.h"
#include "cleartype_table.h"

static int32_t clamp_int(int32_t value, int32_t low, int32_t high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static uint8_t *row_address(const raster_surface *surface, int32_t y) {
    return (uint8_t *)surface->bits + (ptrdiff_t)y * surface->stride;
}

static uint32_t get_pixel_row(const uint8_t *row, int32_t format, int32_t x) {
    switch (format) {
    case RASTER_FORMAT_1BPP:
        return (row[x >> 3] >> (7 - (x & 7))) & 0x1;
    case RASTER_FORMAT_2BPP:
        return (row[x >> 2] >> (6 - 2 * (x & 3))) & 0x3;
    case RASTER_FORMAT_4BPP:
        return (x & 1) ? (row[x >> 1] & 0xF) : (row[x >> 1] >> 4);
    case RASTER_FORMAT_8BPP:
        return row[x];
    case RASTER_FORMAT_16BPP:
    case RASTER_FORMAT_16BPP_555:
        return ((const uint16_t *)row)[x];
    case RASTER_FORMAT_24BPP: {
        const uint8_t *pixel = row + (ptrdiff_t)x * 3;
        return (uint32_t)pixel[0] | ((uint32_t)pixel[1] << 8) | ((uint32_t)pixel[2] << 16);
    }
    case RASTER_FORMAT_32BPP:
        return ((const uint32_t *)row)[x];
    default:
        return 0;
    }
}

static void put_pixel_row(uint8_t *row, int32_t format, int32_t x, uint32_t value) {
    switch (format) {
    case RASTER_FORMAT_1BPP: {
        uint8_t shift = 7 - (x & 7);
        uint8_t *byte = &row[x >> 3];
        *byte = (uint8_t)((*byte & ~(1u << shift)) | ((value & 0x1) << shift));
        break;
    }
    case RASTER_FORMAT_2BPP: {
        uint8_t shift = 6 - 2 * (x & 3);
        uint8_t *byte = &row[x >> 2];
        *byte = (uint8_t)((*byte & ~(0x3u << shift)) | ((value & 0x3) << shift));
        break;
    }
    case RASTER_FORMAT_4BPP: {
        uint8_t *byte = &row[x >> 1];
        if (x & 1) {
            *byte = (uint8_t)((*byte & 0xF0) | (value & 0xF));
        } else {
            *byte = (uint8_t)((*byte & 0x0F) | ((value & 0xF) << 4));
        }
        break;
    }
    case RASTER_FORMAT_8BPP:
        row[x] = (uint8_t)value;
        break;
    case RASTER_FORMAT_16BPP:
    case RASTER_FORMAT_16BPP_555:
        ((uint16_t *)row)[x] = (uint16_t)value;
        break;
    case RASTER_FORMAT_24BPP: {
        uint8_t *pixel = row + (ptrdiff_t)x * 3;
        pixel[0] = (uint8_t)value;
        pixel[1] = (uint8_t)(value >> 8);
        pixel[2] = (uint8_t)(value >> 16);
        break;
    }
    case RASTER_FORMAT_32BPP:
        ((uint32_t *)row)[x] = value;
        break;
    default:
        break;
    }
}

uint32_t raster_get_pixel(const raster_surface *surface, int32_t x, int32_t y) {
    return get_pixel_row(row_address(surface, y), surface->format, x);
}

void raster_put_pixel(const raster_surface *surface, int32_t x, int32_t y, uint32_t value) {
    put_pixel_row(row_address(surface, y), surface->format, x, value);
}

uint32_t raster_rop3(uint8_t rop, uint32_t dst, uint32_t src, uint32_t pattern) {
    uint32_t result = 0;
    if (rop & 0x01) result |= ~pattern & ~src & ~dst;
    if (rop & 0x02) result |= ~pattern & ~src & dst;
    if (rop & 0x04) result |= ~pattern & src & ~dst;
    if (rop & 0x08) result |= ~pattern & src & dst;
    if (rop & 0x10) result |= pattern & ~src & ~dst;
    if (rop & 0x20) result |= pattern & ~src & dst;
    if (rop & 0x40) result |= pattern & src & ~dst;
    if (rop & 0x80) result |= pattern & src & dst;
    return result;
}

uint32_t raster_rop2(uint8_t code, uint32_t dst, uint32_t pen) {
    uint32_t table = (uint32_t)(code - 1);
    uint32_t result = 0;
    if (table & 1) result |= ~pen & ~dst;
    if (table & 2) result |= ~pen & dst;
    if (table & 4) result |= pen & ~dst;
    if (table & 8) result |= pen & dst;
    return result;
}

static uint32_t scale_channel(uint32_t value, uint32_t from_mask, uint32_t to_mask) {
    if (from_mask == 0 || to_mask == 0) {
        return 0;
    }
    uint32_t from_shift = 0;
    while (((from_mask >> from_shift) & 1) == 0) {
        from_shift++;
    }
    uint32_t to_shift = 0;
    while (((to_mask >> to_shift) & 1) == 0) {
        to_shift++;
    }
    uint32_t from_width = 0;
    while ((from_mask >> (from_shift + from_width)) & 1) {
        from_width++;
    }
    uint32_t to_width = 0;
    while ((to_mask >> (to_shift + to_width)) & 1) {
        to_width++;
    }
    uint32_t channel = (value & from_mask) >> from_shift;
    uint32_t scaled;
    if (from_width >= to_width) {
        scaled = channel >> (from_width - to_width);
    } else {
        uint32_t extra = to_width - from_width;
        uint32_t low = (from_width > extra) ? (channel >> (from_width - extra)) : 0;
        scaled = (channel << extra) | low;
    }
    return (scaled << to_shift) & to_mask;
}

static uint32_t scale_prepared(uint32_t value, uint32_t from_mask, uint32_t from_shift,
                               uint32_t from_width, uint32_t to_shift, uint32_t to_width,
                               uint32_t to_mask) {
    if (from_width == 0 || to_width == 0) {
        return 0;
    }
    uint32_t channel = (value & from_mask) >> from_shift;
    uint32_t scaled;
    if (from_width >= to_width) {
        scaled = channel >> (from_width - to_width);
    } else {
        uint32_t extra = to_width - from_width;
        uint32_t low = (from_width > extra) ? (channel >> (from_width - extra)) : 0;
        scaled = (channel << extra) | low;
    }
    return (scaled << to_shift) & to_mask;
}

uint32_t raster_translate(const raster_xlate *xlate, uint32_t src) {
    if (xlate == NULL || xlate->kind == RASTER_XLATE_TRIVIAL) {
        return src;
    }
    if (xlate->kind == RASTER_XLATE_TABLE) {
        if (xlate->table != NULL && src < xlate->table_count) {
            return xlate->table[src];
        }
        return 0;
    }
    if (xlate->kind == RASTER_XLATE_RGB_TO_INDEX) {
        if (xlate->table == NULL || xlate->table_count == 0) {
            return 0;
        }
        int32_t r = (int32_t)(scale_channel(src, xlate->src_red, 0xFF));
        int32_t g = (int32_t)(scale_channel(src, xlate->src_green, 0xFF));
        int32_t b = (int32_t)(scale_channel(src, xlate->src_blue, 0xFF));
        uint32_t best = 0;
        int32_t best_distance = 0x7FFFFFFF;
        for (uint32_t index = 0; index < xlate->table_count; index++) {
            uint32_t entry = xlate->table[index];
            int32_t dr = r - (int32_t)(entry & 0xFF);
            int32_t dg = g - (int32_t)((entry >> 8) & 0xFF);
            int32_t db = b - (int32_t)((entry >> 16) & 0xFF);
            int32_t distance = dr * dr + dg * dg + db * db;
            if (distance < best_distance) {
                best_distance = distance;
                best = index;
            }
        }
        return best;
    }
    if (xlate->prepared) {
        return scale_prepared(src, xlate->src_red, xlate->src_shift[0], xlate->src_width[0],
                              xlate->dst_shift[0], xlate->dst_width[0], xlate->dst_red) |
               scale_prepared(src, xlate->src_green, xlate->src_shift[1], xlate->src_width[1],
                              xlate->dst_shift[1], xlate->dst_width[1], xlate->dst_green) |
               scale_prepared(src, xlate->src_blue, xlate->src_shift[2], xlate->src_width[2],
                              xlate->dst_shift[2], xlate->dst_width[2], xlate->dst_blue);
    }
    return scale_channel(src, xlate->src_red, xlate->dst_red) |
           scale_channel(src, xlate->src_green, xlate->dst_green) |
           scale_channel(src, xlate->src_blue, xlate->dst_blue);
}

void raster_normalise_rect(raster_rect *rect) {
    if (rect->left > rect->right) {
        int32_t swap = rect->left;
        rect->left = rect->right;
        rect->right = swap;
    }
    if (rect->top > rect->bottom) {
        int32_t swap = rect->top;
        rect->top = rect->bottom;
        rect->bottom = swap;
    }
}

static int clip_contains(const raster_clip *clip, int32_t x, int32_t y) {
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

int raster_clip_contains(const raster_clip *clip, int32_t x, int32_t y) {
    return clip_contains(clip, x, y);
}

static uint32_t brush_pixel(const raster_brush *brush, int32_t x, int32_t y) {
    if (brush == NULL) {
        return 0;
    }
    if (!brush->is_pattern) {
        return brush->solid;
    }
    int32_t width = brush->pattern.width;
    int32_t height = brush->pattern.height;
    int32_t px = (x - brush->org_x) % width;
    int32_t py = (y - brush->org_y) % height;
    if (px < 0) {
        px += width;
    }
    if (py < 0) {
        py += height;
    }
    return raster_get_pixel(&brush->pattern, px, py);
}

typedef struct {
    uint32_t mask;
    uint32_t shift;
    uint32_t width;
} chan_t;

static chan_t chan_make(uint32_t mask) {
    chan_t c;
    c.mask = mask;
    c.shift = 0;
    c.width = 0;
    if (mask != 0) {
        while (((mask >> c.shift) & 1) == 0) {
            c.shift++;
        }
        while ((mask >> (c.shift + c.width)) & 1) {
            c.width++;
        }
    }
    return c;
}

void raster_xlate_prepare(raster_xlate *xlate) {
    xlate->prepared = 0;
    if (xlate->kind != RASTER_XLATE_RGB) {
        return;
    }
    uint32_t src_masks[3] = { xlate->src_red, xlate->src_green, xlate->src_blue };
    uint32_t dst_masks[3] = { xlate->dst_red, xlate->dst_green, xlate->dst_blue };
    for (int i = 0; i < 3; i++) {
        chan_t s = chan_make(src_masks[i]);
        chan_t d = chan_make(dst_masks[i]);
        xlate->src_shift[i] = (uint8_t)s.shift;
        xlate->src_width[i] = (uint8_t)s.width;
        xlate->dst_shift[i] = (uint8_t)d.shift;
        xlate->dst_width[i] = (uint8_t)d.width;
    }
    xlate->prepared = 1;
}

static uint32_t chan_to8(uint32_t value, chan_t c) {
    if (c.mask == 0) {
        return 0;
    }
    uint32_t channel = (value & c.mask) >> c.shift;
    if (c.width >= 8) {
        return channel >> (c.width - 8);
    }
    uint32_t extra = 8 - c.width;
    uint32_t low = (c.width > extra) ? (channel >> (c.width - extra)) : 0;
    return (channel << extra) | low;
}

static uint32_t chan_from8(uint32_t value8, chan_t c) {
    if (c.mask == 0) {
        return 0;
    }
    uint32_t channel = value8 & 0xFF;
    uint32_t scaled;
    if (8 >= c.width) {
        scaled = channel >> (8 - c.width);
    } else {
        uint32_t extra = c.width - 8;
        uint32_t low = (8 > extra) ? (channel >> (8 - extra)) : 0;
        scaled = (channel << extra) | low;
    }
    return (scaled << c.shift) & c.mask;
}

static int32_t gamma_blend(int32_t bg, int32_t fg, int32_t level) {
    uint32_t mix = ((uint32_t)(6 - level) * brain_gamma_fwd[bg] +
                    (uint32_t)level * brain_gamma_fwd[fg] + 3) / 6;
    if (mix > 255) {
        mix = 255;
    }
    return brain_gamma_inv[mix];
}

static uint32_t blend_cleartype(uint32_t bg, uint32_t fg, uint32_t index,
                                chan_t cr, chan_t cg, chan_t cb) {
    if (index > 114) {
        index = 114;
    }
    const uint8_t *level = brain_cleartype_levels[index];
    int32_t r = gamma_blend((int32_t)chan_to8(bg, cr), (int32_t)chan_to8(fg, cr), level[0]);
    int32_t g = gamma_blend((int32_t)chan_to8(bg, cg), (int32_t)chan_to8(fg, cg), level[1]);
    int32_t b = gamma_blend((int32_t)chan_to8(bg, cb), (int32_t)chan_to8(fg, cb), level[2]);
    return chan_from8((uint32_t)r, cr) | chan_from8((uint32_t)g, cg) | chan_from8((uint32_t)b, cb);
}

static uint32_t blend_coverage(uint32_t bg, uint32_t fg, uint32_t coverage,
                               chan_t cr, chan_t cg, chan_t cb) {
    int32_t c = (int32_t)coverage;
    int32_t br = (int32_t)chan_to8(bg, cr), fr = (int32_t)chan_to8(fg, cr);
    int32_t bgn = (int32_t)chan_to8(bg, cg), fgn = (int32_t)chan_to8(fg, cg);
    int32_t bb = (int32_t)chan_to8(bg, cb), fb = (int32_t)chan_to8(fg, cb);
    int32_t r = br + (fr - br) * c / 255;
    int32_t g = bgn + (fgn - bgn) * c / 255;
    int32_t b = bb + (fb - bb) * c / 255;
    return chan_from8((uint32_t)r, cr) | chan_from8((uint32_t)g, cg) | chan_from8((uint32_t)b, cb);
}

static int format_is_rgb(int32_t format) {
    return format == RASTER_FORMAT_16BPP || format == RASTER_FORMAT_16BPP_555 ||
           format == RASTER_FORMAT_24BPP || format == RASTER_FORMAT_32BPP;
}

static void bitblt_generic(const raster_surface *dst,
                           const raster_surface *src,
                           const raster_surface *mask,
                           const raster_clip *clip,
                           const raster_xlate *xlate,
                           const raster_rect *dst_rect,
                           int32_t src_x, int32_t src_y,
                           int32_t mask_x, int32_t mask_y,
                           const raster_brush *brush,
                           uint16_t rop4,
                           const uint32_t *dst_masks) {
    uint8_t rop_foreground = (uint8_t)(rop4 & 0xFF);
    uint8_t rop_background = (uint8_t)(rop4 >> 8);
    uint32_t rmask = dst_masks ? dst_masks[0] : 0xF800;
    uint32_t gmask = dst_masks ? dst_masks[1] : 0x07E0;
    uint32_t bmask = dst_masks ? dst_masks[2] : 0x001F;
    int blend_ok = format_is_rgb(dst->format) && rmask && gmask && bmask;
    chan_t cr = chan_make(rmask);
    chan_t cg = chan_make(gmask);
    chan_t cb = chan_make(bmask);
    int32_t left = clamp_int(dst_rect->left, 0, dst->width);
    int32_t top = clamp_int(dst_rect->top, 0, dst->height);
    int32_t right = clamp_int(dst_rect->right, 0, dst->width);
    int32_t bottom = clamp_int(dst_rect->bottom, 0, dst->height);

    int32_t reverse_x = 0;
    int32_t reverse_y = 0;
    if (src != NULL && src->bits == dst->bits) {
        if (src_x < dst_rect->left) {
            reverse_x = 1;
        }
        if (src_y < dst_rect->top) {
            reverse_y = 1;
        }
    }

    int32_t dst_format = dst->format;
    int32_t src_format = src != NULL ? src->format : 0;
    int32_t mask_format = mask != NULL ? mask->format : 0;
    int clip_needed = !(clip == NULL || clip->kind == RASTER_CLIP_NONE);

    for (int32_t row = 0; row < bottom - top; row++) {
        int32_t y = reverse_y ? (bottom - 1 - row) : (top + row);
        int32_t offset_y = y - dst_rect->top;
        uint8_t *dst_row = row_address(dst, y);
        const uint8_t *src_row = src != NULL ? row_address(src, src_y + offset_y) : NULL;
        const uint8_t *mask_row = mask != NULL ? row_address(mask, mask_y + offset_y) : NULL;
        for (int32_t column = 0; column < right - left; column++) {
            int32_t x = reverse_x ? (right - 1 - column) : (left + column);
            if (clip_needed && !clip_contains(clip, x, y)) {
                continue;
            }
            int32_t offset_x = x - dst_rect->left;

            uint32_t source_value = 0;
            if (src != NULL) {
                source_value = raster_translate(xlate, get_pixel_row(src_row, src_format, src_x + offset_x));
            }
            uint32_t pattern_value = brush_pixel(brush, x, y);
            uint32_t dest_value = get_pixel_row(dst_row, dst_format, x);

            if (mask != NULL && blend_ok &&
                (mask_format == RASTER_FORMAT_4BPP || mask_format == RASTER_FORMAT_8BPP)) {
                uint32_t mval = get_pixel_row(mask_row, mask_format, mask_x + offset_x);
                uint32_t fg = raster_rop3(rop_foreground, dest_value, source_value, pattern_value);
                uint32_t bg = raster_rop3(rop_background, dest_value, source_value, pattern_value);
                uint32_t blended = (mask_format == RASTER_FORMAT_8BPP)
                    ? blend_cleartype(bg, fg, mval, cr, cg, cb)
                    : blend_coverage(bg, fg, mval * 17, cr, cg, cb);
                put_pixel_row(dst_row, dst_format, x, blended);
                continue;
            }

            uint32_t mask_bit = 1;
            if (mask != NULL) {
                mask_bit = get_pixel_row(mask_row, mask_format, mask_x + offset_x) ? 1 : 0;
            }
            uint8_t rop = mask_bit ? rop_foreground : rop_background;
            put_pixel_row(dst_row, dst_format, x, raster_rop3(rop, dest_value, source_value, pattern_value));
        }
    }
}

static int clip_is_plain(const raster_clip *clip, const raster_rect *rect) {
    if (clip == NULL || clip->kind == RASTER_CLIP_NONE) {
        return 1;
    }
    if (clip->kind == RASTER_CLIP_RECT) {
        return rect->left >= clip->bounds.left && rect->right <= clip->bounds.right &&
               rect->top >= clip->bounds.top && rect->bottom <= clip->bounds.bottom;
    }
    return 0;
}

static int fill_solid_16(const raster_surface *dst, const raster_rect *rect, uint16_t value) {
    uint32_t doubled = (uint32_t)value | ((uint32_t)value << 16);
    for (int32_t y = rect->top; y < rect->bottom; y++) {
        uint16_t *pixels = (uint16_t *)row_address(dst, y) + rect->left;
        int32_t count = rect->right - rect->left;
        int32_t index = 0;
        if (((uintptr_t)pixels & 0x3) && count > 0) {
            *pixels++ = value;
            index = 1;
        }
        uint32_t *pair = (uint32_t *)pixels;
        int32_t pairs = (count - index) / 2;
        for (int32_t p = 0; p < pairs; p++) {
            pair[p] = doubled;
        }
        index += pairs * 2;
        if (index < count) {
            pixels[pairs * 2] = value;
        }
    }
    return 1;
}

static void copy_row_16(uint16_t *dest, const uint16_t *source, int32_t count) {
    if (count <= 0) {
        return;
    }
    if ((((uintptr_t)dest ^ (uintptr_t)source) & 2u) == 0) {
        if (((uintptr_t)dest & 2u) != 0) {
            *dest++ = *source++;
            count--;
        }
        uint32_t *d32 = (uint32_t *)dest;
        const uint32_t *s32 = (const uint32_t *)source;
        int32_t words = count >> 1;
        for (int32_t i = 0; i < words; i++) {
            d32[i] = s32[i];
        }
        if (count & 1) {
            dest[words << 1] = source[words << 1];
        }
    } else {
        for (int32_t i = 0; i < count; i++) {
            dest[i] = source[i];
        }
    }
}

static int copy_16(const raster_surface *dst, const raster_surface *src,
                   const raster_rect *rect, int32_t src_x, int32_t src_y) {
    int32_t count = rect->right - rect->left;
    int32_t reverse_y = (src->bits == dst->bits && src_y < rect->top);
    for (int32_t row = 0; row < rect->bottom - rect->top; row++) {
        int32_t dy = reverse_y ? (rect->bottom - 1 - row) : (rect->top + row);
        int32_t sy = src_y + (dy - rect->top);
        uint16_t *dest = (uint16_t *)row_address(dst, dy) + rect->left;
        uint16_t *source = (uint16_t *)row_address(src, sy) + src_x;
        if (src->bits == dst->bits && src_x < rect->left) {
            for (int32_t i = count - 1; i >= 0; i--) {
                dest[i] = source[i];
            }
        } else {
            copy_row_16(dest, source, count);
        }
    }
    return 1;
}

static int or_16(const raster_surface *dst, const raster_surface *src,
                 const raster_rect *rect, int32_t src_x, int32_t src_y) {
    int32_t count = rect->right - rect->left;
    for (int32_t row = 0; row < rect->bottom - rect->top; row++) {
        uint16_t *dest = (uint16_t *)row_address(dst, rect->top + row) + rect->left;
        const uint16_t *source = (const uint16_t *)row_address(src, src_y + row) + src_x;
        for (int32_t i = 0; i < count; i++) {
            dest[i] |= source[i];
        }
    }
    return 1;
}

static int copy_indexed_16(const raster_surface *dst, const raster_surface *src,
                           const raster_rect *rect, int32_t src_x, int32_t src_y,
                           const raster_xlate *xlate) {
    int32_t width = rect->right - rect->left;
    int32_t sfmt = src->format;
    const uint32_t *table = xlate->table;
    uint32_t count = xlate->table_count;
    for (int32_t row = 0; row < rect->bottom - rect->top; row++) {
        uint16_t *dest = (uint16_t *)row_address(dst, rect->top + row) + rect->left;
        const uint8_t *srow = row_address(src, src_y + row);
        for (int32_t x = 0; x < width; x++) {
            uint32_t idx = get_pixel_row(srow, sfmt, src_x + x);
            dest[x] = (uint16_t)(idx < count ? table[idx] : 0);
        }
    }
    return 1;
}

static int masked_copy_16(const raster_surface *dst, const raster_surface *src,
                          const raster_surface *mask, const raster_rect *rect,
                          int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y) {
    int32_t width = rect->right - rect->left;
    for (int32_t row = 0; row < rect->bottom - rect->top; row++) {
        uint16_t *dest = (uint16_t *)row_address(dst, rect->top + row) + rect->left;
        const uint16_t *source = (const uint16_t *)row_address(src, src_y + row) + src_x;
        const uint8_t *mrow = row_address(mask, mask_y + row);
        int32_t x = 0;
        while (x < width) {
            int32_t bit = mask_x + x;
            if ((bit & 7) == 0 && (width - x) >= 8) {
                uint8_t mb = mrow[bit >> 3];
                if (mb == 0xFF) {
                    for (int32_t k = 0; k < 8; k++) {
                        dest[x + k] = source[x + k];
                    }
                } else if (mb != 0x00) {
                    for (int32_t k = 0; k < 8; k++) {
                        if ((mb >> (7 - k)) & 1) {
                            dest[x + k] = source[x + k];
                        }
                    }
                }
                x += 8;
            } else {
                if ((mrow[bit >> 3] >> (7 - (bit & 7))) & 1) {
                    dest[x] = source[x];
                }
                x++;
            }
        }
    }
    return 1;
}

static int and_indexed_16(const raster_surface *dst, const raster_surface *src,
                          const raster_rect *rect, int32_t src_x, int32_t src_y,
                          const raster_xlate *xlate) {
    int32_t width = rect->right - rect->left;
    int32_t sfmt = src->format;
    const uint32_t *table = xlate->table;
    uint32_t count = xlate->table_count;
    for (int32_t row = 0; row < rect->bottom - rect->top; row++) {
        uint16_t *dest = (uint16_t *)row_address(dst, rect->top + row) + rect->left;
        const uint8_t *srow = row_address(src, src_y + row);
        for (int32_t x = 0; x < width; x++) {
            uint32_t idx = get_pixel_row(srow, sfmt, src_x + x);
            dest[x] &= (uint16_t)(idx < count ? table[idx] : 0);
        }
    }
    return 1;
}

static int solid_mask_16(const raster_surface *dst, const raster_surface *mask,
                         const raster_rect *rect, int32_t mask_x, int32_t mask_y,
                         uint16_t solid, uint8_t fg_rop, uint8_t bg_rop) {
    int fg_solid = (fg_rop == 0xF0);
    int bg_keep = (bg_rop == 0xAA);
    int32_t width = rect->right - rect->left;
    for (int32_t y = rect->top; y < rect->bottom; y++) {
        uint16_t *dest = (uint16_t *)row_address(dst, y) + rect->left;
        const uint8_t *mrow = row_address(mask, mask_y + (y - rect->top));
        for (int32_t x = 0; x < width; x++) {
            int32_t mx = mask_x + x;
            int bit = (mrow[mx >> 3] >> (7 - (mx & 7))) & 1;
            if (bit) {
                dest[x] = fg_solid ? solid : (uint16_t)raster_rop3(fg_rop, dest[x], 0, solid);
            } else if (!bg_keep) {
                dest[x] = (uint16_t)raster_rop3(bg_rop, dest[x], 0, solid);
            }
        }
    }
    return 1;
}

static void fill_blend_table(uint16_t table[7][64], chan_t c, uint32_t solid8) {
    uint32_t values = 1u << c.width;
    for (int level = 0; level <= 6; level++) {
        for (uint32_t raw = 0; raw < values; raw++) {
            uint32_t dest8 = chan_to8(raw << c.shift, c);
            uint32_t blended = (uint32_t)gamma_blend((int32_t)dest8, (int32_t)solid8, level);
            table[level][raw] = (uint16_t)chan_from8(blended, c);
        }
    }
}

static int solid_blend_mask_16(const raster_surface *dst, const raster_surface *mask,
                               const raster_rect *rect, int32_t mask_x, int32_t mask_y,
                               uint16_t solid, chan_t cr, chan_t cg, chan_t cb) {
    uint16_t table_r[7][64];
    uint16_t table_g[7][64];
    uint16_t table_b[7][64];
    fill_blend_table(table_r, cr, chan_to8(solid, cr));
    fill_blend_table(table_g, cg, chan_to8(solid, cg));
    fill_blend_table(table_b, cb, chan_to8(solid, cb));

    int32_t width = rect->right - rect->left;
    uint32_t last_mval = 0xFFFFFFFF;
    uint16_t last_d = 0;
    uint16_t last_out = 0;
    for (int32_t y = rect->top; y < rect->bottom; y++) {
        uint16_t *dest = (uint16_t *)row_address(dst, y) + rect->left;
        const uint8_t *mrow = row_address(mask, mask_y + (y - rect->top)) + mask_x;
        for (int32_t x = 0; x < width; x++) {
            uint32_t mval = mrow[x];
            if (mval > 114) {
                mval = 114;
            }
            uint16_t d = dest[x];
            if (mval == last_mval && d == last_d) {
                dest[x] = last_out;
                continue;
            }
            const uint8_t *level = brain_cleartype_levels[mval];
            uint16_t out = table_r[level[0]][(d & cr.mask) >> cr.shift] |
                           table_g[level[1]][(d & cg.mask) >> cg.shift] |
                           table_b[level[2]][(d & cb.mask) >> cb.shift];
            dest[x] = out;
            last_mval = mval;
            last_d = d;
            last_out = out;
        }
    }
    return 1;
}

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
                   const uint32_t *dst_masks) {
    raster_rect rect = *dst_rect;
    raster_normalise_rect(&rect);
    int32_t clamped_left = clamp_int(rect.left, 0, dst->width);
    int32_t clamped_top = clamp_int(rect.top, 0, dst->height);
    int32_t clamped_right = clamp_int(rect.right, 0, dst->width);
    int32_t clamped_bottom = clamp_int(rect.bottom, 0, dst->height);
    if (clamped_left >= clamped_right || clamped_top >= clamped_bottom) {
        return;
    }
    src_x += clamped_left - rect.left;
    src_y += clamped_top - rect.top;
    mask_x += clamped_left - rect.left;
    mask_y += clamped_top - rect.top;
    rect.left = clamped_left;
    rect.top = clamped_top;
    rect.right = clamped_right;
    rect.bottom = clamped_bottom;

    raster_clip clip_none;
    if (clip != NULL && clip->kind == RASTER_CLIP_RECT) {
        int32_t nl = rect.left > clip->bounds.left ? rect.left : clip->bounds.left;
        int32_t nt = rect.top > clip->bounds.top ? rect.top : clip->bounds.top;
        int32_t nr = rect.right < clip->bounds.right ? rect.right : clip->bounds.right;
        int32_t nb = rect.bottom < clip->bounds.bottom ? rect.bottom : clip->bounds.bottom;
        if (nl >= nr || nt >= nb) {
            return;
        }
        src_x += nl - rect.left;
        src_y += nt - rect.top;
        mask_x += nl - rect.left;
        mask_y += nt - rect.top;
        rect.left = nl;
        rect.top = nt;
        rect.right = nr;
        rect.bottom = nb;
        clip_none.kind = RASTER_CLIP_NONE;
        clip_none.rects = NULL;
        clip_none.rect_count = 0;
        clip_none.non_overlapping = 0;
        clip = &clip_none;
    }

    if (clip != NULL && clip->kind == RASTER_CLIP_COMPLEX && clip->non_overlapping &&
        clip->rects != NULL && (src == NULL || src->bits != dst->bits)) {
        raster_clip none;
        none.kind = RASTER_CLIP_NONE;
        none.bounds = rect;
        none.rects = NULL;
        none.rect_count = 0;
        none.non_overlapping = 0;
        for (uint32_t i = 0; i < clip->rect_count; i++) {
            raster_rect sub;
            sub.left = rect.left > clip->rects[i].left ? rect.left : clip->rects[i].left;
            sub.top = rect.top > clip->rects[i].top ? rect.top : clip->rects[i].top;
            sub.right = rect.right < clip->rects[i].right ? rect.right : clip->rects[i].right;
            sub.bottom = rect.bottom < clip->rects[i].bottom ? rect.bottom : clip->rects[i].bottom;
            if (sub.left >= sub.right || sub.top >= sub.bottom) {
                continue;
            }
            int32_t dx = sub.left - rect.left;
            int32_t dy = sub.top - rect.top;
            raster_bitblt(dst, src, mask, &none, xlate, &sub,
                          src_x + dx, src_y + dy, mask_x + dx, mask_y + dy,
                          brush, rop4, dst_masks);
        }
        return;
    }

    int plain_clip = clip_is_plain(clip, &rect);
    int solid_brush = brush != NULL && !brush->is_pattern;

    if (dst->format == RASTER_FORMAT_16BPP && plain_clip) {
        if (src == NULL && mask == NULL && solid_brush && rop4 == 0xF0F0) {
            fill_solid_16(dst, &rect, (uint16_t)brush->solid);
            return;
        }
        if (src != NULL && mask == NULL && src->format == RASTER_FORMAT_16BPP &&
            rop4 == 0xCCCC && (xlate == NULL || xlate->kind == RASTER_XLATE_TRIVIAL)) {
            copy_16(dst, src, &rect, src_x, src_y);
            return;
        }
        if (src != NULL && mask == NULL && src->format == RASTER_FORMAT_16BPP &&
            rop4 == 0xEEEE && (xlate == NULL || xlate->kind == RASTER_XLATE_TRIVIAL)) {
            or_16(dst, src, &rect, src_x, src_y);
            return;
        }
        if (src != NULL && mask == NULL && rop4 == 0xCCCC && src->bits != dst->bits &&
            xlate != NULL && xlate->kind == RASTER_XLATE_TABLE && xlate->table != NULL) {
            copy_indexed_16(dst, src, &rect, src_x, src_y, xlate);
            return;
        }
        if (src != NULL && mask == NULL && rop4 == 0x8888 && src->bits != dst->bits &&
            xlate != NULL && xlate->kind == RASTER_XLATE_TABLE && xlate->table != NULL) {
            and_indexed_16(dst, src, &rect, src_x, src_y, xlate);
            return;
        }
        if (src != NULL && mask != NULL && rop4 == 0xAACC &&
            src->format == RASTER_FORMAT_16BPP && mask->format == RASTER_FORMAT_1BPP &&
            src->bits != dst->bits &&
            (xlate == NULL || xlate->kind == RASTER_XLATE_TRIVIAL)) {
            masked_copy_16(dst, src, mask, &rect, src_x, src_y, mask_x, mask_y);
            return;
        }
        if (src == NULL && mask != NULL && mask->format == RASTER_FORMAT_1BPP && solid_brush) {
            solid_mask_16(dst, mask, &rect, mask_x, mask_y, (uint16_t)brush->solid,
                          (uint8_t)(rop4 & 0xFF), (uint8_t)(rop4 >> 8));
            return;
        }
        if (src == NULL && mask != NULL && mask->format == RASTER_FORMAT_8BPP && solid_brush &&
            rop4 == 0xAAF0) {
            uint32_t rm = dst_masks ? dst_masks[0] : 0xF800;
            uint32_t gm = dst_masks ? dst_masks[1] : 0x07E0;
            uint32_t bm = dst_masks ? dst_masks[2] : 0x001F;
            chan_t cr = chan_make(rm);
            chan_t cg = chan_make(gm);
            chan_t cb = chan_make(bm);
            if (cr.width >= 1 && cr.width <= 6 && cg.width >= 1 && cg.width <= 6 &&
                cb.width >= 1 && cb.width <= 6) {
                solid_blend_mask_16(dst, mask, &rect, mask_x, mask_y, (uint16_t)brush->solid,
                                    cr, cg, cb);
                return;
            }
        }
    }

    bitblt_generic(dst, src, mask, clip, xlate, &rect, src_x, src_y, mask_x, mask_y, brush, rop4, dst_masks);
}

static int32_t abs_int(int32_t value) {
    return value < 0 ? -value : value;
}

#define RASTER_MAX_CROSSINGS 128

void raster_fill_polygon(const raster_surface *dst, const raster_clip *clip,
                         const raster_point *points, const int32_t *subpath_lengths,
                         int32_t subpath_count, const raster_brush *brush,
                         uint8_t rop2, int winding) {
    int32_t min_y = dst->height;
    int32_t max_y = 0;
    int32_t total = 0;
    for (int32_t s = 0; s < subpath_count; s++) {
        for (int32_t i = 0; i < subpath_lengths[s]; i++) {
            int32_t y = points[total + i].y;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
        total += subpath_lengths[s];
    }
    if (min_y < 0) min_y = 0;
    if (max_y > dst->height) max_y = dst->height;

    int clip_needed = !(clip == NULL || clip->kind == RASTER_CLIP_NONE);
    int clip_rect = (clip != NULL && clip->kind == RASTER_CLIP_RECT);
    int32_t dst_format = dst->format;
    int solid_span = (rop2 == 13) && brush != NULL && !brush->is_pattern &&
                     dst_format == RASTER_FORMAT_16BPP &&
                     (!clip_needed || clip_rect);
    uint16_t solid_value = brush != NULL ? (uint16_t)brush->solid : 0;

    for (int32_t y = min_y; y < max_y; y++) {
        if (solid_span && clip_rect &&
            (y < clip->bounds.top || y >= clip->bounds.bottom)) {
            continue;
        }
        uint8_t *dst_row = row_address(dst, y);
        int32_t cross_x[RASTER_MAX_CROSSINGS];
        int32_t cross_dir[RASTER_MAX_CROSSINGS];
        int32_t count = 0;
        int32_t base = 0;
        for (int32_t s = 0; s < subpath_count; s++) {
            int32_t n = subpath_lengths[s];
            for (int32_t i = 0; i < n; i++) {
                const raster_point *a = &points[base + i];
                const raster_point *b = &points[base + ((i + 1) % n)];
                int32_t y0 = a->y;
                int32_t y1 = b->y;
                if (y0 == y1) {
                    continue;
                }
                int32_t ylo = y0 < y1 ? y0 : y1;
                int32_t yhi = y0 < y1 ? y1 : y0;
                if (y < ylo || y >= yhi) {
                    continue;
                }
                if (count < RASTER_MAX_CROSSINGS) {
                    int32_t x = a->x + (y - y0) * (b->x - a->x) / (y1 - y0);
                    cross_x[count] = x;
                    cross_dir[count] = (y1 > y0) ? 1 : -1;
                    count++;
                }
            }
            base += n;
        }
        for (int32_t i = 1; i < count; i++) {
            int32_t kx = cross_x[i];
            int32_t kd = cross_dir[i];
            int32_t j = i - 1;
            while (j >= 0 && cross_x[j] > kx) {
                cross_x[j + 1] = cross_x[j];
                cross_dir[j + 1] = cross_dir[j];
                j--;
            }
            cross_x[j + 1] = kx;
            cross_dir[j + 1] = kd;
        }

        int32_t wind = 0;
        for (int32_t i = 0; i + 1 < count; i++) {
            int inside;
            if (winding) {
                wind += cross_dir[i];
                inside = (wind != 0);
            } else {
                inside = ((i & 1) == 0);
            }
            if (!inside) {
                continue;
            }
            int32_t xa = cross_x[i];
            int32_t xb = cross_x[i + 1];
            if (xa < 0) xa = 0;
            if (xb > dst->width) xb = dst->width;
            if (solid_span) {
                if (clip_rect) {
                    if (xa < clip->bounds.left) xa = clip->bounds.left;
                    if (xb > clip->bounds.right) xb = clip->bounds.right;
                }
                uint16_t *row16 = (uint16_t *)dst_row;
                for (int32_t x = xa; x < xb; x++) {
                    row16[x] = solid_value;
                }
                continue;
            }
            for (int32_t x = xa; x < xb; x++) {
                if (clip_needed && !clip_contains(clip, x, y)) {
                    continue;
                }
                uint32_t dest_value = get_pixel_row(dst_row, dst_format, x);
                uint32_t pattern_value = brush_pixel(brush, x, y);
                put_pixel_row(dst_row, dst_format, x, raster_rop2(rop2, dest_value, pattern_value));
            }
        }
    }
}

static int32_t stretch_sample(int32_t out, int32_t out_lo, int32_t out_span_signed,
                              int32_t in_lo, int32_t in_span_signed) {
    int32_t out_abs = abs_int(out_span_signed);
    int32_t in_abs = abs_int(in_span_signed);
    int32_t doff = (out_span_signed >= 0) ? (out - out_lo) : (out_lo - 1 - out);
    int32_t soff = ((doff * 2 + 1) * in_abs) / (2 * out_abs);
    return (in_span_signed >= 0) ? (in_lo + soff) : (in_lo - 1 - soff);
}

void raster_anyblt(const raster_surface *dst, const raster_surface *src, const raster_surface *mask,
                   const raster_clip *clip, const raster_xlate *xlate,
                   const raster_rect *dst_rect, const raster_rect *src_rect,
                   int32_t mask_x, int32_t mask_y,
                   const raster_brush *brush, uint16_t rop4) {
    int32_t dst_w = dst_rect->right - dst_rect->left;
    int32_t dst_h = dst_rect->bottom - dst_rect->top;
    int32_t src_w = src_rect->right - src_rect->left;
    int32_t src_h = src_rect->bottom - src_rect->top;

    if (dst_w == src_w && dst_h == src_h) {
        raster_bitblt(dst, src, mask, clip, xlate, dst_rect,
                      src_rect->left, src_rect->top, mask_x, mask_y, brush, rop4, NULL);
        return;
    }
    if (dst_w == 0 || dst_h == 0) {
        return;
    }

    uint8_t rop_foreground = (uint8_t)(rop4 & 0xFF);
    uint8_t rop_background = (uint8_t)(rop4 >> 8);
    int32_t left = dst_rect->left < dst_rect->right ? dst_rect->left : dst_rect->right;
    int32_t right = dst_rect->left < dst_rect->right ? dst_rect->right : dst_rect->left;
    int32_t top = dst_rect->top < dst_rect->bottom ? dst_rect->top : dst_rect->bottom;
    int32_t bottom = dst_rect->top < dst_rect->bottom ? dst_rect->bottom : dst_rect->top;
    if (left < 0) left = 0;
    if (left > dst->width) left = dst->width;
    if (right < 0) right = 0;
    if (right > dst->width) right = dst->width;
    if (top < 0) top = 0;
    if (top > dst->height) top = dst->height;
    if (bottom < 0) bottom = 0;
    if (bottom > dst->height) bottom = dst->height;
    if (left >= right || top >= bottom) {
        return;
    }

    int clip_needed = !(clip == NULL || clip->kind == RASTER_CLIP_NONE);
    int32_t dst_format = dst->format;
    int32_t src_format = src != NULL ? src->format : 0;
    int32_t mask_format = mask != NULL ? mask->format : 0;

    int32_t sx_buf[1024];
    int use_sx_table = (src != NULL) && (right - left) <= 1024;
    if (use_sx_table) {
        for (int32_t x = left; x < right; x++) {
            sx_buf[x - left] = stretch_sample(x, dst_rect->left, dst_w, src_rect->left, src_w);
        }
    }

    for (int32_t y = top; y < bottom; y++) {
        int32_t sy = src ? stretch_sample(y, dst_rect->top, dst_h, src_rect->top, src_h) : 0;
        int32_t my = mask_y + (y - top);
        uint8_t *dst_row = row_address(dst, y);
        const uint8_t *src_row = src != NULL ? row_address(src, sy) : NULL;
        const uint8_t *mask_row = mask != NULL ? row_address(mask, my) : NULL;
        for (int32_t x = left; x < right; x++) {
            if (clip_needed && !clip_contains(clip, x, y)) {
                continue;
            }
            uint32_t mask_bit = 1;
            if (mask != NULL) {
                mask_bit = get_pixel_row(mask_row, mask_format, mask_x + (x - left)) ? 1 : 0;
            }
            uint8_t rop = mask_bit ? rop_foreground : rop_background;
            uint32_t source_value = 0;
            if (src != NULL) {
                int32_t sx = use_sx_table ? sx_buf[x - left]
                                          : stretch_sample(x, dst_rect->left, dst_w, src_rect->left, src_w);
                source_value = raster_translate(xlate, get_pixel_row(src_row, src_format, sx));
            }
            uint32_t pattern_value = brush_pixel(brush, x, y);
            uint32_t dest_value = get_pixel_row(dst_row, dst_format, x);
            put_pixel_row(dst_row, dst_format, x, raster_rop3(rop, dest_value, source_value, pattern_value));
        }
    }
}

void raster_line(const raster_surface *dst, const raster_clip *clip,
                 int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 uint32_t pen, uint8_t rop2) {
    int32_t dx = abs_int(x1 - x0);
    int32_t dy = -abs_int(y1 - y0);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    int32_t x = x0;
    int32_t y = y0;
    for (;;) {
        if (x == x1 && y == y1) {
            break;
        }
        if (x >= 0 && x < dst->width && y >= 0 && y < dst->height && clip_contains(clip, x, y)) {
            uint32_t d = raster_get_pixel(dst, x, y);
            raster_put_pixel(dst, x, y, raster_rop2(rop2, d, pen));
        }
        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

void raster_transparentblt(const raster_surface *dst, const raster_surface *src,
                           const raster_clip *clip, const raster_xlate *xlate,
                           const raster_rect *dst_rect, const raster_rect *src_rect,
                           uint32_t trans_colour) {
    int32_t dst_w = dst_rect->right - dst_rect->left;
    int32_t dst_h = dst_rect->bottom - dst_rect->top;
    int32_t src_w = src_rect->right - src_rect->left;
    int32_t src_h = src_rect->bottom - src_rect->top;
    if (dst_w == 0 || dst_h == 0) {
        return;
    }
    int32_t left = dst_rect->left < dst_rect->right ? dst_rect->left : dst_rect->right;
    int32_t right = dst_rect->left < dst_rect->right ? dst_rect->right : dst_rect->left;
    int32_t top = dst_rect->top < dst_rect->bottom ? dst_rect->top : dst_rect->bottom;
    int32_t bottom = dst_rect->top < dst_rect->bottom ? dst_rect->bottom : dst_rect->top;
    if (left < 0) left = 0;
    if (left > dst->width) left = dst->width;
    if (right < 0) right = 0;
    if (right > dst->width) right = dst->width;
    if (top < 0) top = 0;
    if (top > dst->height) top = dst->height;
    if (bottom < 0) bottom = 0;
    if (bottom > dst->height) bottom = dst->height;
    if (left >= right || top >= bottom) {
        return;
    }

    int clip_needed = !(clip == NULL || clip->kind == RASTER_CLIP_NONE);
    int32_t dst_format = dst->format;
    int32_t src_format = src->format;

    int32_t sx_buf[1024];
    int use_sx_table = (right - left) <= 1024;
    if (use_sx_table) {
        for (int32_t x = left; x < right; x++) {
            sx_buf[x - left] = stretch_sample(x, dst_rect->left, dst_w, src_rect->left, src_w);
        }
    }

    for (int32_t y = top; y < bottom; y++) {
        int32_t sy = stretch_sample(y, dst_rect->top, dst_h, src_rect->top, src_h);
        uint8_t *dst_row = row_address(dst, y);
        const uint8_t *src_row = row_address(src, sy);
        for (int32_t x = left; x < right; x++) {
            if (clip_needed && !clip_contains(clip, x, y)) {
                continue;
            }
            int32_t sx = use_sx_table ? sx_buf[x - left]
                                      : stretch_sample(x, dst_rect->left, dst_w, src_rect->left, src_w);
            uint32_t raw = get_pixel_row(src_row, src_format, sx);
            if (raw == trans_colour) {
                continue;
            }
            put_pixel_row(dst_row, dst_format, x, raster_translate(xlate, raw));
        }
    }
}
