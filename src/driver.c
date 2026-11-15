/*
 * driver.c - Windows CE DDI entry points (DrvEnableDriver and the Drv* drawing
 * callbacks GWES calls). Translates DDI objects into rasteriser calls, marks
 * dirty regions for the refresh thread, and dispatches escapes including
 * present mode.
 */
#include <windows.h>
#include <winddi.h>
#include <string.h>

#include "board.h"
#include "raster.h"
#include "log.h"
#include "panel.h"
#include "refresh.h"
#include "te.h"
#include "pxp.h"
#include "present.h"

LPVOID AllocPhysMem(DWORD size, DWORD protect, DWORD alignment, DWORD flags, PULONG physical);
BOOL FreePhysMem(LPVOID address);
BOOL WINAPI VirtualCopy(LPVOID dest, LPVOID src, DWORD size, DWORD protect);

static ENGCALLBACKS g_eng;

#define BRAIN_RED_MASK   0xF800
#define BRAIN_GREEN_MASK 0x07E0
#define BRAIN_BLUE_MASK  0x001F

typedef struct {
    ENGCALLBACKS eng;
    GDIINFO gdiinfo;
    DEVINFO devinfo;
    HPALETTE palette;

    HSURF primary_surface;
    DHSURF primary_handle;
    void *framebuffer;
    void *framebuffer_cached;
    ULONG framebuffer_physical;
    int32_t width;
    int32_t height;
    int32_t stride;
    void *present_capture;

    ULONG orientation;
} brain_pdev;

typedef struct {
    void *bits;
    int32_t width;
    int32_t height;
    int32_t stride;
} brain_devbitmap;

typedef struct {
    int32_t width;
    int32_t height;
    int32_t stride;
} brain_rbrush;

static int surface_to_raster(brain_pdev *pdev, SURFOBJ *pso, raster_surface *out) {
    if (pso == NULL) {
        return 0;
    }
    if (pso->dhsurf != 0) {
        if (pso->dhsurf == pdev->primary_handle) {
            out->bits = pdev->framebuffer;
            out->width = pdev->width;
            out->height = pdev->height;
            out->stride = pdev->stride;
            out->format = RASTER_FORMAT_16BPP;
            return 1;
        }
        brain_devbitmap *bitmap = (brain_devbitmap *)pso->dhsurf;
        out->bits = bitmap->bits;
        out->width = bitmap->width;
        out->height = bitmap->height;
        out->stride = bitmap->stride;
        out->format = RASTER_FORMAT_16BPP;
        return out->bits != NULL;
    }
    out->bits = pso->pvScan0;
    out->width = pso->sizlBitmap.cx;
    out->height = pso->sizlBitmap.cy;
    out->stride = pso->lDelta;
    out->format = pso->iBitmapFormat;
    return out->bits != NULL;
}

static int surface_is_primary(brain_pdev *pdev, SURFOBJ *pso) {
    return pso != NULL && pso->dhsurf != 0 && pso->dhsurf == pdev->primary_handle;
}

/* Trace a DDI entry, but only while presenting and with Log on, so the call
   sequence of a screenshot taken during present mode can be timed without any
   cost on the normal draw path. */
static void ddi_trace(const char *name) {
    if (brain_log_debug_active() && brain_refresh_presenting()) {
        brain_log_value(name, GetTickCount());
    }
}

/* A screen read while present mode holds the primary (a screenshot) should see
   the presented frame, not the stale primary. Redirect the source to a capture
   of the last presented frame. */
static void redirect_present_source(brain_pdev *pdev, SURFOBJ *psoSrc, raster_surface *source) {
    if (surface_is_primary(pdev, psoSrc) && brain_refresh_presenting() &&
        pdev->present_capture != NULL &&
        brain_refresh_capture_present((uint16_t *)pdev->present_capture, pdev->width, pdev->height,
                                      pdev->stride)) {
        source->bits = pdev->present_capture;
    }
}

static void masks_for_format(int32_t format, uint32_t *red, uint32_t *green, uint32_t *blue) {
    switch (format) {
    case RASTER_FORMAT_16BPP:
        *red = BRAIN_RED_MASK;
        *green = BRAIN_GREEN_MASK;
        *blue = BRAIN_BLUE_MASK;
        break;
    case RASTER_FORMAT_16BPP_555:
        *red = 0x7C00;
        *green = 0x03E0;
        *blue = 0x001F;
        break;
    case RASTER_FORMAT_24BPP:
    case RASTER_FORMAT_32BPP:
        *red = 0x00FF0000;
        *green = 0x0000FF00;
        *blue = 0x000000FF;
        break;
    default:
        *red = 0;
        *green = 0;
        *blue = 0;
        break;
    }
}

static uint32_t format_palette_count(int32_t format) {
    switch (format) {
    case RASTER_FORMAT_1BPP: return 2;
    case RASTER_FORMAT_2BPP: return 4;
    case RASTER_FORMAT_4BPP: return 16;
    case RASTER_FORMAT_8BPP: return 256;
    default: return 0;
    }
}

static uint32_t rgb_to_dst(ULONG colour, int32_t dst_format) {
    ULONG r = colour & 0xFF;
    ULONG g = (colour >> 8) & 0xFF;
    ULONG b = (colour >> 16) & 0xFF;
    if (dst_format == RASTER_FORMAT_24BPP || dst_format == RASTER_FORMAT_32BPP) {
        return b | (g << 8) | (r << 16);
    }
    if (dst_format == RASTER_FORMAT_16BPP_555) {
        return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    }
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static int is_rgb24or32(int32_t format) {
    return format == RASTER_FORMAT_24BPP || format == RASTER_FORMAT_32BPP;
}

static void rgb_order_masks(int pal_type, uint32_t *red, uint32_t *green, uint32_t *blue) {
    if (pal_type == PAL_RGB) {
        *red = 0x000000FF;
        *green = 0x0000FF00;
        *blue = 0x00FF0000;
    } else {
        *red = 0x00FF0000;
        *green = 0x0000FF00;
        *blue = 0x000000FF;
    }
}

static void source_masks(brain_pdev *pdev, XLATEOBJ *pxlo, int32_t src_format,
                         uint32_t *red, uint32_t *green, uint32_t *blue) {
    if (pxlo != NULL && pxlo->iSrcType == PAL_BITFIELDS) {
        ULONG masks[3] = {0, 0, 0};
        pdev->eng.XLATEOBJ_cGetPalette(pxlo, XO_SRCPALETTE, 3, masks);
        if (masks[0] != 0 || masks[1] != 0 || masks[2] != 0) {
            *red = masks[0];
            *green = masks[1];
            *blue = masks[2];
            return;
        }
    }
    if (pxlo != NULL && is_rgb24or32(src_format)) {
        rgb_order_masks(pxlo->iSrcType, red, green, blue);
        return;
    }
    masks_for_format(src_format, red, green, blue);
}

static void dest_masks(brain_pdev *pdev, XLATEOBJ *pxlo, int32_t dst_format,
                       uint32_t *red, uint32_t *green, uint32_t *blue) {
    if (pxlo != NULL && pxlo->iDstType == PAL_BITFIELDS) {
        ULONG masks[3] = {0, 0, 0};
        pdev->eng.XLATEOBJ_cGetPalette(pxlo, XO_DESTPALETTE, 3, masks);
        if (masks[0] != 0 || masks[1] != 0 || masks[2] != 0) {
            *red = masks[0];
            *green = masks[1];
            *blue = masks[2];
            return;
        }
    }
    if (pxlo != NULL && is_rgb24or32(dst_format)) {
        rgb_order_masks(pxlo->iDstType, red, green, blue);
        return;
    }
    masks_for_format(dst_format, red, green, blue);
}

static void xlate_to_raster(brain_pdev *pdev, XLATEOBJ *pxlo, int32_t src_format, int32_t dst_format,
                            uint32_t *table_buffer, raster_xlate *out) {
    out->table = NULL;
    out->table_count = 0;
    out->prepared = 0;
    if (pxlo == NULL || (pxlo->flXlate & XO_TRIVIAL)) {
        out->kind = RASTER_XLATE_TRIVIAL;
        return;
    }
    if ((pxlo->flXlate & XO_TABLE) && pxlo->pulXlate != NULL) {
        out->kind = RASTER_XLATE_TABLE;
        out->table = (const uint32_t *)pxlo->pulXlate;
        out->table_count = pxlo->cEntries;
        return;
    }
    uint32_t src_palette = format_palette_count(src_format);
    if (src_palette > 0) {
        ULONG palette[256];
        uint32_t copied = pdev->eng.XLATEOBJ_cGetPalette(pxlo, XO_SRCPALETTE, src_palette, palette);
        if (copied > 0) {
            if (copied > 256) {
                copied = 256;
            }
            for (uint32_t index = 0; index < copied; index++) {
                table_buffer[index] = rgb_to_dst(palette[index], dst_format);
            }
            out->kind = RASTER_XLATE_TABLE;
            out->table = table_buffer;
            out->table_count = copied;
            return;
        }
    }
    uint32_t dst_palette = format_palette_count(dst_format);
    if (dst_palette > 0 && pxlo->iDstType == PAL_INDEXED) {
        uint32_t copied = pdev->eng.XLATEOBJ_cGetPalette(pxlo, XO_DESTPALETTE, dst_palette, (ULONG *)table_buffer);
        if (copied > 0) {
            out->kind = RASTER_XLATE_RGB_TO_INDEX;
            out->table = table_buffer;
            out->table_count = copied > 256 ? 256 : copied;
            source_masks(pdev, pxlo, src_format, &out->src_red, &out->src_green, &out->src_blue);
            return;
        }
    }
    out->kind = RASTER_XLATE_RGB;
    source_masks(pdev, pxlo, src_format, &out->src_red, &out->src_green, &out->src_blue);
    dest_masks(pdev, pxlo, dst_format, &out->dst_red, &out->dst_green, &out->dst_blue);
    if (src_format == dst_format &&
        out->src_red == out->dst_red &&
        out->src_green == out->dst_green &&
        out->src_blue == out->dst_blue) {
        out->kind = RASTER_XLATE_TRIVIAL;
        return;
    }
    raster_xlate_prepare(out);
}

#define BRAIN_MAX_CLIP_RECTS 64

typedef struct {
    raster_clip clip;
    raster_rect rects[BRAIN_MAX_CLIP_RECTS];
    BYTE enum_buffer[sizeof(ULONG) + BRAIN_MAX_CLIP_RECTS * sizeof(RECTL)];
} brain_clip;

static void clip_to_raster(brain_pdev *pdev, CLIPOBJ *pco, brain_clip *out) {
    if (pco == NULL || pco->iDComplexity == DC_TRIVIAL) {
        out->clip.kind = RASTER_CLIP_NONE;
        return;
    }
    out->clip.bounds.left = pco->rclBounds.left;
    out->clip.bounds.top = pco->rclBounds.top;
    out->clip.bounds.right = pco->rclBounds.right;
    out->clip.bounds.bottom = pco->rclBounds.bottom;
    if (pco->iDComplexity == DC_RECT) {
        out->clip.kind = RASTER_CLIP_RECT;
        return;
    }
    out->clip.kind = RASTER_CLIP_COMPLEX;
    out->clip.non_overlapping = 1;
    out->clip.rects = out->rects;
    out->clip.rect_count = 0;
    pdev->eng.CLIPOBJ_cEnumStart(pco, FALSE, CT_RECTANGLES, CD_ANY, BRAIN_MAX_CLIP_RECTS);
    for (;;) {
        ENUMRECTS *batch = (ENUMRECTS *)out->enum_buffer;
        BOOL more = pdev->eng.CLIPOBJ_bEnum(pco, sizeof(out->enum_buffer), (ULONG *)batch);
        for (ULONG index = 0; index < batch->c && out->clip.rect_count < BRAIN_MAX_CLIP_RECTS; index++) {
            raster_rect *r = &out->rects[out->clip.rect_count++];
            r->left = batch->arcl[index].left;
            r->top = batch->arcl[index].top;
            r->right = batch->arcl[index].right;
            r->bottom = batch->arcl[index].bottom;
        }
        if (!more) {
            break;
        }
    }
}

static void brush_to_raster(brain_pdev *pdev, BRUSHOBJ *pbo, POINTL *brush_org, raster_brush *out) {
    out->is_pattern = 0;
    out->solid = 0;
    out->org_x = brush_org ? brush_org->x : 0;
    out->org_y = brush_org ? brush_org->y : 0;
    if (pbo == NULL) {
        return;
    }
    if (pbo->iSolidColor != 0xFFFFFFFF) {
        out->solid = pbo->iSolidColor;
        return;
    }
    brain_rbrush *realized = (brain_rbrush *)pdev->eng.BRUSHOBJ_pvGetRbrush(pbo);
    if (realized != NULL) {
        out->is_pattern = 1;
        out->pattern.bits = (void *)(realized + 1);
        out->pattern.width = realized->width;
        out->pattern.height = realized->height;
        out->pattern.stride = realized->stride;
        out->pattern.format = RASTER_FORMAT_16BPP;
    }
}

static BOOL APIENTRY brain_BitBlt(SURFOBJ *psoTrg, SURFOBJ *psoSrc, SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo,
                                  RECTL *prclTrg, POINTL *pptlSrc, POINTL *pptlMask, BRUSHOBJ *pbo, POINTL *pptlBrush,
                                  ROP4 rop4) {
    brain_pdev *pdev = (brain_pdev *)psoTrg->dhpdev;
    raster_surface target, source, mask;
    raster_surface *source_ptr = NULL;
    raster_surface *mask_ptr = NULL;
    brain_clip clip;
    raster_xlate xlate;
    raster_brush brush;
    raster_rect rect;
    uint32_t xlate_table[256];

    ddi_trace("ddi BitBlt in");
    if (!surface_to_raster(pdev, psoTrg, &target)) {
        return FALSE;
    }
    if (surface_to_raster(pdev, psoSrc, &source)) {
        source_ptr = &source;
        redirect_present_source(pdev, psoSrc, &source);
    }
    if (surface_to_raster(pdev, psoMask, &mask)) {
        mask_ptr = &mask;
    }
    clip_to_raster(pdev, pco, &clip);
    xlate_to_raster(pdev, pxlo, source_ptr ? source.format : 0, target.format, xlate_table, &xlate);
    brush_to_raster(pdev, pbo, pptlBrush, &brush);

    rect.left = prclTrg->left;
    rect.top = prclTrg->top;
    rect.right = prclTrg->right;
    rect.bottom = prclTrg->bottom;

    uint32_t dst_masks[3];
    dest_masks(pdev, pxlo, target.format, &dst_masks[0], &dst_masks[1], &dst_masks[2]);

    brain_snapshot_lock();
    raster_bitblt(&target, source_ptr, mask_ptr, &clip.clip, &xlate, &rect,
                  pptlSrc ? pptlSrc->x : 0, pptlSrc ? pptlSrc->y : 0,
                  pptlMask ? pptlMask->x : 0, pptlMask ? pptlMask->y : 0,
                  &brush, (uint16_t)rop4, dst_masks);
    brain_snapshot_unlock();

    if (surface_is_primary(pdev, psoTrg)) {
        raster_rect dirty = rect;
        raster_normalise_rect(&dirty);
        brain_refresh_mark_dirty(dirty.left, dirty.top, dirty.right, dirty.bottom);
    }
    ddi_trace("ddi BitBlt out");
    return TRUE;
}

static BOOL APIENTRY brain_CopyBits(SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo,
                                    RECTL *prclDest, POINTL *pptlSrc) {
    ddi_trace("ddi CopyBits in");
    BOOL ok = brain_BitBlt(psoDest, psoSrc, NULL, pco, pxlo, prclDest, pptlSrc, NULL, NULL, NULL, 0xCCCC);
    ddi_trace("ddi CopyBits out");
    return ok;
}

static ULONG pack_rgb565(ULONG colour) {
    ULONG r = (colour & 0xFF);
    ULONG g = (colour >> 8) & 0xFF;
    ULONG b = (colour >> 16) & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static ULONG place_channel(ULONG value8, ULONG mask) {
    if (mask == 0) {
        return 0;
    }
    ULONG shift = 0;
    while (((mask >> shift) & 1) == 0) {
        shift++;
    }
    ULONG width = 0;
    while ((mask >> (shift + width)) & 1) {
        width++;
    }
    return ((value8 >> (8 - width)) << shift) & mask;
}

static ULONG pack_masks(ULONG colorref, ULONG rmask, ULONG gmask, ULONG bmask) {
    return place_channel(colorref & 0xFF, rmask) |
           place_channel((colorref >> 8) & 0xFF, gmask) |
           place_channel((colorref >> 16) & 0xFF, bmask);
}

static ULONG APIENTRY brain_RealizeColor(USHORT iDstType, ULONG cEntries, ULONG *pPalette, ULONG rgbColor) {
    switch (iDstType) {
    case PAL_BITFIELDS:
        if (pPalette != NULL && cEntries >= 3) {
            return pack_masks(rgbColor, pPalette[0], pPalette[1], pPalette[2]);
        }
        return pack_rgb565(rgbColor);
    case PAL_RGB:
        return rgbColor & 0x00FFFFFF;
    case PAL_BGR:
        return ((rgbColor & 0xFF) << 16) | (rgbColor & 0x0000FF00) | ((rgbColor >> 16) & 0xFF);
    case PAL_INDEXED:
        if (pPalette != NULL) {
            ULONG best = 0;
            LONG best_distance = 0x7FFFFFFF;
            ULONG r = rgbColor & 0xFF;
            ULONG g = (rgbColor >> 8) & 0xFF;
            ULONG b = (rgbColor >> 16) & 0xFF;
            for (ULONG index = 0; index < cEntries; index++) {
                ULONG entry = pPalette[index];
                LONG dr = (LONG)r - (LONG)(entry & 0xFF);
                LONG dg = (LONG)g - (LONG)((entry >> 8) & 0xFF);
                LONG db = (LONG)b - (LONG)((entry >> 16) & 0xFF);
                LONG distance = dr * dr + dg * dg + db * db;
                if (distance < best_distance) {
                    best_distance = distance;
                    best = index;
                }
            }
            return best;
        }
        return 0;
    default:
        return pack_rgb565(rgbColor);
    }
}

static ULONG APIENTRY brain_UnrealizeColor(USHORT iSrcType, ULONG cEntries, ULONG *pPalette, ULONG iRealizedColor) {
    if (iSrcType == PAL_BITFIELDS) {
        ULONG r = (iRealizedColor >> 11) & 0x1F;
        ULONG g = (iRealizedColor >> 5) & 0x3F;
        ULONG b = iRealizedColor & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        return r | (g << 8) | (b << 16);
    }
    if (iSrcType == PAL_INDEXED && pPalette != NULL && iRealizedColor < cEntries) {
        return pPalette[iRealizedColor];
    }
    return iRealizedColor;
}

static void fill_gdiinfo(GDIINFO *info) {
    memset(info, 0, sizeof(*info));
    info->ulVersion = 0x00040001;
    info->ulTechnology = DT_RASDISPLAY;
    info->ulHorzSize = BRAIN_SCREEN_MM_X;
    info->ulVertSize = BRAIN_SCREEN_MM_Y;
    info->ulHorzRes = BRAIN_SCREEN_WIDTH;
    info->ulVertRes = BRAIN_SCREEN_HEIGHT;
    info->cBitsPixel = 16;
    info->cPlanes = 1;
    info->ulNumBrushes = 65536;
    info->flRaster = 0x00012801;
    info->ulLogPixelsX = 96;
    info->ulLogPixelsY = 96;
    info->flTextCaps = 0x01000500;
    info->ulReserved1 = 0x00000017;
    info->ulAspectX = 1;
    info->ulAspectY = 1;
    info->ulAspectXY = 1;
}

static void fill_devinfo(DEVINFO *info, HPALETTE palette) {
    memset(info, 0, sizeof(*info));
    info->flGraphicsCaps = 0x01000500;
    info->iDitherFormat = BMF_16BPP;
    info->hpalDefault = palette;
}

static int dmdo_to_angle(ULONG dmdo) {
    switch (dmdo) {
    case 1: return 1;
    case 2: return 2;
    case 4: return 3;
    default: return 0;
    }
}

/* Publish the primary framebuffer's location to HKLM\Software\Brain\Display so a
   board-driver screenshot path can find it without going through GDI: physical
   address, RGB565, width and height in pixels (stride is width*2). Written at
   EnableSurface and refreshed when rotation swaps the dimensions. */
static void publish_framebuffer_info(brain_pdev *pdev) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Brain\\Display", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return;
    }
    DWORD physical = brain_refresh_presenting() ? 0 : pdev->framebuffer_physical;
    DWORD width = (DWORD)pdev->width;
    DWORD height = (DWORD)pdev->height;
    RegSetValueExW(key, L"FramebufferPhysical", 0, REG_DWORD, (const BYTE *)&physical, sizeof(physical));
    RegSetValueExW(key, L"FramebufferWidth", 0, REG_DWORD, (const BYTE *)&width, sizeof(width));
    RegSetValueExW(key, L"FramebufferHeight", 0, REG_DWORD, (const BYTE *)&height, sizeof(height));
    RegCloseKey(key);
}

static DHPDEV APIENTRY brain_EnablePDEV(DEVMODEW *pdm, LPWSTR log_address, ULONG pat_count, HSURF *patterns,
                                        ULONG caps_size, ULONG *devcaps, ULONG devinfo_size, DEVINFO *devinfo,
                                        HDEV hdev, LPWSTR device_name, HANDLE driver) {
    static brain_pdev pdev;
    static int pdev_initialized;
    int reentry = pdev_initialized;

    if (!pdev_initialized) {
        memset(&pdev, 0, sizeof(pdev));
        pdev.eng = g_eng;
        pdev.palette = pdev.eng.EngCreatePalette(PAL_BITFIELDS, 0, NULL, BRAIN_RED_MASK, BRAIN_GREEN_MASK, BRAIN_BLUE_MASK);
        pdev_initialized = 1;
        brain_log("EnablePDEV: first init");
    } else {
        brain_log("EnablePDEV: re-entrant");
    }

    brain_log_value("EnablePDEV: cjDevInfo", devinfo_size);
    brain_log_value("EnablePDEV: cjCaps", caps_size);
    brain_log_value("EnablePDEV: pdm", (unsigned long)pdm);
    fill_gdiinfo(&pdev.gdiinfo);
    if (pdm != NULL) {
        pdev.width = pdm->dmPelsWidth ? (int32_t)pdm->dmPelsWidth : BRAIN_SCREEN_WIDTH;
        pdev.height = pdm->dmPelsHeight ? (int32_t)pdm->dmPelsHeight : BRAIN_SCREEN_HEIGHT;
    } else {
        pdev.width = BRAIN_SCREEN_WIDTH;
        pdev.height = BRAIN_SCREEN_HEIGHT;
    }
    pdev.stride = pdev.width * 2;
    pdev.gdiinfo.ulHorzRes = pdev.width;
    pdev.gdiinfo.ulVertRes = pdev.height;

    if (caps_size != 0 && devcaps != NULL) {
        memcpy(devcaps, &pdev.gdiinfo, caps_size < sizeof(pdev.gdiinfo) ? caps_size : sizeof(pdev.gdiinfo));
    }
    brain_log_value("EnablePDEV: palette", (unsigned long)pdev.palette);
    if (devinfo_size != 0 && devinfo != NULL) {
        fill_devinfo(&pdev.devinfo, pdev.palette);
        memcpy(devinfo, &pdev.devinfo, devinfo_size < sizeof(pdev.devinfo) ? devinfo_size : sizeof(pdev.devinfo));
    }
    if (reentry) {
        brain_refresh_set_rotation(dmdo_to_angle(pdev.orientation), pdev.width, pdev.height, pdev.stride);
        publish_framebuffer_info(&pdev);
    }
    brain_log_value("EnablePDEV: return", (unsigned long)&pdev);
    return (DHPDEV)&pdev;
}

static HSURF APIENTRY brain_EnableSurface(DHPDEV dhpdev) {
    brain_pdev *pdev = (brain_pdev *)dhpdev;
    SIZEL size;
    size.cx = pdev->width;
    size.cy = pdev->height;
    pdev->stride = pdev->width * 2;

    brain_log("EnableSurface: enter");
    DWORD framebuffer_bytes = (DWORD)pdev->stride * pdev->height;
    pdev->framebuffer_physical = 0;
    pdev->framebuffer_cached = NULL;
    ULONG phys = 0;
    void *base = AllocPhysMem(framebuffer_bytes, PAGE_READWRITE, 0, 0, &phys);
    if (base != NULL && phys != 0) {
        void *mapped = VirtualAlloc(NULL, framebuffer_bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (mapped != NULL && VirtualCopy(mapped, (LPVOID)(phys >> 8), framebuffer_bytes,
                                          PAGE_READWRITE | PAGE_PHYSICAL)) {
            pdev->framebuffer = mapped;
            pdev->framebuffer_cached = base;
            pdev->framebuffer_physical = phys;
        } else {
            if (mapped != NULL) {
                VirtualFree(mapped, 0, MEM_RELEASE);
            }
            FreePhysMem(base);
        }
    } else if (base != NULL) {
        FreePhysMem(base);
    }
    if (pdev->framebuffer_physical == 0) {
        pdev->framebuffer = VirtualAlloc(NULL, framebuffer_bytes,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        brain_log("EnableSurface: framebuffer not contiguous, pxp disabled");
    }
    if (pdev->framebuffer == NULL) {
        brain_log("EnableSurface: framebuffer alloc failed");
        return 0;
    }
    memset(pdev->framebuffer, 0, (size_t)framebuffer_bytes);
    brain_log_value("EnableSurface: framebuffer", (unsigned long)pdev->framebuffer);
    brain_log_value("EnableSurface: framebuffer phys", pdev->framebuffer_physical);
    {
        volatile const uint32_t *probe = (const uint32_t *)pdev->framebuffer;
        DWORD words = framebuffer_bytes / 4;
        uint32_t accumulator = 0;
        DWORD cold_start = GetTickCount();
        for (DWORD i = 0; i < words; i++) accumulator += probe[i];
        DWORD cold_ms = GetTickCount() - cold_start;
        DWORD warm_start = GetTickCount();
        for (DWORD i = 0; i < words; i++) accumulator += probe[i];
        DWORD warm_ms = GetTickCount() - warm_start;
        brain_log_value("EnableSurface: fb read cold ms", cold_ms);
        brain_log_value("EnableSurface: fb read warm ms", warm_ms);
        brain_log_value("EnableSurface: fb probe", accumulator);
    }

    pdev->primary_handle = (DHSURF)pdev;
    pdev->primary_surface = pdev->eng.EngCreateDeviceSurface(pdev->primary_handle, size, BMF_16BPP);
    brain_log_value("EnableSurface: primary handle", (unsigned long)pdev->primary_surface);

    pdev->present_capture = LocalAlloc(LMEM_FIXED, framebuffer_bytes);

    if (!brain_panel_init()) {
        brain_log("EnableSurface: panel init failed");
    }
    if (!brain_refresh_start(pdev->framebuffer, pdev->framebuffer_physical,
                             pdev->width, pdev->height, pdev->stride)) {
        brain_log("EnableSurface: refresh start failed");
    }
    publish_framebuffer_info(pdev);
    brain_log("EnableSurface: done");
    return pdev->primary_surface;
}

static VOID APIENTRY brain_DisableSurface(DHPDEV dhpdev) {
    brain_pdev *pdev = (brain_pdev *)dhpdev;
    brain_log("DisableSurface: enter");
    brain_refresh_stop();
    if (pdev->primary_surface != 0) {
        pdev->eng.EngDeleteSurface(pdev->primary_surface);
        pdev->primary_surface = 0;
    }
    if (pdev->present_capture != NULL) {
        LocalFree(pdev->present_capture);
        pdev->present_capture = NULL;
    }
    if (pdev->framebuffer != NULL) {
        VirtualFree(pdev->framebuffer, 0, MEM_RELEASE);
        pdev->framebuffer = NULL;
    }
    if (pdev->framebuffer_cached != NULL) {
        FreePhysMem(pdev->framebuffer_cached);
        pdev->framebuffer_cached = NULL;
    }
}

static VOID APIENTRY brain_DisablePDEV(DHPDEV dhpdev) {
    (void)dhpdev;
    brain_log("DisablePDEV: enter");
}

static ULONG APIENTRY brain_GetModes(HANDLE driver, ULONG size, DEVMODEW *pdm) {
    ULONG needed = sizeof(DEVMODEW);
    brain_log_value("GetModes: size", size);
    if (pdm == NULL || size < needed) {
        return needed;
    }
    brain_log("GetModes: fill");
    memset(pdm, 0, needed);
    pdm->dmDeviceName[0] = 'G';
    pdm->dmDeviceName[1] = 'P';
    pdm->dmDeviceName[2] = 'E';
    pdm->dmSize = (WORD)sizeof(DEVMODEW);
    pdm->dmBitsPerPel = 16;
    pdm->dmPelsWidth = BRAIN_SCREEN_WIDTH;
    pdm->dmPelsHeight = BRAIN_SCREEN_HEIGHT;
    pdm->dmDisplayFrequency = 60;
    pdm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    return needed;
}

static BOOL APIENTRY brain_RealizeBrush(BRUSHOBJ *pbo, SURFOBJ *target, SURFOBJ *pattern, SURFOBJ *mask,
                                        XLATEOBJ *pxlo, ULONG hatch) {
    (void)mask;
    (void)hatch;
    brain_pdev *pdev = (brain_pdev *)target->dhpdev;
    raster_surface pattern_src;
    if (!surface_to_raster(pdev, pattern, &pattern_src)) {
        return FALSE;
    }
    int32_t width = pattern_src.width;
    int32_t height = pattern_src.height;
    if (width <= 0 || height <= 0) {
        return FALSE;
    }
    ULONG bytes = sizeof(brain_rbrush) + (ULONG)width * height * 2;
    brain_rbrush *realized = (brain_rbrush *)pdev->eng.BRUSHOBJ_pvAllocRbrush(pbo, bytes);
    if (realized == NULL) {
        return FALSE;
    }
    realized->width = width;
    realized->height = height;
    realized->stride = width * 2;

    uint32_t xlate_table[256];
    raster_xlate xlate;
    xlate_to_raster(pdev, pxlo, pattern_src.format, RASTER_FORMAT_16BPP, xlate_table, &xlate);

    uint16_t *pixels = (uint16_t *)(realized + 1);
    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            pixels[y * width + x] = (uint16_t)raster_translate(&xlate, raster_get_pixel(&pattern_src, x, y));
        }
    }
    return TRUE;
}

static ULONG APIENTRY brain_SetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo,
                                            LONG xHot, LONG yHot, LONG x, LONG y, RECTL *prcl, FLONG fl) {
    if (psoMask == NULL || psoMask->pvScan0 == NULL) {
        brain_cursor_hide();
        return SPS_ACCEPT_NOEXCLUDE;
    }
    brain_cursor_set_shape(psoMask->pvScan0, psoMask->lDelta, psoMask->sizlBitmap.cx,
                           psoMask->sizlBitmap.cy / 2, xHot, yHot, x, y);
    return SPS_ACCEPT_NOEXCLUDE;
}

static VOID APIENTRY brain_MovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl) {
    brain_cursor_move(x, y);
}

static BOOL APIENTRY brain_StrokePath(SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco, XFORMOBJ *pxo, BRUSHOBJ *pbo,
                                      POINTL *pptlBrushOrg, LINEATTRS *plineattrs, MIX mix) {
    brain_pdev *pdev = (brain_pdev *)pso->dhpdev;
    raster_surface target;
    brain_clip clip;

    if (!surface_to_raster(pdev, pso, &target)) {
        return FALSE;
    }
    clip_to_raster(pdev, pco, &clip);

    uint32_t pen = (pbo != NULL) ? pbo->iSolidColor : 0;
    uint8_t rop2 = (uint8_t)(mix & 0xFF);
    if (rop2 < 1 || rop2 > 16) {
        rop2 = 13;
    }

    PATHDATA data;
    int have_prev = 0;
    LONG prev_x = 0, prev_y = 0, start_x = 0, start_y = 0;
    LONG min_x = target.width, min_y = target.height, max_x = 0, max_y = 0;
    int drew = 0;

    pdev->eng.PATHOBJ_vEnumStart(ppo);
    brain_snapshot_lock();
    for (;;) {
        BOOL more = pdev->eng.PATHOBJ_bEnum(ppo, &data);
        for (ULONG index = 0; index < data.count; index++) {
            LONG px = data.pptfx[index].x >> 4;
            LONG py = data.pptfx[index].y >> 4;
            if (index == 0 && (data.flags & PD_BEGINSUBPATH)) {
                start_x = px;
                start_y = py;
                prev_x = px;
                prev_y = py;
                have_prev = 1;
                continue;
            }
            if (have_prev) {
                raster_line(&target, &clip.clip, prev_x, prev_y, px, py, pen, rop2);
                drew = 1;
                if (prev_x < min_x) min_x = prev_x;
                if (prev_y < min_y) min_y = prev_y;
                if (prev_x > max_x) max_x = prev_x;
                if (prev_y > max_y) max_y = prev_y;
                if (px < min_x) min_x = px;
                if (py < min_y) min_y = py;
                if (px > max_x) max_x = px;
                if (py > max_y) max_y = py;
            }
            prev_x = px;
            prev_y = py;
            have_prev = 1;
        }
        if ((data.flags & PD_CLOSEFIGURE) && have_prev) {
            raster_line(&target, &clip.clip, prev_x, prev_y, start_x, start_y, pen, rop2);
            drew = 1;
            have_prev = 0;
        }
        if (!more) {
            break;
        }
    }
    brain_snapshot_unlock();

    if (drew && surface_is_primary(pdev, pso)) {
        brain_refresh_mark_dirty(min_x, min_y, max_x + 1, max_y + 1);
    }
    return TRUE;
}

#ifndef FP_WINDINGMODE
#define FP_WINDINGMODE 2
#endif

#define BRAIN_MAX_PATH_POINTS 300
#define BRAIN_MAX_SUBPATHS 32

static BOOL APIENTRY brain_FillPath(SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco, BRUSHOBJ *pbo,
                                    POINTL *pptlBrushOrg, MIX mix, FLONG flOptions) {
    brain_pdev *pdev = (brain_pdev *)pso->dhpdev;
    raster_surface target;
    brain_clip clip;
    raster_brush brush;

    if (!surface_to_raster(pdev, pso, &target)) {
        return FALSE;
    }
    clip_to_raster(pdev, pco, &clip);
    brush_to_raster(pdev, pbo, pptlBrushOrg, &brush);

    uint8_t rop2 = (uint8_t)(mix & 0xFF);
    if (rop2 < 1 || rop2 > 16) {
        rop2 = 13;
    }
    int winding = (flOptions & FP_WINDINGMODE) ? 1 : 0;

    raster_point points[BRAIN_MAX_PATH_POINTS];
    int32_t subpath_lengths[BRAIN_MAX_SUBPATHS];
    int32_t point_count = 0;
    int32_t subpath_count = 0;
    int32_t current_start = 0;
    LONG min_x = target.width, min_y = target.height, max_x = 0, max_y = 0;

    PATHDATA data;
    pdev->eng.PATHOBJ_vEnumStart(ppo);
    for (;;) {
        BOOL more = pdev->eng.PATHOBJ_bEnum(ppo, &data);
        if (data.flags & PD_BEGINSUBPATH) {
            current_start = point_count;
        }
        for (ULONG index = 0; index < data.count; index++) {
            if (point_count >= BRAIN_MAX_PATH_POINTS) {
                break;
            }
            LONG px = data.pptfx[index].x >> 4;
            LONG py = data.pptfx[index].y >> 4;
            points[point_count].x = px;
            points[point_count].y = py;
            point_count++;
            if (px < min_x) min_x = px;
            if (py < min_y) min_y = py;
            if (px > max_x) max_x = px;
            if (py > max_y) max_y = py;
        }
        if (data.flags & PD_ENDSUBPATH) {
            if (subpath_count < BRAIN_MAX_SUBPATHS) {
                subpath_lengths[subpath_count++] = point_count - current_start;
            }
            current_start = point_count;
        }
        if (!more) {
            break;
        }
    }

    if (subpath_count > 0) {
        brain_snapshot_lock();
        raster_fill_polygon(&target, &clip.clip, points, subpath_lengths, subpath_count, &brush, rop2, winding);
        brain_snapshot_unlock();
        if (surface_is_primary(pdev, pso)) {
            brain_refresh_mark_dirty(min_x, min_y, max_x + 1, max_y + 1);
        }
    }
    return TRUE;
}

static BOOL APIENTRY brain_Paint(SURFOBJ *pso, CLIPOBJ *pco, BRUSHOBJ *pbo, POINTL *pptlBrushOrg, MIX mix) {
    return FALSE;
}

static BOOL APIENTRY brain_AnyBlt(SURFOBJ *psoDest, SURFOBJ *psoSrc, SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo,
                                  POINTL *pptlHTOrg, RECTL *prclDest, RECTL *prclSrc, POINTL *pptlMask, BRUSHOBJ *pbo,
                                  POINTL *pptlBrush, ROP4 rop4, ULONG iMode, ULONG bltFlags) {
    brain_pdev *pdev = (brain_pdev *)psoDest->dhpdev;
    raster_surface target, source, mask;
    raster_surface *source_ptr = NULL;
    raster_surface *mask_ptr = NULL;
    brain_clip clip;
    raster_xlate xlate;
    raster_brush brush;
    raster_rect dst_rect, src_rect;
    uint32_t xlate_table[256];

    ddi_trace("ddi AnyBlt in");

    if (!surface_to_raster(pdev, psoDest, &target)) {
        return FALSE;
    }
    if (surface_to_raster(pdev, psoSrc, &source)) {
        source_ptr = &source;
        redirect_present_source(pdev, psoSrc, &source);
    }
    if (surface_to_raster(pdev, psoMask, &mask)) {
        mask_ptr = &mask;
    }
    clip_to_raster(pdev, pco, &clip);
    xlate_to_raster(pdev, pxlo, source_ptr ? source.format : 0, target.format, xlate_table, &xlate);
    brush_to_raster(pdev, pbo, pptlBrush, &brush);

    dst_rect.left = prclDest->left;
    dst_rect.top = prclDest->top;
    dst_rect.right = prclDest->right;
    dst_rect.bottom = prclDest->bottom;
    src_rect.left = prclSrc->left;
    src_rect.top = prclSrc->top;
    src_rect.right = prclSrc->right;
    src_rect.bottom = prclSrc->bottom;

    brain_snapshot_lock();
    raster_anyblt(&target, source_ptr, mask_ptr, &clip.clip, &xlate, &dst_rect, &src_rect,
                  pptlMask ? pptlMask->x : 0, pptlMask ? pptlMask->y : 0, &brush, (uint16_t)rop4);
    brain_snapshot_unlock();

    if (surface_is_primary(pdev, psoDest)) {
        raster_rect dirty = dst_rect;
        raster_normalise_rect(&dirty);
        brain_refresh_mark_dirty(dirty.left, dirty.top, dirty.right, dirty.bottom);
    }
    return TRUE;
}

static BOOL APIENTRY brain_TransparentBlt(SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo,
                                          RECTL *prclDest, RECTL *prclSrc, ULONG TransColor) {
    brain_pdev *pdev = (brain_pdev *)psoDest->dhpdev;
    raster_surface target, source;
    brain_clip clip;
    raster_xlate xlate;
    raster_rect dst_rect, src_rect;
    uint32_t xlate_table[256];

    if (!surface_to_raster(pdev, psoDest, &target)) {
        return FALSE;
    }
    if (!surface_to_raster(pdev, psoSrc, &source)) {
        return FALSE;
    }
    clip_to_raster(pdev, pco, &clip);
    xlate_to_raster(pdev, pxlo, source.format, target.format, xlate_table, &xlate);

    dst_rect.left = prclDest->left;
    dst_rect.top = prclDest->top;
    dst_rect.right = prclDest->right;
    dst_rect.bottom = prclDest->bottom;
    src_rect.left = prclSrc->left;
    src_rect.top = prclSrc->top;
    src_rect.right = prclSrc->right;
    src_rect.bottom = prclSrc->bottom;

    brain_snapshot_lock();
    raster_transparentblt(&target, &source, &clip.clip, &xlate, &dst_rect, &src_rect, TransColor);
    brain_snapshot_unlock();

    if (surface_is_primary(pdev, psoDest)) {
        raster_rect dirty = dst_rect;
        raster_normalise_rect(&dirty);
        brain_refresh_mark_dirty(dirty.left, dirty.top, dirty.right, dirty.bottom);
    }
    return TRUE;
}

static BOOL APIENTRY brain_GradientFill(SURFOBJ *pso, CLIPOBJ *pco, XLATEOBJ *pxlo, TRIVERTEX *pVertex, ULONG nVertex,
                                        PVOID pMesh, ULONG nMesh, RECTL *prclExtents, POINTL *pptlDitherOrg, ULONG ulMode) {
    brain_pdev *pdev = (brain_pdev *)pso->dhpdev;
    raster_surface target;
    brain_clip clip;

    if (ulMode != GRADIENT_FILL_RECT_H && ulMode != GRADIENT_FILL_RECT_V) {
        return FALSE;
    }
    if (!surface_to_raster(pdev, pso, &target)) {
        return FALSE;
    }
    clip_to_raster(pdev, pco, &clip);

    GRADIENT_RECT *rects = (GRADIENT_RECT *)pMesh;
    brain_snapshot_lock();
    for (ULONG index = 0; index < nMesh; index++) {
        TRIVERTEX *a = &pVertex[rects[index].UpperLeft];
        TRIVERTEX *b = &pVertex[rects[index].LowerRight];
        LONG left = a->x < b->x ? a->x : b->x;
        LONG right = a->x < b->x ? b->x : a->x;
        LONG top = a->y < b->y ? a->y : b->y;
        LONG bottom = a->y < b->y ? b->y : a->y;
        LONG denom = (ulMode == GRADIENT_FILL_RECT_H) ? (b->x - a->x) : (b->y - a->y);
        if (denom == 0) {
            continue;
        }
        for (LONG y = top; y < bottom; y++) {
            if (y < 0 || y >= target.height) {
                continue;
            }
            for (LONG x = left; x < right; x++) {
                if (x < 0 || x >= target.width) {
                    continue;
                }
                if (!raster_clip_contains(&clip.clip, x, y)) {
                    continue;
                }
                LONG numerator = (ulMode == GRADIENT_FILL_RECT_H) ? (x - a->x) : (y - a->y);
                LONG red = (a->Red + (LONG)(b->Red - a->Red) * numerator / denom) >> 8;
                LONG green = (a->Green + (LONG)(b->Green - a->Green) * numerator / denom) >> 8;
                LONG blue = (a->Blue + (LONG)(b->Blue - a->Blue) * numerator / denom) >> 8;
                uint32_t colour = (uint32_t)(red & 0xFF) | ((uint32_t)(green & 0xFF) << 8) | ((uint32_t)(blue & 0xFF) << 16);
                raster_put_pixel(&target, x, y, rgb_to_dst(colour, target.format));
            }
        }
    }
    brain_snapshot_unlock();

    if (surface_is_primary(pdev, pso) && prclExtents != NULL) {
        brain_refresh_mark_dirty(prclExtents->left, prclExtents->top, prclExtents->right, prclExtents->bottom);
    }
    return TRUE;
}

static HBITMAP APIENTRY brain_CreateDeviceBitmap(DHPDEV dhpdev, SIZEL sizl, ULONG iFormat) {
    brain_pdev *pdev = (brain_pdev *)dhpdev;
    ddi_trace("ddi CreateDeviceBitmap");
    if (iFormat != BMF_16BPP || sizl.cx <= 0 || sizl.cy <= 0) {
        return (HBITMAP)0xFFFFFFFF;
    }
    brain_devbitmap *bitmap = (brain_devbitmap *)LocalAlloc(LPTR, sizeof(brain_devbitmap));
    if (bitmap == NULL) {
        return (HBITMAP)0xFFFFFFFF;
    }
    bitmap->width = sizl.cx;
    bitmap->height = sizl.cy;
    bitmap->stride = sizl.cx * 2;
    bitmap->bits = VirtualAlloc(NULL, (DWORD)bitmap->stride * bitmap->height,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (bitmap->bits == NULL) {
        LocalFree(bitmap);
        return (HBITMAP)0xFFFFFFFF;
    }
    HBITMAP handle = pdev->eng.EngCreateDeviceBitmap((DHSURF)bitmap, sizl, BMF_16BPP);
    if (handle == 0) {
        VirtualFree(bitmap->bits, 0, MEM_RELEASE);
        LocalFree(bitmap);
        return (HBITMAP)0xFFFFFFFF;
    }
    return handle;
}

static VOID APIENTRY brain_DeleteDeviceBitmap(DHSURF dhsurf) {
    brain_devbitmap *bitmap = (brain_devbitmap *)dhsurf;
    ddi_trace("ddi DeleteDeviceBitmap");
    if (bitmap == NULL) {
        return;
    }
    if (bitmap->bits != NULL) {
        VirtualFree(bitmap->bits, 0, MEM_RELEASE);
    }
    LocalFree(bitmap);
}

static BOOL APIENTRY brain_SetPalette(DHPDEV dhpdev, PALOBJ *ppalo, FLONG fl, ULONG iStart, ULONG cColors) {
    return TRUE;
}

static ULONG brain_masks[3] = {BRAIN_RED_MASK, BRAIN_GREEN_MASK, BRAIN_BLUE_MASK};

static ULONG *APIENTRY brain_GetMasks(DHPDEV dhpdev) {
    return brain_masks;
}

static BOOL APIENTRY brain_EndDoc(SURFOBJ *pso, FLONG fl) {
    return FALSE;
}

static BOOL APIENTRY brain_StartDoc(SURFOBJ *pso, PWSTR docname, DWORD jobid) {
    return FALSE;
}

static BOOL APIENTRY brain_StartPage(SURFOBJ *pso) {
    return FALSE;
}

static BOOL APIENTRY brain_ContrastControl(DHPDEV dhpdev, ULONG cmd, ULONG *value) {
    (void)dhpdev; (void)cmd; (void)value;
    return TRUE;
}

static VOID APIENTRY brain_PowerHandler(DHPDEV dhpdev, BOOL off) {
    (void)dhpdev;
    if (off) {
        brain_log_set_suspended(1);
        brain_log("PowerHandler: suspend enter");
        brain_refresh_suspend();
        brain_log("PowerHandler: suspend thread paused");
        brain_panel_suspend();
        brain_log("PowerHandler: suspend done");
    } else {
        brain_log("PowerHandler: resume enter");
        brain_panel_resume_prepare();
        brain_pxp_resume();
        brain_te_recover();
        brain_log("PowerHandler: resume push full");
        brain_refresh_push_full();
        brain_log("PowerHandler: resume push done");
        brain_panel_display_on();
        brain_refresh_resume();
        brain_log("PowerHandler: resume done");
        brain_log_hold_file_io(5000);
        brain_log_set_suspended(0);
    }
}

#ifndef QUERYESCSUPPORT
#define QUERYESCSUPPORT 8
#endif
#define DRVESC_SETSCREENROTATION 6301
#define DRVESC_GETSCREENROTATION 6302
#define DISPLAY_PANEL_COMMAND 100153
#define DISPLAY_NIGHT_LIGHT_PREVIEW 100155
#define BRAIN_ESC_FORCE_RECOVER 0x42524543

static ULONG APIENTRY brain_Escape(DHPDEV dhpdev, SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
                                   ULONG cjOut, PVOID pvOut) {
    brain_pdev *pdev = (brain_pdev *)dhpdev;
    (void)pso;

    if (iEsc == QUERYESCSUPPORT) {
        if (pvIn != NULL && cjIn >= sizeof(ULONG)) {
            ULONG query = *(ULONG *)pvIn;
            if (query == DRVESC_GETSCREENROTATION || query == DRVESC_SETSCREENROTATION) {
                return 1;
            }
            if (query == DISPLAY_NIGHT_LIGHT_PREVIEW) {
                return 1;
            }
            if (query == DISPLAY_PANEL_COMMAND && brain_refresh_panel_escapes_enabled()) {
                return 1;
            }
            if (brain_present_supported(query)) {
                return 1;
            }
        }
        return 0;
    }
    if (brain_present_supported(iEsc)) {
        return brain_present_escape(iEsc, cjIn, pvIn, cjOut, pvOut);
    }
    if (iEsc == DISPLAY_PANEL_COMMAND) {
        if (!brain_refresh_panel_escapes_enabled() || pvIn == NULL || cjIn == 0) {
            return (ULONG)-1;
        }
        brain_log_value("escape: panel command bytes", cjIn);
        return brain_refresh_panel_command((const uint8_t *)pvIn, cjIn) ? 0 : (ULONG)-1;
    }
    if (iEsc == DISPLAY_NIGHT_LIGHT_PREVIEW) {
        if (pvIn == NULL || cjIn < 2 * sizeof(ULONG)) {
            return (ULONG)-1;
        }
        ULONG active = ((ULONG *)pvIn)[0];
        ULONG strength = ((ULONG *)pvIn)[1];
        brain_log_value("escape: night light active", active);
        brain_log_value("escape: night light strength", strength);
        brain_refresh_night_light_preview((int)active, strength);
        return 0;
    }
    if (iEsc == DRVESC_SETSCREENROTATION) {
        pdev->orientation = cjIn;
        brain_log_value("escape: set rotation dmdo", cjIn);
        return 0;
    }
    if (iEsc == DRVESC_GETSCREENROTATION) {
        if (pvOut != NULL && cjOut >= sizeof(ULONG)) {
            *(ULONG *)pvOut = (0x7u << 8) | (pdev->orientation & 0xFF);
            return 0;
        }
        return (ULONG)-1;
    }
    if (iEsc == BRAIN_ESC_FORCE_RECOVER) {
        if (pvIn == NULL || cjIn < sizeof(ULONG)) {
            return (ULONG)-1;
        }
        ULONG level = *(ULONG *)pvIn;
        brain_log_value("escape: force recover", level);
        if (level == 4) {
            brain_te_recover();
        } else {
            brain_panel_recover(level);
        }
        brain_refresh_mark_dirty(0, 0, pdev->width, pdev->height);
        return 0;
    }
    return (ULONG)-1;
}

static ULONG APIENTRY brain_slot_stub_28(DHPDEV dhpdev) {
    (void)dhpdev;
    return 0;
}

static ULONG APIENTRY brain_slot_stub_30(DHPDEV dhpdev) {
    (void)dhpdev;
    return 0;
}

BOOL APIENTRY DrvEnableDriver(ULONG engine_version, ULONG cj, DRVENABLEDATA *pded, PENGCALLBACKS callbacks) {
    static ENGCALLBACKS saved_callbacks;
    (void)engine_version;

    brain_log("brain-display " BRAIN_DISPLAY_VERSION " " BRAIN_BOARD_NAME);
    brain_log_value("DrvEnableDriver: cj", cj);
    brain_present_init();

    if (callbacks != NULL) {
        memcpy(&saved_callbacks, callbacks, sizeof(saved_callbacks));
        g_eng = saved_callbacks;
    }

    memset(pded, 0, cj < sizeof(DRVENABLEDATA) ? cj : sizeof(DRVENABLEDATA));
    pded->DrvEnablePDEV = brain_EnablePDEV;
    pded->DrvDisablePDEV = brain_DisablePDEV;
    pded->DrvEnableSurface = brain_EnableSurface;
    pded->DrvDisableSurface = brain_DisableSurface;
    pded->DrvCreateDeviceBitmap = brain_CreateDeviceBitmap;
    pded->DrvDeleteDeviceBitmap = brain_DeleteDeviceBitmap;
    pded->DrvRealizeBrush = brain_RealizeBrush;
    pded->DrvStrokePath = brain_StrokePath;
    pded->DrvFillPath = brain_FillPath;
    pded->DrvPaint = brain_Paint;
    pded->DrvBitBlt = brain_BitBlt;
    pded->DrvCopyBits = brain_CopyBits;
    pded->DrvAnyBlt = brain_AnyBlt;
    pded->DrvTransparentBlt = brain_TransparentBlt;
    pded->DrvSetPalette = brain_SetPalette;
    pded->DrvSetPointerShape = brain_SetPointerShape;
    pded->DrvMovePointer = brain_MovePointer;
    pded->DrvGetModes = brain_GetModes;
    pded->DrvRealizeColor = brain_RealizeColor;
    pded->DrvGetMasks = brain_GetMasks;
    pded->DrvUnrealizeColor = brain_UnrealizeColor;
    pded->DrvContrastControl = brain_ContrastControl;
    pded->DrvPowerHandler = brain_PowerHandler;
    pded->DrvEndDoc = brain_EndDoc;
    pded->DrvStartDoc = brain_StartDoc;
    pded->DrvStartPage = brain_StartPage;
    pded->DrvEscape = brain_Escape;
    pded->DrvGradientFill = brain_GradientFill;

    void **slots = (void **)pded;
    if (cj >= 29 * sizeof(void *)) {
        slots[28] = (void *)brain_slot_stub_28;
    }
    if (cj >= 31 * sizeof(void *)) {
        slots[30] = (void *)brain_slot_stub_30;
    }

    brain_log("DrvEnableDriver: done");
    return TRUE;
}

VOID APIENTRY DrvDisableDriver(VOID) {
}

BOOL APIENTRY HALInit(VOID) {
    return FALSE;
}
