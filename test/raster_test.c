#include "../src/raster.h"
#include "raster_ref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;

static uint32_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static uint32_t random_below(uint32_t limit) {
    return next_random() % limit;
}

static int format_bits(int32_t format) {
    switch (format) {
    case RASTER_FORMAT_1BPP: return 1;
    case RASTER_FORMAT_2BPP: return 2;
    case RASTER_FORMAT_4BPP: return 4;
    case RASTER_FORMAT_8BPP: return 8;
    case RASTER_FORMAT_16BPP: return 16;
    case RASTER_FORMAT_24BPP: return 24;
    case RASTER_FORMAT_32BPP: return 32;
    default: return 0;
    }
}

typedef struct {
    raster_surface surface;
    uint8_t *buffer;
    size_t buffer_size;
    int32_t abs_stride;
} managed_surface;

static void surface_create(managed_surface *managed, int32_t format, int32_t width, int32_t height, int negative) {
    int bits = format_bits(format);
    int32_t row_bytes = ((width * bits + 31) / 32) * 4;
    managed->abs_stride = row_bytes;
    managed->buffer_size = (size_t)row_bytes * height;
    managed->buffer = malloc(managed->buffer_size);
    for (size_t index = 0; index < managed->buffer_size; index++) {
        managed->buffer[index] = (uint8_t)next_random();
    }
    managed->surface.format = format;
    managed->surface.width = width;
    managed->surface.height = height;
    if (negative) {
        managed->surface.stride = -row_bytes;
        managed->surface.bits = managed->buffer + (size_t)row_bytes * (height - 1);
    } else {
        managed->surface.stride = row_bytes;
        managed->surface.bits = managed->buffer;
    }
}

static void surface_destroy(managed_surface *managed) {
    free(managed->buffer);
    managed->buffer = NULL;
}

static void random_rect_within(raster_rect *rect, int32_t width, int32_t height) {
    int32_t x0 = random_below(width);
    int32_t x1 = random_below(width);
    int32_t y0 = random_below(height);
    int32_t y1 = random_below(height);
    rect->left = x0 < x1 ? x0 : x1;
    rect->right = (x0 < x1 ? x1 : x0) + 1;
    rect->top = y0 < y1 ? y0 : y1;
    rect->bottom = (y0 < y1 ? y1 : y0) + 1;
}

static int buffers_match(const managed_surface *a, const managed_surface *b) {
    return memcmp(a->buffer, b->buffer, a->buffer_size) == 0;
}

static const int32_t all_formats[] = {
    RASTER_FORMAT_1BPP, RASTER_FORMAT_2BPP, RASTER_FORMAT_4BPP, RASTER_FORMAT_8BPP,
    RASTER_FORMAT_16BPP, RASTER_FORMAT_24BPP, RASTER_FORMAT_32BPP
};

static int run_case(uint32_t iteration) {
    int32_t dst_format = all_formats[random_below(7)];
    int scenario = random_below(3);
    int32_t src_format = dst_format;
    int use_source = 0;
    int use_table = 0;
    if (scenario == 1) {
        use_source = 1;
    } else if (scenario == 2) {
        use_source = 1;
        static const int32_t indexed[] = {RASTER_FORMAT_1BPP, RASTER_FORMAT_2BPP, RASTER_FORMAT_4BPP, RASTER_FORMAT_8BPP};
        src_format = indexed[random_below(4)];
        dst_format = RASTER_FORMAT_16BPP;
        use_table = 1;
    }

    int32_t width = 8 + random_below(56);
    int32_t height = 8 + random_below(40);

    managed_surface dst_a, dst_b;
    surface_create(&dst_a, dst_format, width, height, random_below(2));
    dst_b = dst_a;
    dst_b.buffer = malloc(dst_a.buffer_size);
    memcpy(dst_b.buffer, dst_a.buffer, dst_a.buffer_size);
    dst_b.surface.bits = (dst_a.surface.stride < 0)
        ? dst_b.buffer + (size_t)dst_a.abs_stride * (height - 1)
        : dst_b.buffer;

    managed_surface src;
    int same_surface = use_source && random_below(2);
    if (use_source && !same_surface) {
        surface_create(&src, src_format, width, height, random_below(2));
    }

    managed_surface mask;
    int use_mask = random_below(2);
    if (use_mask) {
        surface_create(&mask, RASTER_FORMAT_1BPP, width, height, random_below(2));
    }

    uint32_t table[256];
    raster_xlate xlate;
    memset(&xlate, 0, sizeof(xlate));
    if (use_table) {
        uint32_t entries = 1u << format_bits(src_format);
        for (uint32_t index = 0; index < entries; index++) {
            table[index] = next_random() & 0xFFFF;
        }
        xlate.kind = RASTER_XLATE_TABLE;
        xlate.table = table;
        xlate.table_count = entries;
    } else {
        xlate.kind = RASTER_XLATE_TRIVIAL;
    }

    managed_surface pattern;
    raster_brush brush;
    memset(&brush, 0, sizeof(brush));
    int use_pattern = !same_surface && random_below(2);
    if (use_pattern) {
        surface_create(&pattern, dst_format, 8, 8, 0);
        brush.is_pattern = 1;
        brush.pattern = pattern.surface;
        brush.org_x = random_below(8);
        brush.org_y = random_below(8);
    } else {
        brush.is_pattern = 0;
        brush.solid = next_random();
    }

    uint16_t rop4;
    if (same_surface) {
        rop4 = 0xCCCC;
    } else {
        uint8_t fg = (uint8_t)random_below(256);
        uint8_t bg = use_mask ? (uint8_t)random_below(256) : fg;
        rop4 = (uint16_t)(fg | (bg << 8));
    }

    raster_rect rect;
    random_rect_within(&rect, width, height);

    int32_t rect_w = rect.right - rect.left;
    int32_t rect_h = rect.bottom - rect.top;
    int32_t src_x = 0, src_y = 0;
    if (use_source && same_surface) {
        src_x = random_below(width - rect_w + 1);
        src_y = random_below(height - rect_h + 1);
    }
    int32_t mask_x = use_mask ? random_below(width - rect_w + 1) : 0;
    int32_t mask_y = use_mask ? random_below(height - rect_h + 1) : 0;

    raster_rect clip_rects[4];
    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    int clip_kind = random_below(3);
    if (clip_kind == 0) {
        clip.kind = RASTER_CLIP_NONE;
    } else if (clip_kind == 1) {
        clip.kind = RASTER_CLIP_RECT;
        random_rect_within(&clip.bounds, width, height);
    } else {
        clip.kind = RASTER_CLIP_COMPLEX;
        clip.rect_count = 1 + random_below(4);
        clip.rects = clip_rects;
        raster_rect bounds = {width, height, 0, 0};
        for (uint32_t index = 0; index < clip.rect_count; index++) {
            random_rect_within(&clip_rects[index], width, height);
            if (clip_rects[index].left < bounds.left) bounds.left = clip_rects[index].left;
            if (clip_rects[index].top < bounds.top) bounds.top = clip_rects[index].top;
            if (clip_rects[index].right > bounds.right) bounds.right = clip_rects[index].right;
            if (clip_rects[index].bottom > bounds.bottom) bounds.bottom = clip_rects[index].bottom;
        }
        clip.bounds = bounds;
    }

    const raster_surface *src_ptr = NULL;
    if (use_source) {
        src_ptr = same_surface ? &dst_a.surface : &src.surface;
    }
    const raster_surface *mask_ptr = use_mask ? &mask.surface : NULL;

    raster_bitblt(&dst_a.surface, src_ptr, mask_ptr, &clip, &xlate, &rect,
                  src_x, src_y, mask_x, mask_y, &brush, rop4, NULL);

    const raster_surface *ref_src_ptr = NULL;
    if (use_source) {
        ref_src_ptr = same_surface ? &dst_b.surface : &src.surface;
    }
    raster_ref_bitblt(&dst_b.surface, ref_src_ptr, mask_ptr, &clip, &xlate, &rect,
                      src_x, src_y, mask_x, mask_y, &brush, rop4);

    int ok = buffers_match(&dst_a, &dst_b);
    if (!ok) {
        printf("MISMATCH iteration %u: dstfmt=%d scenario=%d srcfmt=%d rop4=0x%04X "
               "rect=(%d,%d,%d,%d) clip=%d mask=%d pattern=%d same=%d neg=%d\n",
               iteration, dst_format, scenario, src_format, rop4,
               rect.left, rect.top, rect.right, rect.bottom,
               clip.kind, use_mask, use_pattern, same_surface, dst_a.surface.stride < 0);
    }

    surface_destroy(&dst_a);
    free(dst_b.buffer);
    if (use_source && !same_surface) surface_destroy(&src);
    if (use_mask) surface_destroy(&mask);
    if (use_pattern) surface_destroy(&pattern);
    return ok;
}

int main(int argc, char **argv) {
    uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 200000;
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 10) : 0x9E3779B97F4A7C15ull;
    if (rng_state == 0) {
        rng_state = 1;
    }
    uint32_t failures = 0;
    for (uint32_t iteration = 0; iteration < iterations; iteration++) {
        if (!run_case(iteration)) {
            failures++;
            if (failures >= 10) {
                break;
            }
        }
    }
    if (failures) {
        printf("FAILED: %u mismatch(es) in %u iterations\n", failures, iterations);
        return 1;
    }
    printf("OK: %u iterations passed\n", iterations);
    return 0;
}
