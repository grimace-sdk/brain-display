/*
 * refresh.c - The refresh thread. Tracks dirty regions, transposes them with
 * the PXP into staging buffers, and sends them over the LCDIF on the TE pulse
 * under a fixed-N cadence scheduler. Also runs present mode: composing app
 * buffers and overlays straight to the panel while GDI updates are held.
 */
#include <windows.h>
#include <grimace/extras.h>
#include <string.h>

#include "board.h"
#include "compose.h"
#include "panel.h"
#include "pxp.h"
#include "raster.h"
#include "refresh.h"
#include "te.h"
#include "timebase.h"
#include "present.h"
#include "nightlight.h"
#include "log.h"

LPVOID AllocPhysMem(DWORD size, DWORD protect, DWORD alignment, DWORD flags, PULONG physical);
void CacheRangeFlush(LPVOID address, DWORD length, DWORD flags);
BOOL CeSetThreadPriority(HANDLE thread, int priority);
#define CACHE_SYNC_WRITEBACK 0x004
#ifndef PAGE_WRITECOMBINE
#define PAGE_WRITECOMBINE 0x400
#endif

/* Refresh thread priority. Default CreateThread threads run at 251; a lower
   number is higher priority, so this puts the (IRQ-blocked) refresh thread
   just above the draw thread so its wakeups preempt it promptly. */
#define BRAIN_REFRESH_PRIORITY 248

#ifdef BRAIN_NO_PXP
static const int g_use_pxp = 0;
#else
static const int g_use_pxp = 1;
#endif

static void *g_framebuffer;
static uint32_t g_framebuffer_phys;
static int32_t g_width;
static int32_t g_height;
static int32_t g_stride;
static int g_angle;

static uint16_t *g_staging[2];
static uint32_t g_staging_phys[2];
static DWORD g_snapshot_enabled = 1;

static CRITICAL_SECTION g_lock;
static CRITICAL_SECTION g_snapshot_lock;
static int g_snapshot_lock_ready;
static int g_dirty_valid;
static raster_rect g_dirty;

#define BRAIN_CADENCE_DEFAULT 1
#define BRAIN_TE_OFFSET_DEFAULT 2
static DWORD g_cadence_enabled = BRAIN_CADENCE_DEFAULT;
static DWORD g_te_offset_ms = BRAIN_TE_OFFSET_DEFAULT;
static DWORD g_te_offset_us = 0;
static DWORD g_log_enabled = 0;

#define BRAIN_NIGHT_WARM_GREEN_DEFAULT 0x18u
#define BRAIN_NIGHT_WARM_BLUE_DEFAULT 0x00u
static brain_night_light_config g_night_cfg = {0, 0, 0, 0, 0, BRAIN_NIGHT_WARM_GREEN_DEFAULT,
                                               BRAIN_NIGHT_WARM_BLUE_DEFAULT};
static volatile LONG g_night_preview_active;
static volatile LONG g_night_preview_strength;
static volatile LONG g_night_dirty;
static DWORD g_night_next;
static uint32_t g_night_last_green = 0xFFFFFFFFu;
static uint32_t g_night_last_blue = 0xFFFFFFFFu;

static DWORD g_panel_escapes;
#define BRAIN_PANEL_CMD_MAX 64
static uint8_t g_panel_cmd[BRAIN_PANEL_CMD_MAX];
static volatile LONG g_panel_cmd_len;
static CRITICAL_SECTION g_panel_cmd_lock;
static HANDLE g_panel_cmd_done;

#define BRAIN_WHOLE_CACHE_BYTES 16384

static HANDLE g_thread;
static HANDLE g_wake;
static volatile LONG g_running;
static volatile LONG g_paused;
static uint32_t g_xfer_fail_streak;
static uint32_t g_te_timeout_streak;

#define BRAIN_PRESENT_CHECK_MS 250
#define BRAIN_PRESENT_WAIT_LIMIT_MS 500
#define BRAIN_PRESENT_WAIT_STEP_MS 8

static CRITICAL_SECTION g_present_lock;
static brain_present_source g_present_source;
static volatile LONG g_presenting;
static uint32_t g_present_owner;
static HANDLE g_present_owner_handle;
static uint32_t g_present_pending;
static uint32_t g_present_reading;
static uint32_t g_present_last_shown;
static uint32_t g_present_queued_us;
static volatile LONG g_present_clear;
static HANDLE g_present_done;
static BrainPresentStats g_present_stats;

#define BRAIN_OVERLAY_HIDDEN 0xFFFFFFFFu
#define BRAIN_OVERLAY_HIDE 0xFFFFFFFEu
static uint32_t g_overlay_shown[BRAIN_PRESENT_MAX_OVERLAYS];
static uint32_t g_overlay_reading[BRAIN_PRESENT_MAX_OVERLAYS];

#define BRAIN_CURSOR_MASK_BYTES 1024

static CRITICAL_SECTION g_cursor_lock;
static int g_cursor_valid;
static int g_cursor_visible;
static int32_t g_cursor_x;
static int32_t g_cursor_y;
static int32_t g_cursor_w;
static int32_t g_cursor_h;
static int32_t g_cursor_hot_x;
static int32_t g_cursor_hot_y;
static int32_t g_cursor_stride;
static uint8_t g_cursor_mask[BRAIN_CURSOR_MASK_BYTES];

static int mask_bit(int32_t x, int32_t row) {
    uint8_t byte = g_cursor_mask[row * g_cursor_stride + (x >> 3)];
    return (byte >> (7 - (x & 7))) & 1;
}

void brain_cursor_set_shape(const void *mask_bits, int32_t stride, int32_t width, int32_t cursor_height,
                            int32_t hot_x, int32_t hot_y, int32_t x, int32_t y) {
    EnterCriticalSection(&g_cursor_lock);
    int32_t old_x = g_cursor_x, old_y = g_cursor_y, old_w = g_cursor_w, old_h = g_cursor_h;
    int old_visible = g_cursor_visible;
    int32_t rows = cursor_height * 2;
    int32_t abs_stride = stride < 0 ? -stride : stride;
    if (mask_bits == NULL || abs_stride * rows > BRAIN_CURSOR_MASK_BYTES) {
        g_cursor_visible = 0;
    } else {
        g_cursor_w = width;
        g_cursor_h = cursor_height;
        g_cursor_stride = abs_stride;
        g_cursor_hot_x = hot_x;
        g_cursor_hot_y = hot_y;
        const uint8_t *src = (const uint8_t *)mask_bits;
        for (int32_t r = 0; r < rows; r++) {
            for (int32_t b = 0; b < abs_stride; b++) {
                g_cursor_mask[r * abs_stride + b] = src[r * stride + b];
            }
        }
        g_cursor_x = x - hot_x;
        g_cursor_y = y - hot_y;
        g_cursor_visible = 1;
        g_cursor_valid = 1;
    }
    LeaveCriticalSection(&g_cursor_lock);
    if (old_visible) {
        brain_refresh_mark_dirty(old_x, old_y, old_x + old_w, old_y + old_h);
    }
    if (g_cursor_visible) {
        brain_refresh_mark_dirty(x - hot_x, y - hot_y, x - hot_x + width, y - hot_y + cursor_height);
    }
}

void brain_cursor_move(int32_t x, int32_t y) {
    EnterCriticalSection(&g_cursor_lock);
    if (!g_cursor_valid || x == -1) {
        int was_visible = g_cursor_visible;
        int32_t ox = g_cursor_x, oy = g_cursor_y, ow = g_cursor_w, oh = g_cursor_h;
        g_cursor_visible = 0;
        LeaveCriticalSection(&g_cursor_lock);
        if (was_visible) {
            brain_refresh_mark_dirty(ox, oy, ox + ow, oy + oh);
        }
        return;
    }
    int32_t old_x = g_cursor_x, old_y = g_cursor_y, w = g_cursor_w, h = g_cursor_h;
    int was_visible = g_cursor_visible;
    g_cursor_x = x - g_cursor_hot_x;
    g_cursor_y = y - g_cursor_hot_y;
    g_cursor_visible = 1;
    int32_t new_x = g_cursor_x, new_y = g_cursor_y;
    LeaveCriticalSection(&g_cursor_lock);
    if (was_visible) {
        brain_refresh_mark_dirty(old_x, old_y, old_x + w, old_y + h);
    }
    brain_refresh_mark_dirty(new_x, new_y, new_x + w, new_y + h);
}

void brain_cursor_hide(void) {
    brain_cursor_move(-1, -1);
}

static void composite_cursor(const brain_panel_window *window, const raster_rect *dirty,
                             uint16_t *staging) {
    EnterCriticalSection(&g_cursor_lock);
    if (g_cursor_visible) {
        for (int32_t cy = 0; cy < g_cursor_h; cy++) {
            int32_t sy = g_cursor_y + cy;
            if (sy < dirty->top || sy >= dirty->bottom) {
                continue;
            }
            for (int32_t cx = 0; cx < g_cursor_w; cx++) {
                int32_t sx = g_cursor_x + cx;
                if (sx < dirty->left || sx >= dirty->right) {
                    continue;
                }
                int32_t pi = sx - window->page_start;
                int32_t panel_col = brain_current_board.mirror_columns
                    ? (brain_current_board.panel_width - 1 - sy) : sy;
                int32_t ci = panel_col - window->col_start;
                if (pi < 0 || pi >= window->page_count || ci < 0 || ci >= window->col_count) {
                    continue;
                }
                int and_bit = mask_bit(cx, cy);
                int xor_bit = mask_bit(cx, cy + g_cursor_h);
                uint16_t *pixel = &staging[(size_t)pi * window->col_count + ci];
                uint16_t value = *pixel;
                if (!and_bit) {
                    value = 0;
                }
                if (xor_bit) {
                    value ^= 0xFFFF;
                }
                *pixel = value;
            }
        }
    }
    LeaveCriticalSection(&g_cursor_lock);
}

void brain_snapshot_lock(void) {
    if (g_snapshot_lock_ready) {
        EnterCriticalSection(&g_snapshot_lock);
    }
}

void brain_snapshot_unlock(void) {
    if (g_snapshot_lock_ready) {
        LeaveCriticalSection(&g_snapshot_lock);
    }
}

void brain_refresh_mark_dirty(int32_t left, int32_t top, int32_t right, int32_t bottom) {
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > g_width) right = g_width;
    if (bottom > g_height) bottom = g_height;
    if (left >= right || top >= bottom) {
        return;
    }
    EnterCriticalSection(&g_lock);
    int transition = !g_dirty_valid;
    if (!g_dirty_valid) {
        g_dirty.left = left;
        g_dirty.top = top;
        g_dirty.right = right;
        g_dirty.bottom = bottom;
        g_dirty_valid = 1;
    } else {
        if (left < g_dirty.left) g_dirty.left = left;
        if (top < g_dirty.top) g_dirty.top = top;
        if (right > g_dirty.right) g_dirty.right = right;
        if (bottom > g_dirty.bottom) g_dirty.bottom = bottom;
    }
    LeaveCriticalSection(&g_lock);
    if (transition && g_wake != NULL) {
        SetEvent(g_wake);
    }
}

static int take_dirty(raster_rect *out) {
    int valid;
    EnterCriticalSection(&g_lock);
    valid = g_dirty_valid;
    if (valid) {
        *out = g_dirty;
        g_dirty_valid = 0;
    }
    LeaveCriticalSection(&g_lock);
    return valid;
}

static int dirty_pending(void) {
    int valid;
    EnterCriticalSection(&g_lock);
    valid = g_dirty_valid;
    LeaveCriticalSection(&g_lock);
    return valid;
}

static int use_pxp(void) {
    return g_use_pxp && g_framebuffer_phys != 0 && brain_pxp_available();
}

static void send_setup(const brain_panel_window *window) {
    brain_panel_set_window(window->col_start, window->col_count,
                           window->page_start, window->page_count);
}

static void send_kick(const brain_panel_window *window, int buf) {
    brain_panel_transfer_start(g_staging_phys[buf], window->col_count, window->page_count);
}

static void send_wait(void) {
    if (!brain_panel_transfer_wait()) {
        g_xfer_fail_streak++;
        brain_log("refresh: transfer timeout");
        brain_log_value("refresh: CTRL", brain_panel_reg_ctrl());
        brain_log_value("refresh: STAT", brain_panel_reg_stat());
        brain_panel_recover(g_xfer_fail_streak);
        if (g_xfer_fail_streak >= 2) {
            brain_refresh_mark_dirty(0, 0, g_width, g_height);
        }
    } else {
        g_xfer_fail_streak = 0;
    }
}

static uint32_t g_copy_sum, g_copy_max;
static uint32_t g_lockwait_sum, g_lockwait_max;
static uint32_t g_pxp_sum, g_pxp_max;
static uint32_t g_offset_sum, g_offset_max;
static uint32_t g_send_sum, g_send_max;
static uint32_t g_stat_count;
static uint32_t g_prepare_count;

static void stat_add(uint32_t *sum, uint32_t *max, uint32_t v) {
    *sum += v;
    if (v > *max) {
        *max = v;
    }
}

static int prepare_region(raster_rect dirty, int buf, brain_panel_window *window) {
    EnterCriticalSection(&g_lock);
    int32_t width = g_width;
    int32_t height = g_height;
    int32_t stride = g_stride;
    int angle = g_angle;
    LeaveCriticalSection(&g_lock);

    if (dirty.left < 0) dirty.left = 0;
    if (dirty.top < 0) dirty.top = 0;
    if (dirty.right > width) dirty.right = width;
    if (dirty.bottom > height) dirty.bottom = height;
    if (dirty.left >= dirty.right || dirty.top >= dirty.bottom) {
        return 0;
    }

    if (use_pxp()) {
        dirty.left &= ~7;
        dirty.top &= ~7;
        dirty.right = (dirty.right + 7) & ~7;
        dirty.bottom = (dirty.bottom + 7) & ~7;
        if (dirty.right > width) dirty.right = width;
        if (dirty.bottom > height) dirty.bottom = height;

        brain_compose_window(&brain_current_board, &dirty, angle, window);
        uint32_t tl0 = brain_us();
        if (g_snapshot_enabled) {
            brain_snapshot_lock();
        }
        uint32_t tc0 = brain_us();
        if (g_framebuffer != NULL) {
            int full_width = (dirty.left == 0 && dirty.right == width);
            size_t row_offset = (size_t)dirty.left * 2;
            DWORD row_bytes = (DWORD)((dirty.right - dirty.left) * 2);
            size_t total_bytes = (size_t)(dirty.bottom - dirty.top) *
                                 (full_width ? (size_t)stride : (size_t)row_bytes);
            if (total_bytes > BRAIN_WHOLE_CACHE_BYTES) {
                CacheRangeFlush(NULL, 0, CACHE_SYNC_WRITEBACK);
            } else if (full_width) {
                CacheRangeFlush((uint8_t *)g_framebuffer + (size_t)dirty.top * stride,
                                (DWORD)((dirty.bottom - dirty.top) * stride), CACHE_SYNC_WRITEBACK);
            } else {
                for (int32_t fy = dirty.top; fy < dirty.bottom; fy++) {
                    CacheRangeFlush((uint8_t *)g_framebuffer + (size_t)fy * stride + row_offset,
                                    row_bytes, CACHE_SYNC_WRITEBACK);
                }
            }
        }
        uint32_t tc1 = brain_us();
        brain_pxp_transpose(g_framebuffer_phys, stride / 2, &dirty, g_staging_phys[buf],
                            angle, brain_current_board.mirror_columns);
        uint32_t tp1 = brain_us();
        if (g_snapshot_enabled) {
            brain_snapshot_unlock();
        }
        stat_add(&g_lockwait_sum, &g_lockwait_max, tc0 - tl0);
        stat_add(&g_copy_sum, &g_copy_max, tc1 - tc0);
        stat_add(&g_pxp_sum, &g_pxp_max, tp1 - tc1);
        g_prepare_count++;
        composite_cursor(window, &dirty, g_staging[buf]);
        return 1;
    }

    raster_surface primary;
    primary.bits = g_framebuffer;
    primary.width = width;
    primary.height = height;
    primary.stride = stride;
    primary.format = RASTER_FORMAT_16BPP;

    brain_snapshot_lock();
    brain_compose(&brain_current_board, &primary, &dirty, angle, g_staging[buf], window);
    brain_snapshot_unlock();
    composite_cursor(window, &dirty, g_staging[buf]);
    return 1;
}

static void read_settle_config(void);
static void night_light_tick(void);
static void panel_command_tick(void);

static void refresh_report(uint32_t *ticks, uint32_t *sends, int n_pending, DWORD *report_at) {
    DWORD now = GetTickCount();
    if ((int32_t)(now - *report_at) < 0) {
        return;
    }
    if (g_log_enabled && (*ticks != 0 || *sends != 0)) {
        brain_log_value("refresh: ticks/s", *ticks);
        brain_log_value("refresh: sends/s", *sends);
        brain_log_value("refresh: n", (unsigned long)n_pending);
        brain_log_value("refresh: te period us", brain_te_period_us());
    }
    if (g_log_enabled && g_prepare_count != 0) {
        uint32_t pxp_irq, pxp_spin;
        brain_pxp_path_counts(&pxp_irq, &pxp_spin);
        brain_log_value("refresh: pxp irq/s", pxp_irq);
        brain_log_value("refresh: pxp spin/s", pxp_spin);
        brain_log_value("refresh: lockwait us avg", g_lockwait_sum / g_prepare_count);
        brain_log_value("refresh: lockwait us max", g_lockwait_max);
        brain_log_value("refresh: copy us avg", g_copy_sum / g_prepare_count);
        brain_log_value("refresh: copy us max", g_copy_max);
        brain_log_value("refresh: pxp us avg", g_pxp_sum / g_prepare_count);
        brain_log_value("refresh: pxp us max", g_pxp_max);
    }
    if (g_log_enabled && g_stat_count != 0) {
        brain_log_value("refresh: send us avg", g_send_sum / g_stat_count);
        brain_log_value("refresh: send us max", g_send_max);
        brain_log_value("refresh: offset us avg", g_offset_sum / g_stat_count);
        brain_log_value("refresh: offset us max", g_offset_max);
    }
    *ticks = 0;
    *sends = 0;
    g_lockwait_sum = g_lockwait_max = 0;
    g_copy_sum = g_copy_max = 0;
    g_pxp_sum = g_pxp_max = 0;
    g_send_sum = g_send_max = 0;
    g_offset_sum = g_offset_max = 0;
    g_stat_count = 0;
    g_prepare_count = 0;
    *report_at = now + 1000;
    read_settle_config();
    brain_log_flush();
}

static int prepare_timed(raster_rect dirty, int buf, brain_panel_window *window,
                         uint32_t *prepare_us) {
    uint32_t p0 = brain_us();
    int ok = prepare_region(dirty, buf, window);
    *prepare_us = brain_us() - p0;
    return ok;
}

static uint32_t take_present(uint32_t *queued_us) {
    uint32_t index;
    EnterCriticalSection(&g_present_lock);
    index = g_present_pending;
    *queued_us = g_present_queued_us;
    g_present_pending = BRAIN_PRESENT_NONE;
    g_present_reading = index;
    LeaveCriticalSection(&g_present_lock);
    return index;
}

static void finish_present_read(void) {
    uint32_t n;
    EnterCriticalSection(&g_present_lock);
    g_present_reading = BRAIN_PRESENT_NONE;
    for (n = 0; n < BRAIN_PRESENT_MAX_OVERLAYS; n++) {
        g_overlay_reading[n] = BRAIN_OVERLAY_HIDDEN;
    }
    LeaveCriticalSection(&g_present_lock);
    SetEvent(g_present_done);
}

static void build_overlay_set(brain_pxp_overlay_set *set) {
    uint32_t n;
    EnterCriticalSection(&g_present_lock);
    set->count = g_present_source.overlays;
    set->colour_key_low = g_present_source.colour_key_low;
    set->colour_key_high = g_present_source.colour_key_high;
    for (n = 0; n < g_present_source.overlays && n < BRAIN_PXP_MAX_OVERLAYS; n++) {
        uint32_t shown = g_overlay_shown[n];
        g_overlay_reading[n] = shown;
        if (shown < g_present_source.overlay[n].buffers) {
            set->overlay[n].physical = g_present_source.overlay[n].physical[shown];
            set->overlay[n].size = g_present_source.overlay[n].size;
            set->overlay[n].parameters = g_present_source.overlay[n].parameters;
        } else {
            set->overlay[n].physical = 0;
            set->overlay[n].size = 0;
            set->overlay[n].parameters = 0;
        }
    }
    LeaveCriticalSection(&g_present_lock);
}

static int prepare_present(uint32_t index, int buf, brain_panel_window *window) {
    int32_t pw = (int32_t)g_present_source.width;
    int32_t ph = (int32_t)g_present_source.height;
    int32_t left = ((g_width - pw) / 2) & ~7;
    int32_t top = ((g_height - ph) / 2) & ~7;
    raster_rect dest;
    raster_rect src;
    brain_pxp_overlay_set overlays;
    if (!use_pxp()) {
        return 0;
    }
    dest.left = left;
    dest.top = top;
    dest.right = left + pw;
    dest.bottom = top + ph;
    src.left = 0;
    src.top = 0;
    src.right = pw;
    src.bottom = ph;
    brain_compose_window(&brain_current_board, &dest, g_angle, window);
    build_overlay_set(&overlays);
    uint32_t t0 = brain_us();
    brain_pxp_transpose_overlays(g_present_source.physical[index], pw, &src, g_staging_phys[buf],
                                 g_angle, brain_current_board.mirror_columns, &overlays);
    EnterCriticalSection(&g_present_lock);
    g_present_stats.PxpMicroseconds += brain_us() - t0;
    LeaveCriticalSection(&g_present_lock);
    return 1;
}

static void clear_present_panel(void) {
    raster_rect full;
    brain_panel_window window;
    uint16_t background;
    size_t pixels;
    size_t i;

    full.left = 0;
    full.top = 0;
    full.right = g_width;
    full.bottom = g_height;
    brain_compose_window(&brain_current_board, &full, g_angle, &window);
    background = (uint16_t)g_present_source.background;
    pixels = (size_t)window.col_count * window.page_count;
    for (i = 0; i < pixels; i++) {
        g_staging[0][i] = background;
    }
    send_setup(&window);
    send_kick(&window, 0);
    send_wait();
}

static int present_owner_exited(void) {
    DWORD code;
    if (g_present_owner == 0) {
        return 0;
    }
    if (g_present_owner_handle == NULL) {
        g_present_owner_handle = OpenProcess(0, FALSE, g_present_owner);
        if (g_present_owner_handle == NULL) {
            return 0;
        }
    }
    return GetExitCodeProcess(g_present_owner_handle, &code) && code != STILL_ACTIVE;
}

int brain_refresh_begin_present(const brain_present_source *source, uint32_t owner_id) {
    int started = 0;
    if (g_thread == NULL) {
        return 0;
    }
    if (source->width > (uint32_t)g_width || source->height > (uint32_t)g_height) {
        return 0;
    }
    EnterCriticalSection(&g_present_lock);
    if (!g_presenting) {
        uint32_t n;
        g_present_source = *source;
        g_present_clear = 1;
        g_present_pending = BRAIN_PRESENT_NONE;
        g_present_reading = BRAIN_PRESENT_NONE;
        g_present_last_shown = BRAIN_PRESENT_NONE;
        for (n = 0; n < BRAIN_PRESENT_MAX_OVERLAYS; n++) {
            g_overlay_shown[n] = BRAIN_OVERLAY_HIDDEN;
            g_overlay_reading[n] = BRAIN_OVERLAY_HIDDEN;
        }
        memset(&g_present_stats, 0, sizeof(g_present_stats));
        g_present_owner = owner_id;
        InterlockedExchange(&g_presenting, 1);
        started = 1;
    }
    LeaveCriticalSection(&g_present_lock);
    if (!started) {
        return 0;
    }
    if (source->flags & BRAIN_PRESENT_FAST_CPU) {
        brain_present_hold_cpu(1);
    }
    SetEvent(g_wake);
    return 1;
}

void brain_refresh_end_present(void) {
    if (g_thread == NULL) {
        return;
    }
    uint32_t n;
    InterlockedExchange(&g_presenting, 0);
    EnterCriticalSection(&g_present_lock);
    g_present_pending = BRAIN_PRESENT_NONE;
    g_present_reading = BRAIN_PRESENT_NONE;
    g_present_last_shown = BRAIN_PRESENT_NONE;
    for (n = 0; n < BRAIN_PRESENT_MAX_OVERLAYS; n++) {
        g_overlay_shown[n] = BRAIN_OVERLAY_HIDDEN;
        g_overlay_reading[n] = BRAIN_OVERLAY_HIDDEN;
    }
    LeaveCriticalSection(&g_present_lock);
    SetEvent(g_present_done);
    brain_present_hold_cpu(0);
    brain_refresh_mark_dirty(0, 0, g_width, g_height);
    SetEvent(g_wake);
}

int brain_refresh_presenting(void) {
    return g_presenting;
}

/* Compose the last presented frame into a screen-space buffer so a GDI
   screenshot can be answered while present mode holds the primary. Frame only,
   centred on the background, matching the panel minus overlays. Holds the
   present lock so the source buffer cannot be freed mid-copy; a screenshot is
   rare, so the brief stall of the present thread is acceptable. */
int brain_refresh_capture_present(uint16_t *dest, int32_t width, int32_t height, int32_t stride_bytes) {
    int ok = 0;
    if (dest == NULL || width <= 0 || height <= 0 || stride_bytes < width * 2) {
        return 0;
    }
    int32_t pitch = stride_bytes / 2;
    EnterCriticalSection(&g_present_lock);
    if (g_presenting && g_present_last_shown != BRAIN_PRESENT_NONE) {
        const uint16_t *src = (const uint16_t *)g_present_source.kernel[g_present_last_shown];
        int32_t pw = (int32_t)g_present_source.width;
        int32_t ph = (int32_t)g_present_source.height;
        uint16_t background = (uint16_t)g_present_source.background;
        if (src != NULL && pw > 0 && ph > 0) {
            for (int32_t y = 0; y < height; y++) {
                uint16_t *row = dest + (size_t)y * pitch;
                for (int32_t x = 0; x < width; x++) {
                    row[x] = background;
                }
            }
            int32_t left = (width - pw) / 2;
            int32_t top = (height - ph) / 2;
            if (left < 0) left = 0;
            if (top < 0) top = 0;
            int32_t copy_w = pw < width ? pw : width;
            int32_t copy_h = ph < height ? ph : height;
            for (int32_t y = 0; y < copy_h; y++) {
                const uint16_t *src_row = src + (size_t)y * pw;
                uint16_t *row = dest + (size_t)(top + y) * pitch + left;
                for (int32_t x = 0; x < copy_w; x++) {
                    row[x] = src_row[x];
                }
            }
            ok = 1;
        }
    }
    LeaveCriticalSection(&g_present_lock);
    return ok;
}

int brain_refresh_queue_present(uint32_t index, const uint32_t *overlays, uint32_t *replaced) {
    uint32_t n;
    if (!g_presenting || index >= g_present_source.buffers) {
        return 0;
    }
    EnterCriticalSection(&g_present_lock);
    for (n = 0; overlays != NULL && n < g_present_source.overlays &&
                n < BRAIN_PRESENT_MAX_OVERLAYS; n++) {
        if (overlays[n] == BRAIN_OVERLAY_HIDE) {
            g_overlay_shown[n] = BRAIN_OVERLAY_HIDDEN;
        } else if (overlays[n] != BRAIN_PRESENT_KEEP) {
            g_overlay_shown[n] = overlays[n];
        }
    }
    if (replaced != NULL) {
        *replaced = g_present_pending;
    }
    g_present_stats.Submitted++;
    if (g_present_pending != BRAIN_PRESENT_NONE) {
        g_present_stats.Replaced++;
    }
    g_present_pending = index;
    g_present_queued_us = brain_us();
    LeaveCriticalSection(&g_present_lock);
    SetEvent(g_wake);
    return 1;
}

int brain_refresh_wait_present(uint32_t index) {
    DWORD first = GetTickCount();
    for (;;) {
        int busy;
        EnterCriticalSection(&g_present_lock);
        busy = g_presenting && (g_present_pending == index || g_present_reading == index);
        LeaveCriticalSection(&g_present_lock);
        if (!busy) {
            return 1;
        }
        if (GetTickCount() - first >= BRAIN_PRESENT_WAIT_LIMIT_MS) {
            return 0;
        }
        WaitForSingleObject(g_present_done, BRAIN_PRESENT_WAIT_STEP_MS);
    }
}

int brain_refresh_wait_overlay(uint32_t overlay, uint32_t buffer) {
    DWORD first = GetTickCount();
    if (overlay >= BRAIN_PRESENT_MAX_OVERLAYS) {
        return 0;
    }
    for (;;) {
        int busy;
        EnterCriticalSection(&g_present_lock);
        busy = g_presenting &&
               (g_overlay_shown[overlay] == buffer || g_overlay_reading[overlay] == buffer);
        LeaveCriticalSection(&g_present_lock);
        if (!busy) {
            return 1;
        }
        if (GetTickCount() - first >= BRAIN_PRESENT_WAIT_LIMIT_MS) {
            return 0;
        }
        WaitForSingleObject(g_present_done, BRAIN_PRESENT_WAIT_STEP_MS);
    }
}

void brain_refresh_add_clean_time(uint32_t microseconds) {
    EnterCriticalSection(&g_present_lock);
    g_present_stats.CleanMicroseconds += microseconds;
    LeaveCriticalSection(&g_present_lock);
}

void brain_refresh_get_present_stats(BrainPresentStats *stats, int reset) {
    EnterCriticalSection(&g_present_lock);
    g_present_stats.State = (DWORD)((g_presenting ? BRAIN_PRESENT_PRESENTING : 0) |
                                    (g_paused ? BRAIN_PRESENT_SUSPENDED : 0) |
                                    (brain_te_available() ? BRAIN_PRESENT_TE : 0));
    g_present_stats.Orientation = (DWORD)g_angle;
    *stats = g_present_stats;
    if (reset) {
        g_present_stats.Frames = 0;
        g_present_stats.Replaced = 0;
        g_present_stats.Dropped = 0;
        g_present_stats.CleanMicroseconds = 0;
        g_present_stats.PxpMicroseconds = 0;
        g_present_stats.LatencyMicroseconds = 0;
        g_present_stats.Submitted = 0;
        g_present_stats.TeTimeouts = 0;
    }
    LeaveCriticalSection(&g_present_lock);
}

static int copy_next_present(int buf, brain_panel_window *window, uint32_t *queued_us) {
    uint32_t q;
    uint32_t index = take_present(&q);
    int copied = -1;
    if (index != BRAIN_PRESENT_NONE) {
        if (prepare_present(index, buf, window)) {
            copied = buf;
            *queued_us = q;
            EnterCriticalSection(&g_present_lock);
            g_present_last_shown = index;
            LeaveCriticalSection(&g_present_lock);
        } else {
            EnterCriticalSection(&g_present_lock);
            g_present_stats.Dropped++;
            LeaveCriticalSection(&g_present_lock);
        }
        finish_present_read();
    }
    return copied;
}

static void run_present(HANDLE te_tick) {
    int cur = 0;
    int ready = -1;
    brain_panel_window window;
    brain_panel_window next_window;
    uint32_t ready_queued_us = 0;
    uint32_t next_queued_us = 0;
    DWORD last_check = GetTickCount();

    brain_te_arm();
    if (g_present_clear) {
        clear_present_panel();
        InterlockedExchange(&g_present_clear, 0);
    }
    while (g_running && g_presenting && !g_paused) {
        if (ready < 0) {
            ready = copy_next_present(cur, &window, &ready_queued_us);
            if (ready >= 0) {
                cur ^= 1;
            }
        }

        if (ready < 0) {
            WaitForSingleObject(g_wake, BRAIN_PRESENT_CHECK_MS);
        } else {
            uint32_t period = brain_te_period_us();
            DWORD tick_timeout = period ? (period / 1000) * 2 + 4 : 50;
            if (WaitForSingleObject(te_tick, tick_timeout) != WAIT_OBJECT_0) {
                EnterCriticalSection(&g_present_lock);
                g_present_stats.TeTimeouts++;
                LeaveCriticalSection(&g_present_lock);
            }
            if (!g_running || !g_presenting || g_paused) {
                break;
            }
            send_setup(&window);
            uint32_t latency = brain_us() - ready_queued_us;
            send_kick(&window, ready);
            int next = copy_next_present(cur, &next_window, &next_queued_us);
            send_wait();
            EnterCriticalSection(&g_present_lock);
            g_present_stats.Frames++;
            g_present_stats.LatencyMicroseconds += latency;
            LeaveCriticalSection(&g_present_lock);
            if (next >= 0) {
                cur ^= 1;
                window = next_window;
                ready_queued_us = next_queued_us;
            }
            ready = next;
        }

        if (GetTickCount() - last_check >= BRAIN_PRESENT_CHECK_MS) {
            last_check = GetTickCount();
            if (present_owner_exited()) {
                brain_log("refresh: present owner exited");
                brain_refresh_end_present();
            }
        }
    }
    brain_te_disarm();
    if (g_present_owner_handle != NULL) {
        CloseHandle(g_present_owner_handle);
        g_present_owner_handle = NULL;
    }
}

static DWORD WINAPI refresh_thread(LPVOID parameter) {
    (void)parameter;
    CeSetThreadPriority(GetCurrentThread(), BRAIN_REFRESH_PRIORITY);
    brain_log("refresh: thread start");
    uint32_t ticks = 0;
    uint32_t sends = 0;
    DWORD report_at = GetTickCount() + 1000;

    int cur = 0;
    int prepared = 0;
    int armed = 0;
    int n_pending = 1;
    int n_hyst = 0;
    uint32_t last_prepare_us = 0;
    uint32_t last_send_us = 0;
    uint32_t last_send_edge = 0;
    brain_panel_window windows[2];

    HANDLE te_tick = brain_te_tick_handle();

    while (g_running) {
        night_light_tick();
        panel_command_tick();
        if (g_presenting && !g_paused) {
            if (armed) {
                brain_te_disarm();
                armed = 0;
            }
            prepared = 0;
            run_present(te_tick);
            continue;
        }
        if (g_paused || (!prepared && !dirty_pending())) {
            if (armed) {
                brain_te_disarm();
                armed = 0;
            }
            if (g_paused) {
                prepared = 0;
            }
            refresh_report(&ticks, &sends, n_pending, &report_at);
            WaitForSingleObject(g_wake, 1000);
            continue;
        }

        if (!armed) {
            brain_te_arm();
            last_send_edge = brain_te_count() - 2;
            armed = 1;
        }

        if (brain_current_board.te_prepare_ahead && !prepared && dirty_pending()) {
            raster_rect dirty;
            if (take_dirty(&dirty)) {
                prepared = prepare_timed(dirty, cur, &windows[cur], &last_prepare_us);
            }
        }

        uint32_t period_us = brain_te_period_us();
        DWORD tick_timeout = period_us ? (period_us / 1000) * 2 + 4 : 50;
        DWORD wr = WaitForSingleObject(te_tick, tick_timeout);
        if (!g_running) {
            break;
        }
        if (g_paused) {
            continue;
        }
        ticks++;

        int force = (wr != WAIT_OBJECT_0);
        if (force) {
            if (++g_te_timeout_streak >= 5) {
                brain_te_recover();
                brain_panel_recover(3);
                brain_refresh_mark_dirty(0, 0, g_width, g_height);
                brain_te_arm();
                last_send_edge = brain_te_count() - 2;
                g_te_timeout_streak = 0;
            }
        } else {
            g_te_timeout_streak = 0;
        }

        refresh_report(&ticks, &sends, n_pending, &report_at);

        int n = g_cadence_enabled ? n_pending : 1;
        uint32_t te_now = brain_te_count();
        uint32_t edges_since = te_now - last_send_edge;

        if (!force) {
            if (edges_since + 1 < (uint32_t)n) {
                continue;
            }
            if (n >= 2 && edges_since + 1 == (uint32_t)n) {
                if (!prepared) {
                    raster_rect dirty;
                    if (take_dirty(&dirty)) {
                        prepared = prepare_timed(dirty, cur, &windows[cur], &last_prepare_us);
                    }
                }
                continue;
            }
            if (edges_since < (uint32_t)n) {
                continue;
            }
        }

        if (!prepared) {
            raster_rect dirty;
            if (!take_dirty(&dirty)) {
                continue;
            }
            prepared = prepare_timed(dirty, cur, &windows[cur], &last_prepare_us);
            if (!prepared) {
                continue;
            }
        }

        send_setup(&windows[cur]);

        uint32_t edge_count, edge_us;
        brain_te_last(&edge_count, &edge_us);
        if (g_te_offset_us != 0) {
            while (brain_us() - edge_us < g_te_offset_us) {
            }
        } else if (g_te_offset_ms != 0) {
            Sleep(g_te_offset_ms);
        }
        stat_add(&g_offset_sum, &g_offset_max, brain_us() - edge_us);

        send_kick(&windows[cur], cur);
        last_send_edge = brain_te_count();

        uint32_t s0 = brain_us();
        send_wait();
        last_send_us = brain_us() - s0;
        stat_add(&g_send_sum, &g_send_max, last_send_us);
        g_stat_count++;
        sends++;
        prepared = 0;
        cur ^= 1;

        if (g_cadence_enabled) {
            uint32_t budget = period_us ? (period_us * 7 / 10) : 11000;
            uint32_t cycle = last_prepare_us + last_send_us;
            if (n_pending == 1) {
                if (cycle > budget) {
                    n_pending = 2;
                    n_hyst = 0;
                }
            } else if (cycle <= budget) {
                if (++n_hyst >= 2) {
                    n_pending = 1;
                    n_hyst = 0;
                }
            } else {
                n_hyst = 0;
            }
        }
    }
    brain_te_disarm();
    brain_log("refresh: thread stop");
    return 0;
}

void brain_refresh_suspend(void) {
    g_paused = 1;
    Sleep(150);
}

void brain_refresh_push_full(void) {
    raster_rect full = {0, 0, g_width, g_height};
    brain_panel_window window;
    brain_log("push full: prepare");
    if (prepare_region(full, 0, &window)) {
        brain_log("push full: send");
        send_setup(&window);
        send_kick(&window, 0);
        send_wait();
        brain_log("push full: sent");
    }
}

void brain_refresh_resume(void) {
    g_paused = 0;
    brain_refresh_mark_dirty(0, 0, g_width, g_height);
}

void brain_refresh_set_rotation(int angle, int32_t width, int32_t height, int32_t stride) {
    g_paused = 1;
    Sleep(60);
    EnterCriticalSection(&g_lock);
    g_angle = angle;
    g_width = width;
    g_height = height;
    g_stride = stride;
    g_dirty_valid = 0;
    LeaveCriticalSection(&g_lock);
    if (g_framebuffer != NULL) {
        memset(g_framebuffer, 0, (size_t)stride * height);
        CacheRangeFlush(g_framebuffer, (DWORD)(stride * height), CACHE_SYNC_WRITEBACK);
    }
    g_xfer_fail_streak = 0;
    g_te_timeout_streak = 0;
    g_paused = 0;
    brain_log_value("refresh: rotation angle", (unsigned long)angle);
}

static DWORD read_reg_dword(HKEY key, const WCHAR *name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        return value;
    }
    return fallback;
}

static void read_settle_config(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Brain\\Display", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        g_snapshot_enabled = read_reg_dword(key, L"Snapshot", 1);
        g_cadence_enabled = read_reg_dword(key, L"Cadence", BRAIN_CADENCE_DEFAULT);
        g_te_offset_ms = read_reg_dword(key, L"TeOffsetMs", (DWORD)brain_current_board.te_offset_ms);
        g_te_offset_us = read_reg_dword(key, L"TeOffsetUs", 0);
        g_log_enabled = read_reg_dword(key, L"Log", 0);
        g_panel_escapes = read_reg_dword(key, L"PanelEscapes", 0);
        RegCloseKey(key);
    }
    brain_log_set_debug((int)g_log_enabled);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Brain\\NightLight", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        g_night_cfg.enabled = (int)read_reg_dword(key, L"Enabled", 0);
        g_night_cfg.start_min = read_reg_dword(key, L"Start", 0);
        g_night_cfg.end_min = read_reg_dword(key, L"End", 0);
        g_night_cfg.transition_min = read_reg_dword(key, L"Transition", 0);
        g_night_cfg.strength = read_reg_dword(key, L"Strength", 0);
        g_night_cfg.warm_green = read_reg_dword(key, L"WarmGreen", BRAIN_NIGHT_WARM_GREEN_DEFAULT);
        g_night_cfg.warm_blue = read_reg_dword(key, L"WarmBlue", BRAIN_NIGHT_WARM_BLUE_DEFAULT);
        RegCloseKey(key);
    } else {
        g_night_cfg.enabled = 0;
    }
}

static void night_light_tick(void) {
    DWORD now = GetTickCount();
    int forced = InterlockedExchange(&g_night_dirty, 0);
    if (!forced && (int32_t)(now - g_night_next) < 0) {
        return;
    }
    g_night_next = now + 1000;

    uint32_t strength;
    if (InterlockedCompareExchange(&g_night_preview_active, 0, 0)) {
        strength = (uint32_t)InterlockedCompareExchange(&g_night_preview_strength, 0, 0);
    } else {
        SYSTEMTIME local;
        GetLocalTime(&local);
        uint32_t minutes = (uint32_t)local.wHour * 60u + (uint32_t)local.wMinute;
        strength = brain_night_light_schedule_strength(&g_night_cfg, minutes);
    }

    uint32_t green, blue;
    brain_night_light_gains(strength, g_night_cfg.warm_green, g_night_cfg.warm_blue, &green, &blue);
    if (green != g_night_last_green || blue != g_night_last_blue) {
        g_night_last_green = green;
        g_night_last_blue = blue;
        brain_panel_night_light(green, blue);
    }
}

void brain_refresh_night_light_preview(int active, uint32_t strength) {
    InterlockedExchange(&g_night_preview_strength, (LONG)strength);
    InterlockedExchange(&g_night_preview_active, active ? 1 : 0);
    InterlockedExchange(&g_night_dirty, 1);
    if (g_wake != NULL) {
        SetEvent(g_wake);
    }
}

int brain_refresh_panel_escapes_enabled(void) {
    return g_panel_escapes != 0;
}

/* Run a raw panel command on the refresh thread, which owns the panel, so it
   cannot collide with a frame transfer. Blocks until the refresh thread has
   sent it. A probe tool, gated by the PanelEscapes registry setting. */
int brain_refresh_panel_command(const uint8_t *bytes, uint32_t length) {
    if (length == 0 || length > BRAIN_PANEL_CMD_MAX || !g_running) {
        return 0;
    }
    EnterCriticalSection(&g_panel_cmd_lock);
    memcpy(g_panel_cmd, bytes, length);
    ResetEvent(g_panel_cmd_done);
    InterlockedExchange(&g_panel_cmd_len, (LONG)length);
    SetEvent(g_wake);
    DWORD wr = WaitForSingleObject(g_panel_cmd_done, 200);
    LeaveCriticalSection(&g_panel_cmd_lock);
    return wr == WAIT_OBJECT_0;
}

static void panel_command_tick(void) {
    LONG length = InterlockedExchange(&g_panel_cmd_len, 0);
    if (length <= 0) {
        return;
    }
    brain_panel_command_raw(g_panel_cmd, (uint32_t)length);
    if (g_panel_cmd_done != NULL) {
        SetEvent(g_panel_cmd_done);
    }
}

int brain_refresh_start(void *framebuffer, uint32_t framebuffer_phys,
                        int32_t width, int32_t height, int32_t stride) {
    read_settle_config();
    g_framebuffer = framebuffer;
    g_framebuffer_phys = framebuffer_phys;
    g_width = width;
    g_height = height;
    g_stride = stride;

    DWORD staging_bytes = (DWORD)brain_current_board.panel_width * brain_current_board.panel_height * 2;
    for (int i = 0; i < 2; i++) {
        ULONG phys = 0;
        void *cached = AllocPhysMem(staging_bytes, PAGE_READWRITE, 0, 0, &phys);
        if (cached == NULL) {
            brain_log("refresh: AllocPhysMem failed");
            return 0;
        }
        void *uncached = VirtualAlloc(NULL, staging_bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (uncached == NULL || !VirtualCopy(uncached, (LPVOID)(phys >> 8), staging_bytes,
                                             PAGE_READWRITE | PAGE_PHYSICAL | PAGE_NOCACHE)) {
            brain_log("refresh: staging map failed");
            return 0;
        }
        g_staging[i] = (uint16_t *)uncached;
        g_staging_phys[i] = phys;
        brain_log_value("refresh: staging phys", phys);
    }

    brain_timebase_init();
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_snapshot_lock);
    InitializeCriticalSection(&g_present_lock);
    g_present_pending = BRAIN_PRESENT_NONE;
    g_present_reading = BRAIN_PRESENT_NONE;
    g_present_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_snapshot_lock_ready = 1;
    InitializeCriticalSection(&g_cursor_lock);
    InitializeCriticalSection(&g_panel_cmd_lock);
    g_panel_cmd_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_dirty.left = 0;
    g_dirty.top = 0;
    g_dirty.right = width;
    g_dirty.bottom = height;
    g_dirty_valid = 1;

    g_wake = CreateEventW(NULL, FALSE, TRUE, NULL);
    if (g_wake == NULL) {
        brain_log("refresh: CreateEvent failed");
        return 0;
    }

    brain_te_init();
    if (g_use_pxp && g_framebuffer_phys != 0) {
        brain_pxp_init();
    }

    g_running = 1;
    g_thread = CreateThread(NULL, 0, refresh_thread, NULL, 0, NULL);
    if (g_thread == NULL) {
        g_running = 0;
        brain_log("refresh: CreateThread failed");
        return 0;
    }
    return 1;
}

void brain_refresh_stop(void) {
    g_running = 0;
    if (g_wake != NULL) {
        SetEvent(g_wake);
    }
    if (g_thread != NULL) {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}
