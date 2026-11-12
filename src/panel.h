#ifndef BRAIN_PANEL_H
#define BRAIN_PANEL_H

#include <stdint.h>

int brain_panel_init(void);
void brain_panel_set_window(int32_t col_start, int32_t col_count, int32_t page_start, int32_t page_count);
int brain_panel_transfer(uint32_t phys, int32_t pixels_per_row, int32_t rows);
void brain_panel_suspend(void);
void brain_panel_resume_prepare(void);
void brain_panel_display_on(void);

uint32_t brain_panel_reg_ctrl(void);
uint32_t brain_panel_reg_ctrl1(void);
uint32_t brain_panel_reg_stat(void);

void brain_panel_recover(uint32_t level);

#endif
