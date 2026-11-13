/*
 * present_minimal.c - Minimal brain-display present-mode app. Opens a
 * full-screen, double-buffered RGB565 surface, presents an animated fill on
 * every TE, and closes on Escape. Build as a Windows CE app for the device and
 * run it full-screen. See docs/present.md for the walkthrough.
 */
#include <windows.h>
#include <string.h>

#include "../abi/brain_present.h"

static void draw_frame(UINT16 *pixels, DWORD width, DWORD height, DWORD stride_px, int tick) {
    DWORD x;
    DWORD y;
    for (y = 0; y < height; y++) {
        UINT16 *row = pixels + (size_t)y * stride_px;
        for (x = 0; x < width; x++) {
            row[x] = (UINT16)((((x + (DWORD)tick) >> 3) & 0x1F) << 11);
        }
    }
}

int present_minimal(void) {
    HDC dc = GetDC(NULL);
    BrainPresentOpen open;
    BrainPresentInfo info;
    DWORD buffer = 0;
    int tick = 0;

    memset(&open, 0, sizeof(open));
    open.Width = (DWORD)(GetDeviceCaps(dc, HORZRES) & ~7);
    open.Height = (DWORD)(GetDeviceCaps(dc, VERTRES) & ~7);
    open.Scale = 1;
    open.Buffers = 2;
    open.Background = 0;
    open.Flags = BRAIN_PRESENT_FAST_CPU;

    if (ExtEscape(dc, BRAIN_PRESENT_OPEN, sizeof(open), (LPCSTR)&open, sizeof(info),
                  (LPSTR)&info) <= 0) {
        ReleaseDC(NULL, dc);
        return 0;
    }

    while ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0) {
        DWORD stride_px = info.Stride / (DWORD)sizeof(UINT16);
        ExtEscape(dc, BRAIN_PRESENT_WAIT, sizeof(buffer), (LPCSTR)&buffer, 0, NULL);
        draw_frame(info.pBuffers[buffer], info.Width, info.Height, stride_px, tick++);
        ExtEscape(dc, BRAIN_PRESENT, sizeof(buffer), (LPCSTR)&buffer, 0, NULL);
        buffer ^= 1;
    }

    ExtEscape(dc, BRAIN_PRESENT_CLOSE, 0, NULL, 0, NULL);
    ReleaseDC(NULL, dc);
    return 1;
}
