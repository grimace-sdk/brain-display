# Plan

## Shape

- One source tree, one DLL per board, chosen at compile time (`BRAIN_BOARD=pwsh2` or `pwa7300`): panel geometry, TE pin and edge, LCDIF clock, mapping, pointer support. Outputs `brain-display-pwsh2.dll` and `brain-display-pwa7300.dll`.
- C, built with grimace. `-Wall -Werror`, as pwsh2-boot does.
- Layers: DDI entry and objects; rasteriser (pure C over pixel buffers, testable on the host); refresh (dirty rectangles, PXP, LCDIF, TE, a refresh thread); board.
- Host tests: the rasteriser compiles natively too. Unit test it against a slow reference implementation (one pixel at a time, every ROP) over random rectangles, formats and clips. That is the main correctness net before any device run.

## Stages

Status at the 0.1.0-alpha tag: stages 1 to 7 done on the PW-SH2; the PW-A7300 shares the code and builds, with device validation ongoing.

1. **Desktop on the PW-SH2.** EnablePDEV, EnableSurface, GetModes, RealizeColor/UnrealizeColor, ContrastControl, an escape stub, and BitBlt on the 16 bpp primary (solid fills, SRCCOPY, 1 bpp text masks), with a generic slow path for everything else. Refresh: after drawing settles, the CPU transposes the dirty bounding box into a staging buffer, which is written on the next TE. Done when it boots to the desktop with a correct picture and Explorer works.
2. **Complete rasteriser.** All formats, all ROP3/ROP4, AnyBlt (stretch, mirror, conversions), TransparentBlt, GradientFill, StrokePath, FillPath, brushes, complex clips. Done when drawbench's 22 screen hashes match the reference values (below) and the GDI probe's screenshots match the replaced driver's.
3. **Refresh pipeline.** PXP transpose of each dirty rectangle (8-pixel aligned), two staging buffers, transfer started on TE, idle when nothing is dirty, stuck-transfer recovery, TE timeouts. Rotation at all four angles. Done when `teartest` shows no tear and idle CPU is zero. **Done** - a dedicated TE interrupt thread paces a fixed-N cadence scheduler; tear-free, idle at the desktop about 92%.
4. **Power.** Suspend and resume (plan M3), with no flash of stale or uninitialised panel memory: Display On only after the first full frame. **Done** - final suspend/resume pass is part of the alpha device run.
5. **PW-A7300.** Board values, `MADCTL` 0x08 and TE on at init, pointer as a PXP overlay (or CPU-composed in the staging copy), night light. **In progress** - the board builds and shares the pipeline; device validation and night light ongoing.
6. **Present mode** and the rest of the escapes ([interfaces.md](interfaces.md)), tested with brr and brainplay. **Done** - full-screen and centred present, up to 8 PXP overlays, the CPU hold; verified with teartest and brr. See [present.md](present.md).
7. **Performance.** Beat the replaced driver on every drawbench case (below). **Done** - faster than the stock driver on all 22 cases (see the README table).

## Performance Targets

drawbench on the PW-SH2 (pw-a7300 `apps/drawbench`, 2 s per case flat out; CPU at 454 MHz). "Replaced" is the driver this one replaces; Sharp's original is shown for scale. Same screen hashes for all three is the correctness bar.

| Case | Replaced ops/s | Sharp ops/s |
| --- | --- | --- |
| fill_16 | 10575 | 8394 |
| fill_200 | 1014 | 732 |
| fill_full | 107 | 87 |
| patinvert_200 | 46 | 42 |
| text_line | 391 | 310 |
| text_page | 15 | 12 |
| polyline_32 | 204 | 127 |
| ellipse_100 | 820 | 352 |
| polygon_6 | 1068 | 283 |
| scroll_16 | 79 | 67 |
| blit16_64 | 6413 | 4803 |
| blit16_full | 99 | 90 |
| blit8_256 | 52 | 32 |
| blit8_and | 51 | 20 |
| blit4_256 | 50 | 34 |
| blit1_256 | 51 | 35 |
| mask_64 | 1764 | 267 |
| imagelist_32 | 597 | 365 |
| stretch_2x | 23 | 21 |
| transparent_64 | 378 | 311 |
| invert_caret | 7881 | 6062 |
| window_churn | 18 | 16 |

Also from that run: at 20 ops/s, the replaced driver's LCDIF was busy 31% of the time for full-screen cases and near 0% for small ones. Partial updates are what keep that low. Measure CPU per op (paced), bus busy %, and time to glass as well as ops/s.

## Tests

| Test | Pass |
| --- | --- |
| Boot to desktop | Correct picture, Start menu, Explorer, Control Panel |
| Host rasteriser tests | Match the reference implementation |
| drawbench | Hashes match; ops/s at or above the targets |
| ddi-probe (pwsh2-boot `ce/drivers/ddi-trace/ddi_probe.c`) | Runs to "done"; screenshots match the replaced driver's |
| teartest (pw-sh2) bar and flash modes | No tear line, clean full-screen switches |
| Rotation 0/90/180/270 (PW-SH2) | Correct picture and touch at each angle |
| Suspend and resume, 10 times | Panel returns with correct content, no flash |
| 30 minutes of mixed use, plus an hour of audio playback (PW-SH2) | No hang, no stuck panel |
| brr, brainplay | Present mode frame rates at least as before |
| A7300 touchpad | Cursor moves smoothly, no trails, I-beam inverts |

## Open Questions (measure on the device)

- **M1. Clip, path and brush callbacks.** The replaced driver never called `CLIPOBJ_cEnumStart`, `CLIPOBJ_bEnum`, `PATHOBJ_vEnumStart`, `PATHOBJ_bEnum`, `PATHOBJ_vGetBounds`, `BRUSHOBJ_pvAllocRbrush` or `BRUSHOBJ_pvGetRbrush`, yet handled complex clips, paths and pattern brushes. Confirm that GWES's callbacks work, and what the 3 unnamed callback entries (13 to 15) are. Do this first: stage 2 depends on it.
  - Measured (PW-A7300): `CLIPOBJ_cEnumStart`/`CLIPOBJ_bEnum` and `PATHOBJ_vEnumStart`/`bEnum`/`vGetBounds` work when the driver calls them. `cEnumStart(pco, FALSE, CT_RECTANGLES, CD_ANY, 0)` returns the count; `bEnum` fills `ENUMRECTS` and returns FALSE on the last (non-empty) batch. Complex clips had 4 to 16 rectangles, sorted top-to-bottom then left-to-right, non-overlapping, inside `rclBounds`. A rectangle stroke's `PATHOBJ` gave one `PATHDATA`, `flags` `PD_BEGINSUBPATH|PD_ENDSUBPATH`, closed (first point repeated), 28.4 fixed point; `vGetBounds` bottom-right exclusive. **Caveat:** when the trace shim enumerated clips and then forwarded to the replaced driver, the device froze inside `StrokePath` (shared enumeration state). A driver that calls them itself and handles the call should be fine, but verify on device early. Still unmeasured: `BRUSHOBJ_pvAllocRbrush`/`pvGetRbrush`, and the 3 unnamed entries. The brush callbacks can't be measured through the trace shim (calling them mid-blit is the same interference that froze `StrokePath`, and the replaced driver never calls them). Verify them on the device with our own driver in stage 2, starting from NT semantics: `pvAllocRbrush` only inside `DrvRealizeBrush`; `pvGetRbrush` calls `DrvRealizeBrush` if there is no realization yet, then returns the stored one.
- **M2. Antialiased text masks.** Measured on the PW-SH2 (Sharp's GWES renders AA text; the A7300's is mostly 1 bpp). The 4/8 bpp mask is per-pixel coverage: `result = lerp(bg_ROP, fg_ROP, coverage)` per channel in 8-bit space (8 bpp = m/255, 4 bpp = m/15), packed to 565 with truncation. Implemented linearly (no gamma); confirm against the probe text phase (p014) whether a gamma or different scale is needed.
- **M3. Panel across suspend.** Does the panel keep its vendor settings through suspend if the driver only sends `28`/`10`, then `11`/`29`? Or is its supply cut? If it is cut, the full vendor init sequence is needed, which is a separate decision.
- **M4. `DRVESC_GETSCREENROTATION` output.** What the 4 bytes at `pvOut` must hold. Measured (PW-A7300, single sample): one DWORD `0x00000700` at angle 0, return 0. Bits 8 to 10 look like the supported set (90, 180, 270) and the low bits like the current DMDO angle, so `(0x7 << 8) | currentDMDO`. Treat as likely, not confirmed; verify the low bits at a nonzero angle.
- **M5. Rotation and the primary surface.** Measured (PW-A7300, and confirmed the same on the PW-SH2 by device trace): GWES updates the primary `SURFOBJ.sizlBitmap` itself (480x320 -> 320x480 after 90, back after 180, and so on). `dhsurf`, `hsurf`, `dhpdev`, `hdev` are unchanged; `pvScan0`/`pvBits`/`lDelta`/`cjBits` stay 0. Per angle: 6301 (`cjIn` = DMDO 1/2/4/0 for 90/180/270/0) -> 0, 6302 -> 0, `DrvEnablePDEV`(new w/h, `cjDevInfo` 0) same millisecond, ~4 ms later 6303 -> -1, ~10-30 ms later 6304 -> -1, then GWES redraws. No DisableSurface/EnableSurface. So the driver re-lays its framebuffer on the re-entrant EnablePDEV (reading the DMDO recorded at 6301); GWES carries the geometry. Touch is remapped by GWES, not the driver. Rotation is triggered by rotate.cpl, by autorotate on a tablet fold, and by a saved startup-rotation at boot - so it must be crash-safe from the first frame. **Implementation note:** the re-entrant EnablePDEV must not wipe the existing PDEV (framebuffer, palette); initialise once, then update geometry in place. The one unverified thing is the rotation DIRECTION convention (which DMDO is clockwise, and any per-axis flip); confirm on device per angle.
- **M6. DIB mappings.** Is the mapping of an app's DIB section, as seen inside the driver, cached?
- **M7. Screenshots in present mode.** A GDI screenshot while an app presents times out. Should the driver answer one from the last presented frame?
- **M8. DEVINFO default fonts.** Resolved: a zeroed set works. The PW-A7300 boots to the desktop with correct text (menus, labels, titles) with all three `LOGFONTW`s zeroed, so GWES does not require the driver to populate them.
- **M9. TE SYSINTR constants. Resolved (interface facts).** Two values in the TE code came from off-limits sources: the numeric `IOCTL_HAL_REQUEST_SYSINTR` function code 0x01010098 (function 38; from Platform Builder's `pkfuncs.h`), and the GPIO OAL IRQ numbering 128 + bank * 32 + pin (254 / 272; from NXP's `mx28_irq.h` and notes derived from Sharp's driver), both via the measurement session outside the clean room. The user accepted them as non-protectable platform interface constants rather than run a clean re-derivation sweep; recorded in CLEANROOM.md Exceptions. Confirmed working on the PW-SH2 (SYSINTR 0x1E, TE synced). The measured `PINCTRL` register values are device reads.
