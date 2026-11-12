#include "../src/compose.h"
#include "../src/raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state = 0x243F6A8885A308D3ull;

static uint32_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static const brain_board board_a7300 = {480, 320, 320, 480, 0};
static const brain_board board_pwsh2 = {800, 480, 480, 800, 1};

static void ref_frame_to_panel(const brain_board *board, int angle, int32_t fx, int32_t fy,
                               int32_t *col, int32_t *page) {
    int32_t pw = board->panel_width;
    int32_t ph = board->panel_height;
    int32_t x, y;
    switch (angle) {
    case 1:
        x = fy;
        y = pw - 1 - fx;
        break;
    case 2:
        x = ph - 1 - fx;
        y = pw - 1 - fy;
        break;
    case 3:
        x = ph - 1 - fy;
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

static int check_board_angle(const brain_board *board, const char *name, int angle) {
    int32_t fw = (angle == 0 || angle == 2) ? board->screen_width : board->screen_height;
    int32_t fh = (angle == 0 || angle == 2) ? board->screen_height : board->screen_width;

    size_t frame_bytes = (size_t)fw * fh * 2;
    uint16_t *frame_bits = malloc(frame_bytes);
    for (int32_t y = 0; y < fh; y++) {
        for (int32_t x = 0; x < fw; x++) {
            frame_bits[(size_t)y * fw + x] = (uint16_t)((x * 131 + y * 977 + angle * 7) & 0xFFFF);
        }
    }
    raster_surface primary = {frame_bits, fw, fh, fw * 2, RASTER_FORMAT_16BPP};

    uint16_t *staging = malloc((size_t)board->panel_width * board->panel_height * 2);
    uint16_t *panel = malloc((size_t)board->panel_width * board->panel_height * 2);

    for (int iteration = 0; iteration < 20000; iteration++) {
        int32_t x0 = next_random() % fw;
        int32_t x1 = next_random() % fw;
        int32_t y0 = next_random() % fh;
        int32_t y1 = next_random() % fh;
        raster_rect rect;
        rect.left = x0 < x1 ? x0 : x1;
        rect.right = (x0 < x1 ? x1 : x0) + 1;
        rect.top = y0 < y1 ? y0 : y1;
        rect.bottom = (y0 < y1 ? y1 : y0) + 1;

        brain_panel_window window;
        brain_compose(board, &primary, &rect, angle, staging, &window);

        if (window.col_start < 0 || window.col_start + window.col_count > board->panel_width ||
            window.page_start < 0 || window.page_start + window.page_count > board->panel_height) {
            printf("%s a%d: window out of panel bounds at iteration %d\n", name, angle, iteration);
            return 1;
        }
        if (window.col_count & 1) {
            printf("%s a%d: odd col_count %d at iteration %d\n", name, angle, window.col_count, iteration);
            return 1;
        }

        memset(panel, 0xAB, (size_t)board->panel_width * board->panel_height * 2);
        for (int32_t pi = 0; pi < window.page_count; pi++) {
            for (int32_t ci = 0; ci < window.col_count; ci++) {
                int32_t page = window.page_start + pi;
                int32_t col = window.col_start + ci;
                panel[(size_t)page * board->panel_width + col] = staging[(size_t)pi * window.col_count + ci];
            }
        }

        for (int32_t y = rect.top; y < rect.bottom; y++) {
            for (int32_t x = rect.left; x < rect.right; x++) {
                int32_t col, page;
                ref_frame_to_panel(board, angle, x, y, &col, &page);
                uint16_t got = panel[(size_t)page * board->panel_width + col];
                uint16_t want = frame_bits[(size_t)y * fw + x];
                if (got != want) {
                    printf("%s a%d: mismatch at frame (%d,%d) -> panel page %d col %d: got 0x%X want 0x%X "
                           "(rect %d,%d,%d,%d)\n",
                           name, angle, x, y, page, col, got, want,
                           rect.left, rect.top, rect.right, rect.bottom);
                    return 1;
                }
            }
        }
    }

    free(frame_bits);
    free(staging);
    free(panel);
    return 0;
}

static int check_board(const brain_board *board, const char *name) {
    for (int angle = 0; angle < 4; angle++) {
        if (check_board_angle(board, name, angle)) {
            return 1;
        }
    }
    printf("%s: OK\n", name);
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check_board(&board_a7300, "pwa7300");
    failures += check_board(&board_pwsh2, "pwsh2");
    if (failures) {
        printf("FAILED\n");
        return 1;
    }
    printf("OK: compose\n");
    return 0;
}
