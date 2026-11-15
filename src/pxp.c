/*
 * pxp.c - i.MX28 PXP. Transposes a rectangle from a source buffer into a
 * staging buffer with rotation and flip, blending up to eight overlays, and
 * waits for completion by interrupt or short spin.
 */
#include <windows.h>
#include <grimace/extras.h>
#include <stdint.h>

#include "pxp.h"
#include "irq.h"
#include "log.h"

#define PXP_IRQ 39

#define PXP_PHYS_BASE 0x8002A000u
#define PXP_MAP_SIZE  0x2000u

#define PXP_CTRL          0x000
#define PXP_CTRL_SET      0x004
#define PXP_CTRL_CLR      0x008
#define PXP_STAT          0x010
#define PXP_STAT_CLR      0x018
#define PXP_OUTBUF        0x020
#define PXP_OUTSIZE       0x040
#define PXP_S0BUF         0x050
#define PXP_S0PARAM       0x080
#define PXP_S0BACKGROUND  0x090
#define PXP_S0CROP        0x0A0

#define CTRL_SFTRST       (1u << 31)
#define CTRL_CLKGATE      (1u << 30)
#define CTRL_CROP         (1u << 19)
#define CTRL_VFLIP        (1u << 11)
#define CTRL_ENABLE       (1u << 0)
#define CTRL_IRQ_ENABLE   (1u << 1)
#define CTRL_HFLIP        (1u << 10)
#define CTRL_S0_FORMAT_RGB565  (0x4u << 12)
#define CTRL_OUTBUF_FORMAT_RGB565 (0x4u << 4)
#define CTRL_ROTATE_0     (0x0u << 8)
#define CTRL_ROTATE_90    (0x1u << 8)
#define CTRL_ROTATE_180   (0x2u << 8)
#define CTRL_ROTATE_270   (0x3u << 8)

#define PXP_FLIP_OVERRIDE_OFF 0xFFFFFFFFu
static uint32_t g_flip_override = PXP_FLIP_OVERRIDE_OFF;

/* Test knob: force the flip bits for every angle (bit 0 = HFLIP, bit 1 = VFLIP),
   so the correct rotate/flip per angle can be found on the device. Off by
   default; values above 3 disable it. */
void brain_pxp_set_flip_override(uint32_t value) {
    g_flip_override = value;
}

/* PXP rotate/flip per (angle, mirror) to reproduce the CPU compose mapping.
   The rotate is shared by both boards; the mirror term corrects the A7300, which
   runs the panel without a MADCTL row/column exchange (mirror_columns 0). Angle 0
   is device-confirmed (SH2 ROTATE_90; A7300 ROTATE_90+VFLIP). The A7300's 90/180/
   270 flips were tuned on the SH2 and are being re-tuned on the A7300 (180 showed
   a horizontal mirror); use the flip override to find them. */
static uint32_t pxp_angle_ctrl(int angle, int mirror) {
    uint32_t rotate = angle == 1 ? CTRL_ROTATE_0
                    : angle == 2 ? CTRL_ROTATE_270
                    : angle == 3 ? CTRL_ROTATE_180
                                 : CTRL_ROTATE_90;
    if (g_flip_override <= 3u) {
        return rotate | ((g_flip_override & 1u) ? CTRL_HFLIP : 0u)
                      | ((g_flip_override & 2u) ? CTRL_VFLIP : 0u);
    }
    switch (angle) {
    case 1:
        return rotate | (mirror ? 0u : CTRL_HFLIP);
    case 2:
        return rotate | (mirror ? 0u : (CTRL_VFLIP | CTRL_HFLIP));
    case 3:
        return rotate;
    default:
        return rotate | (mirror ? 0u : CTRL_VFLIP);
    }
}

#define STAT_IRQ          (1u << 0)
#define STAT_AXI_WRITE_ERROR (1u << 1)
#define STAT_AXI_READ_ERROR  (1u << 2)

#define PXP_SPIN_PX 16384

#define PXP_OL0              0x200
#define PXP_OL_STRIDE        0x40
#define PXP_OL_SIZE          0x10
#define PXP_OL_PARAM         0x20
#define PXP_OL_PARAM2        0x30
#define PXP_OLCOLORKEYLOW    0x1A0
#define PXP_OLCOLORKEYHIGH   0x1B0
#define PXP_S0COLORKEYLOW    0x180
#define PXP_S0COLORKEYHIGH   0x190
#define PXP_S0COLORKEY_OFF   0x00FFFFFF

static volatile uint32_t *g_pxp;
static int g_available;
static brain_irq g_irq;
static uint32_t g_path_irq;
static uint32_t g_path_spin;

void brain_pxp_path_counts(uint32_t *irq, uint32_t *spin) {
    *irq = g_path_irq;
    *spin = g_path_spin;
    g_path_irq = 0;
    g_path_spin = 0;
}

static void reg_write(uint32_t offset, uint32_t value) {
    g_pxp[offset / 4] = value;
}

static uint32_t reg_read(uint32_t offset) {
    return g_pxp[offset / 4];
}

int brain_pxp_init(void) {
    void *base = VirtualAlloc(NULL, PXP_MAP_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (base == NULL) {
        brain_log("pxp: VirtualAlloc failed");
        return 0;
    }
    if (!VirtualCopy(base, (LPVOID)(PXP_PHYS_BASE >> 8), PXP_MAP_SIZE,
                     PAGE_READWRITE | PAGE_PHYSICAL | PAGE_NOCACHE)) {
        brain_log("pxp: VirtualCopy failed");
        return 0;
    }
    g_pxp = (volatile uint32_t *)base;

    reg_write(PXP_CTRL_SET, CTRL_SFTRST);
    reg_write(PXP_CTRL_CLR, CTRL_SFTRST | CTRL_CLKGATE);

    brain_irq_init(&g_irq, PXP_IRQ);
    brain_log_value("pxp: irq ready", (unsigned long)g_irq.ready);

    g_available = 1;
    brain_log("pxp: ready");
    return 1;
}

int brain_pxp_available(void) {
    return g_available;
}

void brain_pxp_resume(void) {
    if (!g_available) {
        return;
    }
    reg_write(PXP_CTRL_SET, CTRL_SFTRST);
    reg_write(PXP_CTRL_CLR, CTRL_SFTRST | CTRL_CLKGATE);
}

static void pxp_program_overlays(const brain_pxp_overlay_set *overlays) {
    uint32_t count = overlays ? overlays->count : 0;
    for (uint32_t i = 0; i < BRAIN_PXP_MAX_OVERLAYS; i++) {
        uint32_t base = PXP_OL0 + i * PXP_OL_STRIDE;
        if (i < count) {
            reg_write(base, overlays->overlay[i].physical);
            reg_write(base + PXP_OL_SIZE, overlays->overlay[i].size);
            reg_write(base + PXP_OL_PARAM, overlays->overlay[i].parameters);
            reg_write(base + PXP_OL_PARAM2, 0);
        } else {
            reg_write(base + PXP_OL_PARAM, 0);
        }
    }
    if (count > 0) {
        reg_write(PXP_OLCOLORKEYLOW, overlays->colour_key_low);
        reg_write(PXP_OLCOLORKEYHIGH, overlays->colour_key_high);
        reg_write(PXP_S0COLORKEYLOW, PXP_S0COLORKEY_OFF);
        reg_write(PXP_S0COLORKEYHIGH, 0);
    }
}

int brain_pxp_transpose(uint32_t src_phys, int32_t src_stride_px,
                        const raster_rect *rect, uint32_t dst_phys, int angle, int mirror) {
    return brain_pxp_transpose_overlays(src_phys, src_stride_px, rect, dst_phys, angle, mirror, NULL);
}

int brain_pxp_transpose_overlays(uint32_t src_phys, int32_t src_stride_px,
                                 const raster_rect *rect, uint32_t dst_phys, int angle, int mirror,
                                 const brain_pxp_overlay_set *overlays) {
    if (!g_available) {
        return 0;
    }
    int32_t left = rect->left;
    int32_t top = rect->top;
    int32_t rect_w = rect->right - rect->left;
    int32_t rect_h = rect->bottom - rect->top;

    uint32_t corner = src_phys + (uint32_t)((top * src_stride_px + left) * 2);

    reg_write(PXP_S0BUF, corner);
    reg_write(PXP_S0PARAM,
              ((uint32_t)(src_stride_px / 8) << 8) | (uint32_t)(rect_h / 8));
    reg_write(PXP_S0CROP,
              ((uint32_t)(rect_w / 8) << 8) | (uint32_t)(rect_h / 8));
    reg_write(PXP_S0BACKGROUND, 0);
    reg_write(PXP_OUTBUF, dst_phys);
    reg_write(PXP_OUTSIZE,
              (0xFFu << 24) | ((uint32_t)rect_w << 12) | (uint32_t)rect_h);

    pxp_program_overlays(overlays);

    uint32_t ctrl = CTRL_ENABLE | CTRL_CROP | CTRL_S0_FORMAT_RGB565 |
                    CTRL_OUTBUF_FORMAT_RGB565 | pxp_angle_ctrl(angle, mirror);

    int ok = 0;
    int spin = ((int32_t)((int64_t)rect_w * rect_h) <= PXP_SPIN_PX);
    if (g_irq.ready && !spin) {
        g_path_irq++;
        reg_write(PXP_STAT_CLR, STAT_IRQ);
        reg_write(PXP_CTRL, ctrl | CTRL_IRQ_ENABLE);
        uint32_t start = GetTickCount();
        for (;;) {
            if (WaitForSingleObject(g_irq.event, 2) == WAIT_OBJECT_0) {
                ok = 1;
                break;
            }
            uint32_t stat = reg_read(PXP_STAT);
            if ((stat & STAT_IRQ) || !(reg_read(PXP_CTRL) & CTRL_ENABLE)) {
                ok = 1;
                break;
            }
            if (stat & (STAT_AXI_READ_ERROR | STAT_AXI_WRITE_ERROR)) {
                break;
            }
            if (GetTickCount() - start > 50) {
                break;
            }
        }
        reg_write(PXP_STAT_CLR, STAT_IRQ);
        brain_irq_done(&g_irq);
    } else {
        g_path_spin++;
        reg_write(PXP_CTRL, ctrl);
        uint32_t start = GetTickCount();
        for (;;) {
            uint32_t stat = reg_read(PXP_STAT);
            if (stat & STAT_IRQ) {
                ok = 1;
                break;
            }
            if (stat & (STAT_AXI_READ_ERROR | STAT_AXI_WRITE_ERROR)) {
                break;
            }
            if (!(reg_read(PXP_CTRL) & CTRL_ENABLE)) {
                ok = 1;
                break;
            }
            if (GetTickCount() - start > 50) {
                break;
            }
        }
        reg_write(PXP_STAT_CLR, STAT_IRQ);
    }

    if (!ok) {
        brain_log_value("pxp TIMEOUT STAT", reg_read(PXP_STAT));
        brain_log_value("pxp TIMEOUT CTRL", reg_read(PXP_CTRL));
    }
    return ok;
}
