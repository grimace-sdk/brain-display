#include "compose.h"

#include <stddef.h>

static void panel_to_frame(const brain_board *board, int angle, int32_t col, int32_t page,
                           int32_t *fx, int32_t *fy) {
    int32_t pw = board->panel_width;
    int32_t ph = board->panel_height;
    int32_t x = page;
    int32_t y = board->mirror_columns ? (pw - 1 - col) : col;
    switch (angle) {
    case 1:
        *fx = (pw - 1) - y;
        *fy = x;
        break;
    case 2:
        *fx = (ph - 1) - x;
        *fy = (pw - 1) - y;
        break;
    case 3:
        *fx = y;
        *fy = (ph - 1) - x;
        break;
    default:
        *fx = x;
        *fy = y;
        break;
    }
}

static void frame_to_panel(const brain_board *board, int angle, int32_t fx, int32_t fy,
                           int32_t *col, int32_t *page) {
    int32_t pw = board->panel_width;
    int32_t ph = board->panel_height;
    int32_t x, y;
    switch (angle) {
    case 1:
        x = fy;
        y = (pw - 1) - fx;
        break;
    case 2:
        x = (ph - 1) - fx;
        y = (pw - 1) - fy;
        break;
    case 3:
        x = (ph - 1) - fy;
        y = fx;
        break;
    default:
        x = fx;
        y = fy;
        break;
    }
    *page = x;
    *col = board->mirror_columns ? (pw - 1 - y) : y;
}

void brain_compose_window(const brain_board *board, const raster_rect *screen_rect, int angle,
                          brain_panel_window *window) {
    int32_t corners_x[4] = {screen_rect->left, screen_rect->right - 1,
                            screen_rect->left, screen_rect->right - 1};
    int32_t corners_y[4] = {screen_rect->top, screen_rect->top,
                            screen_rect->bottom - 1, screen_rect->bottom - 1};
    int32_t col_lo = board->panel_width;
    int32_t col_hi = -1;
    int32_t page_lo = board->panel_height;
    int32_t page_hi = -1;
    for (int i = 0; i < 4; i++) {
        int32_t col, page;
        frame_to_panel(board, angle, corners_x[i], corners_y[i], &col, &page);
        if (col < col_lo) col_lo = col;
        if (col > col_hi) col_hi = col;
        if (page < page_lo) page_lo = page;
        if (page > page_hi) page_hi = page;
    }

    int32_t col_count = col_hi - col_lo + 1;
    if (col_count & 1) {
        if (col_hi + 1 < board->panel_width) {
            col_hi++;
        } else if (col_lo > 0) {
            col_lo--;
        }
        col_count++;
    }

    window->col_start = col_lo;
    window->col_count = col_count;
    window->page_start = page_lo;
    window->page_count = page_hi - page_lo + 1;
}

void brain_compose(const brain_board *board, const raster_surface *primary,
                   const raster_rect *screen_rect, int angle, uint16_t *staging,
                   brain_panel_window *window) {
    brain_compose_window(board, screen_rect, angle, window);

    const uint8_t *base = (const uint8_t *)primary->bits;
    for (int32_t page_index = 0; page_index < window->page_count; page_index++) {
        int32_t page = window->page_start + page_index;
        uint16_t *out = staging + (ptrdiff_t)page_index * window->col_count;
        for (int32_t col_index = 0; col_index < window->col_count; col_index++) {
            int32_t col = window->col_start + col_index;
            int32_t fx, fy;
            panel_to_frame(board, angle, col, page, &fx, &fy);
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            if (fx >= primary->width) fx = primary->width - 1;
            if (fy >= primary->height) fy = primary->height - 1;
            const uint16_t *pixels = (const uint16_t *)(base + (ptrdiff_t)fy * primary->stride);
            out[col_index] = pixels[fx];
        }
    }
}
