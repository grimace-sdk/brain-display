#include "../src/raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ref_rop2(uint8_t code, uint32_t dst, uint32_t pen) {
    uint32_t table = (uint32_t)(code - 1);
    uint32_t result = 0;
    for (int bit = 0; bit < 16; bit++) {
        int p = (pen >> bit) & 1;
        int d = (dst >> bit) & 1;
        int index = (p << 1) | d;
        if ((table >> index) & 1) {
            result |= (1u << bit);
        }
    }
    return result;
}

static int test_rop2(void) {
    uint32_t samples[] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xA5A5, 0x1234, 0xBEEF};
    for (int code = 1; code <= 16; code++) {
        for (unsigned a = 0; a < sizeof(samples) / sizeof(samples[0]); a++) {
            for (unsigned b = 0; b < sizeof(samples) / sizeof(samples[0]); b++) {
                uint32_t got = raster_rop2((uint8_t)code, samples[a], samples[b]) & 0xFFFF;
                uint32_t want = ref_rop2((uint8_t)code, samples[a], samples[b]);
                if (got != want) {
                    printf("rop2 code %d D=0x%X P=0x%X got 0x%X want 0x%X\n",
                           code, samples[a], samples[b], got, want);
                    return 1;
                }
            }
        }
    }
    printf("rop2: OK\n");
    return 0;
}

static int test_line_endpoint(void) {
    uint16_t buffer[16 * 4];
    memset(buffer, 0, sizeof(buffer));
    raster_surface dst = {buffer, 16, 4, 16 * 2, RASTER_FORMAT_16BPP};
    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.kind = RASTER_CLIP_NONE;
    raster_line(&dst, &clip, 0, 0, 5, 0, 0xFFFF, 13);
    for (int x = 0; x < 5; x++) {
        if (raster_get_pixel(&dst, x, 0) != 0xFFFF) {
            printf("line: pixel %d not set\n", x);
            return 1;
        }
    }
    if (raster_get_pixel(&dst, 5, 0) != 0) {
        printf("line: endpoint (5,0) should be excluded\n");
        return 1;
    }
    printf("line: OK\n");
    return 0;
}

static int test_anyblt_stretch(void) {
    uint16_t src_bits[4];
    src_bits[0] = 0x1111;
    src_bits[1] = 0x2222;
    src_bits[2] = 0x3333;
    src_bits[3] = 0x4444;
    raster_surface src = {src_bits, 2, 2, 2 * 2, RASTER_FORMAT_16BPP};

    uint16_t dst_bits[16];
    memset(dst_bits, 0, sizeof(dst_bits));
    raster_surface dst = {dst_bits, 4, 4, 4 * 2, RASTER_FORMAT_16BPP};

    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.kind = RASTER_CLIP_NONE;
    raster_xlate xlate;
    memset(&xlate, 0, sizeof(xlate));
    xlate.kind = RASTER_XLATE_TRIVIAL;
    raster_brush brush;
    memset(&brush, 0, sizeof(brush));
    brush.is_pattern = 0;
    brush.solid = 0;

    raster_rect dst_rect = {0, 0, 4, 4};
    raster_rect src_rect = {0, 0, 2, 2};
    raster_anyblt(&dst, &src, NULL, &clip, &xlate, &dst_rect, &src_rect, 0, 0, &brush, 0xCCCC);

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint16_t want = src_bits[(y / 2) * 2 + (x / 2)];
            uint16_t got = (uint16_t)raster_get_pixel(&dst, x, y);
            if (got != want) {
                printf("anyblt 2x: (%d,%d) got 0x%X want 0x%X\n", x, y, got, want);
                return 1;
            }
        }
    }
    printf("anyblt stretch: OK\n");
    return 0;
}

static int test_anyblt_mirror(void) {
    uint16_t src_bits[8] = {0x10, 0x11, 0x20, 0x21, 0x30, 0x31, 0x40, 0x41};
    raster_surface src = {src_bits, 2, 4, 2 * 2, RASTER_FORMAT_16BPP};
    uint16_t dst_bits[8];
    memset(dst_bits, 0, sizeof(dst_bits));
    raster_surface dst = {dst_bits, 2, 4, 2 * 2, RASTER_FORMAT_16BPP};

    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.kind = RASTER_CLIP_NONE;
    raster_xlate xlate;
    memset(&xlate, 0, sizeof(xlate));
    xlate.kind = RASTER_XLATE_TRIVIAL;
    raster_brush brush;
    memset(&brush, 0, sizeof(brush));

    raster_rect dst_rect = {0, 4, 2, 0};
    raster_rect src_rect = {0, 0, 2, 4};
    raster_anyblt(&dst, &src, NULL, &clip, &xlate, &dst_rect, &src_rect, 0, 0, &brush, 0xCCCC);

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 2; x++) {
            uint16_t want = src_bits[(3 - y) * 2 + x];
            uint16_t got = (uint16_t)raster_get_pixel(&dst, x, y);
            if (got != want) {
                printf("anyblt v-mirror: (%d,%d) got 0x%X want 0x%X\n", x, y, got, want);
                return 1;
            }
        }
    }

    raster_rect off_dst = {-3, -3, 5, 7};
    raster_rect off_src = {0, 0, 2, 4};
    raster_anyblt(&dst, &src, NULL, &clip, &xlate, &off_dst, &off_src, 0, 0, &brush, 0xCCCC);

    printf("anyblt mirror/clip: OK\n");
    return 0;
}

static int test_fill_polygon(void) {
    uint16_t buffer[8 * 8];
    memset(buffer, 0, sizeof(buffer));
    raster_surface dst = {buffer, 8, 8, 8 * 2, RASTER_FORMAT_16BPP};
    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.kind = RASTER_CLIP_NONE;
    raster_brush brush;
    memset(&brush, 0, sizeof(brush));
    brush.is_pattern = 0;
    brush.solid = 0xABCD;

    raster_point points[4] = {{2, 2}, {6, 2}, {6, 6}, {2, 6}};
    int32_t lengths[1] = {4};
    raster_fill_polygon(&dst, &clip, points, lengths, 1, &brush, 13, 0);

    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int inside = (x >= 2 && x < 6 && y >= 2 && y < 6);
            uint16_t got = (uint16_t)raster_get_pixel(&dst, x, y);
            uint16_t want = inside ? 0xABCD : 0x0000;
            if (got != want) {
                printf("fill_polygon: (%d,%d) got 0x%X want 0x%X\n", x, y, got, want);
                return 1;
            }
        }
    }
    printf("fill_polygon: OK\n");
    return 0;
}

static int test_transparentblt(void) {
    uint16_t src_bits[4] = {0x1111, 0x0000, 0x0000, 0x4444};
    raster_surface src = {src_bits, 2, 2, 2 * 2, RASTER_FORMAT_16BPP};
    uint16_t dst_bits[4] = {0x9999, 0x9999, 0x9999, 0x9999};
    raster_surface dst = {dst_bits, 2, 2, 2 * 2, RASTER_FORMAT_16BPP};

    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.kind = RASTER_CLIP_NONE;
    raster_xlate xlate;
    memset(&xlate, 0, sizeof(xlate));
    xlate.kind = RASTER_XLATE_TRIVIAL;

    raster_rect dst_rect = {0, 0, 2, 2};
    raster_rect src_rect = {0, 0, 2, 2};
    raster_transparentblt(&dst, &src, &clip, &xlate, &dst_rect, &src_rect, 0x0000);

    uint16_t want[4] = {0x1111, 0x9999, 0x9999, 0x4444};
    for (int i = 0; i < 4; i++) {
        if (dst_bits[i] != want[i]) {
            printf("transparentblt: pixel %d got 0x%X want 0x%X\n", i, dst_bits[i], want[i]);
            return 1;
        }
    }
    printf("transparentblt: OK\n");
    return 0;
}

static int test_rgb_convert(void) {
    raster_xlate to565;
    memset(&to565, 0, sizeof(to565));
    to565.kind = RASTER_XLATE_RGB;
    to565.src_red = 0xFF0000; to565.src_green = 0x00FF00; to565.src_blue = 0x0000FF;
    to565.dst_red = 0xF800; to565.dst_green = 0x07E0; to565.dst_blue = 0x001F;

    raster_xlate to888;
    memset(&to888, 0, sizeof(to888));
    to888.kind = RASTER_XLATE_RGB;
    to888.src_red = 0xF800; to888.src_green = 0x07E0; to888.src_blue = 0x001F;
    to888.dst_red = 0xFF0000; to888.dst_green = 0x00FF00; to888.dst_blue = 0x0000FF;

    uint32_t packed888 = 8 | (44 << 8) | (74 << 16);
    uint32_t p565 = raster_translate(&to565, packed888);
    uint32_t back = raster_translate(&to888, p565);
    uint32_t r = (back >> 16) & 0xFF, g = (back >> 8) & 0xFF, b = back & 0xFF;
    if (r != 74 || g != 44 || b != 8) {
        printf("rgb_convert: 888->565->888 got (%u,%u,%u) want (74,44,8)\n", r, g, b);
        return 1;
    }

    raster_xlate t555to565;
    memset(&t555to565, 0, sizeof(t555to565));
    t555to565.kind = RASTER_XLATE_RGB;
    t555to565.src_red = 0x7C00; t555to565.src_green = 0x03E0; t555to565.src_blue = 0x001F;
    t555to565.dst_red = 0xF800; t555to565.dst_green = 0x07E0; t555to565.dst_blue = 0x001F;
    uint32_t c555 = (31 << 10) | (31 << 5) | 31;
    uint32_t c565 = raster_translate(&t555to565, c555);
    if (c565 != 0xFFFF) {
        printf("rgb_convert: 555 white -> 565 got 0x%X want 0xFFFF\n", c565);
        return 1;
    }
    printf("rgb_convert: OK\n");
    return 0;
}

static int test_rgb_prepared(void) {
    struct { uint32_t sr, sg, sb, dr, dg, db; } pairs[] = {
        {0x7C00, 0x03E0, 0x001F, 0xF800, 0x07E0, 0x001F},
        {0xF800, 0x07E0, 0x001F, 0x7C00, 0x03E0, 0x001F},
        {0xFF0000, 0x00FF00, 0x0000FF, 0xF800, 0x07E0, 0x001F},
        {0xF800, 0x07E0, 0x001F, 0xFF0000, 0x00FF00, 0x0000FF},
        {0xF800, 0x07E0, 0x001F, 0xF800, 0x07E0, 0x001F},
    };
    for (unsigned p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
        raster_xlate slow, fast;
        memset(&slow, 0, sizeof(slow));
        slow.kind = RASTER_XLATE_RGB;
        slow.src_red = pairs[p].sr; slow.src_green = pairs[p].sg; slow.src_blue = pairs[p].sb;
        slow.dst_red = pairs[p].dr; slow.dst_green = pairs[p].dg; slow.dst_blue = pairs[p].db;
        fast = slow;
        raster_xlate_prepare(&fast);
        for (uint32_t src = 0; src <= 0xFFFF; src++) {
            uint32_t a = raster_translate(&slow, src);
            uint32_t b = raster_translate(&fast, src);
            if (a != b) {
                printf("rgb_prepared: pair %u src 0x%X slow 0x%X fast 0x%X\n", p, src, a, b);
                return 1;
            }
        }
    }
    printf("rgb_prepared: OK\n");
    return 0;
}

static int test_rgb_to_index(void) {
    uint32_t palette[4] = {0x000000, 0x0000FF, 0x00FF00, 0xFF0000};
    raster_xlate x;
    memset(&x, 0, sizeof(x));
    x.kind = RASTER_XLATE_RGB_TO_INDEX;
    x.src_red = 0xF800; x.src_green = 0x07E0; x.src_blue = 0x001F;
    x.table = palette;
    x.table_count = 4;

    struct { uint32_t src565; uint32_t want; } cases[] = {
        {0x0000, 0}, {0xF800, 1}, {0x07E0, 2}, {0x001F, 3}, {0xFFFF, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t got = raster_translate(&x, cases[i].src565);
        if (i < 4 && got != cases[i].want) {
            printf("rgb_to_index: src 0x%X got %u want %u\n", cases[i].src565, got, cases[i].want);
            return 1;
        }
    }
    printf("rgb_to_index: OK\n");
    return 0;
}

static int test_aa_text(void) {
    uint16_t dst_bits[3] = {0, 0, 0};
    raster_surface dst = {dst_bits, 3, 1, 3 * 2, RASTER_FORMAT_16BPP};
    uint8_t mask_bits[4] = {0, 57, 114, 0};
    raster_surface mask = {mask_bits, 3, 1, 4, RASTER_FORMAT_8BPP};

    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.kind = RASTER_CLIP_NONE;
    raster_brush brush;
    memset(&brush, 0, sizeof(brush));
    brush.is_pattern = 0;
    brush.solid = 0xFFFF;
    raster_rect rect = {0, 0, 3, 1};

    raster_bitblt(&dst, NULL, &mask, &clip, NULL, &rect, 0, 0, 0, 0, &brush, 0xAAF0, NULL);

    if (dst_bits[0] != 0x0000) {
        printf("aa_text: coverage 0 gave 0x%X, want 0x0000\n", dst_bits[0]);
        return 1;
    }
    if (dst_bits[2] != 0xFFFF) {
        printf("aa_text: full coverage gave 0x%X, want 0xFFFF\n", dst_bits[2]);
        return 1;
    }
    if (dst_bits[1] == 0x0000 || dst_bits[1] == 0xFFFF) {
        printf("aa_text: half coverage gave 0x%X, want an in-between blend\n", dst_bits[1]);
        return 1;
    }
    printf("aa_text: OK\n");
    return 0;
}

static int test_complex_clip(void) {
    uint16_t a[20 * 12], b[20 * 12], src[20 * 12];
    for (int i = 0; i < 20 * 12; i++) {
        a[i] = b[i] = 0x1234;
        src[i] = (uint16_t)(i * 7 + 1);
    }
    raster_surface da = {a, 20, 12, 40, RASTER_FORMAT_16BPP};
    raster_surface db = {b, 20, 12, 40, RASTER_FORMAT_16BPP};
    raster_surface s = {src, 20, 12, 40, RASTER_FORMAT_16BPP};
    raster_rect rects[3] = {{2, 2, 10, 6}, {2, 6, 10, 11}, {12, 3, 19, 9}};
    raster_clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.kind = RASTER_CLIP_COMPLEX;
    clip.rects = rects;
    clip.rect_count = 3;
    clip.bounds.left = 2;
    clip.bounds.top = 2;
    clip.bounds.right = 19;
    clip.bounds.bottom = 11;
    raster_rect rect = {0, 0, 20, 12};
    raster_brush brush;
    memset(&brush, 0, sizeof(brush));

    clip.non_overlapping = 1;
    raster_bitblt(&da, &s, NULL, &clip, NULL, &rect, 0, 0, 0, 0, &brush, 0xCCCC, NULL);
    clip.non_overlapping = 0;
    raster_bitblt(&db, &s, NULL, &clip, NULL, &rect, 0, 0, 0, 0, &brush, 0xCCCC, NULL);

    if (memcmp(a, b, sizeof(a)) != 0) {
        printf("complex_clip: restructure != per-pixel path\n");
        return 1;
    }
    printf("complex_clip: OK\n");
    return 0;
}

static int test_fill_polygon_clip(void) {
    uint16_t a[24 * 16], b[24 * 16];
    for (int i = 0; i < 24 * 16; i++) {
        a[i] = b[i] = 0x0842;
    }
    raster_surface da = {a, 24, 16, 48, RASTER_FORMAT_16BPP};
    raster_surface db = {b, 24, 16, 48, RASTER_FORMAT_16BPP};
    raster_point pts[4] = {{2, 2}, {20, 3}, {18, 14}, {3, 13}};
    int32_t lens[1] = {4};
    raster_brush brush;
    memset(&brush, 0, sizeof(brush));
    brush.solid = 0xF800;
    raster_rect r = {5, 4, 16, 12};

    raster_clip crect;
    memset(&crect, 0, sizeof(crect));
    crect.kind = RASTER_CLIP_RECT;
    crect.bounds = r;
    raster_fill_polygon(&da, &crect, pts, lens, 1, &brush, 13, 0);

    raster_clip ccomplex;
    memset(&ccomplex, 0, sizeof(ccomplex));
    ccomplex.kind = RASTER_CLIP_COMPLEX;
    ccomplex.bounds = r;
    ccomplex.rects = &r;
    ccomplex.rect_count = 1;
    raster_fill_polygon(&db, &ccomplex, pts, lens, 1, &brush, 13, 0);

    if (memcmp(a, b, sizeof(a)) != 0) {
        printf("fill_polygon_clip: RECT fast path != per-pixel path\n");
        return 1;
    }
    printf("fill_polygon_clip: OK\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_fill_polygon_clip();
    failures += test_complex_clip();
    failures += test_aa_text();
    failures += test_rgb_to_index();
    failures += test_rgb_convert();
    failures += test_rgb_prepared();
    failures += test_rop2();
    failures += test_line_endpoint();
    failures += test_anyblt_stretch();
    failures += test_anyblt_mirror();
    failures += test_fill_polygon();
    failures += test_transparentblt();
    if (failures) {
        printf("FAILED\n");
        return 1;
    }
    printf("OK: raster2\n");
    return 0;
}
