/*
 * compose.h - Frame/panel coordinate transforms and staging compose declarations.
 */
#ifndef BRAIN_COMPOSE_H
#define BRAIN_COMPOSE_H

#include <stdint.h>

#include "board.h"
#include "raster.h"

typedef struct {
    int32_t col_start;
    int32_t col_count;
    int32_t page_start;
    int32_t page_count;
} brain_panel_window;

void brain_compose_window(const brain_board *board, const raster_rect *screen_rect, int angle,
                          brain_panel_window *window);

void brain_compose(const brain_board *board, const raster_surface *primary,
                   const raster_rect *screen_rect, int angle, uint16_t *staging,
                   brain_panel_window *window);

#endif
