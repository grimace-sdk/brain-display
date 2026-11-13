/*
 * board.c - Per-board descriptor (panel geometry, TE pin and period, mirror),
 * selected at compile time by BRAIN_BOARD_pwsh2 / BRAIN_BOARD_pwa7300.
 */
#include "board.h"

const brain_board brain_current_board = {
    BRAIN_SCREEN_WIDTH,
    BRAIN_SCREEN_HEIGHT,
    BRAIN_PANEL_WIDTH,
    BRAIN_PANEL_HEIGHT,
#if defined(BRAIN_BOARD_pwsh2)
    1,
    3, 30, 1, 254, 40, 16900, 2
#else
    0,
    4, 16, 0, 272, 50, 20100, 0
#endif
};
