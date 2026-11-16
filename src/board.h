/*
 * board.h - Board descriptor type, the per-board compile-time values, and the
 * driver version string.
 */
#ifndef BRAIN_BOARD_H
#define BRAIN_BOARD_H

#include <stdint.h>

#define BRAIN_DISPLAY_VERSION "0.1.0-alpha"

typedef struct {
    int32_t screen_width;
    int32_t screen_height;
    int32_t panel_width;
    int32_t panel_height;
    int32_t mirror_columns;
    int32_t te_bank;
    int32_t te_pin;
    int32_t te_rising;
    int32_t te_irq;
    int32_t te_timeout_ms;
    int32_t te_period_us;
    int32_t te_offset_ms;
    int32_t te_prepare_ahead;
} brain_board;

extern const brain_board brain_current_board;

#if defined(BRAIN_BOARD_pwsh2)
#define BRAIN_SCREEN_WIDTH   800
#define BRAIN_SCREEN_HEIGHT  480
#define BRAIN_PANEL_WIDTH    480
#define BRAIN_PANEL_HEIGHT   800
#define BRAIN_HAS_POINTER    0
#define BRAIN_BOARD_NAME     "pwsh2"
#elif defined(BRAIN_BOARD_pwa7300)
#define BRAIN_SCREEN_WIDTH   480
#define BRAIN_SCREEN_HEIGHT  320
#define BRAIN_PANEL_WIDTH    320
#define BRAIN_PANEL_HEIGHT   480
#define BRAIN_HAS_POINTER    1
#define BRAIN_BOARD_NAME     "pwa7300"
#elif defined(BRAIN_BOARD_pwsh4)
#define BRAIN_SCREEN_WIDTH   848
#define BRAIN_SCREEN_HEIGHT  480
#define BRAIN_PANEL_WIDTH    480
#define BRAIN_PANEL_HEIGHT   848
#define BRAIN_HAS_POINTER    0
#define BRAIN_BOARD_NAME     "pwsh4"
#endif

#define BRAIN_SCREEN_MM_X    64
#define BRAIN_SCREEN_MM_Y    60

#endif
