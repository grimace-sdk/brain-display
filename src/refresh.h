/*
 * refresh.h - Refresh thread lifecycle, dirty marking, rotation and suspend/resume.
 */
#ifndef BRAIN_REFRESH_H
#define BRAIN_REFRESH_H

#include <stdint.h>

int brain_refresh_start(void *framebuffer, uint32_t framebuffer_phys,
                        int32_t width, int32_t height, int32_t stride);
void brain_refresh_mark_dirty(int32_t left, int32_t top, int32_t right, int32_t bottom);
void brain_snapshot_lock(void);
void brain_snapshot_unlock(void);
void brain_refresh_stop(void);

void brain_cursor_set_shape(const void *mask_bits, int32_t stride, int32_t width, int32_t cursor_height,
                            int32_t hot_x, int32_t hot_y, int32_t x, int32_t y);
void brain_cursor_move(int32_t x, int32_t y);
void brain_cursor_hide(void);

void brain_refresh_suspend(void);
void brain_refresh_resume(void);
void brain_refresh_push_full(void);
void brain_refresh_set_rotation(int angle, int32_t width, int32_t height, int32_t stride);

void brain_refresh_night_light_preview(int active, uint32_t strength);

int brain_refresh_panel_escapes_enabled(void);
int brain_refresh_panel_command(const uint8_t *bytes, uint32_t length);

#endif
