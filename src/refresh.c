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
#include "log.h"

LPVOID AllocPhysMem(DWORD size, DWORD protect, DWORD alignment, DWORD flags, PULONG physical);
void CacheRangeFlush(LPVOID address, DWORD length, DWORD flags);
BOOL CeSetThreadPriority(HANDLE thread, int priority);
#define CACHE_SYNC_WRITEBACK 0x004

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

static uint16_t *g_staging;
static uint32_t g_staging_phys;

static CRITICAL_SECTION g_lock;
static int g_dirty_valid;
static raster_rect g_dirty;

static HANDLE g_thread;
static HANDLE g_wake;
static volatile LONG g_running;
static volatile LONG g_paused;
static uint32_t g_xfer_fail_streak;
static uint32_t g_te_timeout_streak;

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

static void composite_cursor(const brain_panel_window *window, const raster_rect *dirty) {
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
                uint16_t *pixel = &g_staging[(size_t)pi * window->col_count + ci];
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

static int use_pxp(void) {
    return g_use_pxp && g_framebuffer_phys != 0 && brain_pxp_available();
}

static void transfer_window(const brain_panel_window *window) {
    brain_panel_set_window(window->col_start, window->col_count,
                           window->page_start, window->page_count);
    if (!brain_panel_transfer(g_staging_phys, window->col_count, window->page_count)) {
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

static void push_region(raster_rect dirty) {
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
        return;
    }

    brain_panel_window window;

    if (use_pxp()) {
        dirty.left &= ~7;
        dirty.top &= ~7;
        dirty.right = (dirty.right + 7) & ~7;
        dirty.bottom = (dirty.bottom + 7) & ~7;
        if (dirty.right > width) dirty.right = width;
        if (dirty.bottom > height) dirty.bottom = height;

        brain_compose_window(&brain_current_board, &dirty, angle, &window);
        if (g_framebuffer != NULL) {
            if (dirty.left == 0 && dirty.right == width) {
                CacheRangeFlush((uint8_t *)g_framebuffer + (size_t)dirty.top * stride,
                                (DWORD)((dirty.bottom - dirty.top) * stride), CACHE_SYNC_WRITEBACK);
            } else {
                size_t row_offset = (size_t)dirty.left * 2;
                DWORD row_bytes = (DWORD)((dirty.right - dirty.left) * 2);
                for (int32_t fy = dirty.top; fy < dirty.bottom; fy++) {
                    CacheRangeFlush((uint8_t *)g_framebuffer + (size_t)fy * stride + row_offset,
                                    row_bytes, CACHE_SYNC_WRITEBACK);
                }
            }
        }
        brain_pxp_transpose(g_framebuffer_phys, stride / 2, &dirty, g_staging_phys,
                            angle, brain_current_board.mirror_columns);
        composite_cursor(&window, &dirty);
        transfer_window(&window);
        return;
    }

    raster_surface primary;
    primary.bits = g_framebuffer;
    primary.width = width;
    primary.height = height;
    primary.stride = stride;
    primary.format = RASTER_FORMAT_16BPP;

    brain_compose(&brain_current_board, &primary, &dirty, angle, g_staging, &window);
    composite_cursor(&window, &dirty);
    transfer_window(&window);
}

static DWORD WINAPI refresh_thread(LPVOID parameter) {
    (void)parameter;
    CeSetThreadPriority(GetCurrentThread(), BRAIN_REFRESH_PRIORITY);
    brain_log("refresh: thread start");
    uint32_t wakes = 0;
    uint32_t frames = 0;
    DWORD report_at = GetTickCount() + 1000;
    while (g_running) {
        DWORD result = WaitForSingleObject(g_wake, 1000);
        if (!g_running) {
            break;
        }
        if (!g_paused) {
            if (result == WAIT_OBJECT_0) {
                wakes++;
            }
            brain_log_flush();
            raster_rect dirty;
            if (take_dirty(&dirty)) {
                frames++;
                if (brain_te_wait()) {
                    g_te_timeout_streak = 0;
                } else if (++g_te_timeout_streak >= 5) {
                    brain_te_recover();
                    brain_panel_recover(3);
                    brain_refresh_mark_dirty(0, 0, g_width, g_height);
                    g_te_timeout_streak = 0;
                }
                push_region(dirty);
            }
        }
        DWORD now = GetTickCount();
        if ((int32_t)(now - report_at) >= 0) {
            if (wakes != 0 || frames != 0) {
                brain_log_value("refresh: wakes/s", wakes);
                brain_log_value("refresh: frames/s", frames);
            }
            wakes = 0;
            frames = 0;
            report_at = now + 1000;
        }
    }
    brain_log("refresh: thread stop");
    return 0;
}

void brain_refresh_suspend(void) {
    g_paused = 1;
    Sleep(150);
}

void brain_refresh_push_full(void) {
    raster_rect full = {0, 0, g_width, g_height};
    push_region(full);
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

int brain_refresh_start(void *framebuffer, uint32_t framebuffer_phys,
                        int32_t width, int32_t height, int32_t stride) {
    g_framebuffer = framebuffer;
    g_framebuffer_phys = framebuffer_phys;
    g_width = width;
    g_height = height;
    g_stride = stride;

    DWORD staging_bytes = (DWORD)brain_current_board.panel_width * brain_current_board.panel_height * 2;
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
    g_staging = (uint16_t *)uncached;
    g_staging_phys = phys;
    brain_log_value("refresh: staging phys", phys);

    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_cursor_lock);
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
