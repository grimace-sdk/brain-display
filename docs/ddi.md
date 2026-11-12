# DDI Surface

Measured on a PW-SH2 (Sharp's CE 6.0 GWES) and a PW-A7300 (our pointer-enabled GWES) with `ddi-trace.dll`. The shim takes the `ddraw_mx28.dll` slot, forwards every call to the current driver, and records arguments, return values and call counts. The workload was boot, 23 apps, a GDI probe covering every GDI export in the ROM's coredll (pwsh2-boot `ce/drivers/ddi-trace/ddi_probe.c`), rotation to all four angles, suspend and resume, touchpad use (A7300), and full-screen apps. Raw traces are in [../measurements](../measurements/).

Types and constants are grimace's `winddi.h`. Where it and this page disagree, the measurement wins; report it to grimace.

## Entry

`DrvEnableDriver(iEngineVersion, cj, pded, pEngCallbacks)`:

- `iEngineVersion` is 0x00040001.
- `cj` is 124. `DRVENABLEDATA` is a flat table of function pointers, not NT's `{version, count, DRVFN*}`: the 28 named in `winddi.h`, then 3 more slots. Leave the extra slots NULL. The driver this replaces fills slots 28 and 30, but GWES never called them. Measured (PW-A7300): with slots 28 to 30 forced NULL, boot, the full GDI probe, rotation through 90/180/270/0, and suspend/resume all ran and GWES never called them. Leaving them NULL is safe.
- `pEngCallbacks` points to 16 words; `winddi.h` names the first 13. Copy what you need. Only three were called during normal drawing, all at boot: `XLATEOBJ_cGetPalette`, `EngCreateDeviceSurface` and `EngCreatePalette`. The replaced driver also calls the 3 unnamed entries (13 to 15) once each at boot, during surface setup, with a first argument of 5; entries 14 and 15 are passed a GWES handle (not the `EngCreateDeviceSurface` HSURF). Their identity can't be determined black-box, and nothing calls them after boot. Whether our driver needs them will show when it boots without calling them.
- Export `HALInit` too. It was never called (no DirectDraw), so it can return FALSE.

## Calls

Counts from the PW-SH2 run, then the PW-A7300 run.

| Call | SH2 | A7300 | Notes |
| --- | --- | --- | --- |
| EnablePDEV | 1 | 5 | Once at boot, then again for each rotation |
| EnableSurface | 1 | 1 | Never called again, rotation included |
| CreateDeviceBitmap | 65 | 44 | Always format 4 (16 bpp) |
| DeleteDeviceBitmap | 59 | 42 | |
| RealizeBrush | 2 | 27 | 8x8 patterns in every format, also 64x64 |
| StrokePath | 183 | 847 | Cosmetic lines only, width 1 |
| FillPath | 27 | 371 | Polygons, ellipses, rounded rectangles |
| BitBlt | 15824 | 18881 | Everything else, including text |
| AnyBlt | 2058 | 1349 | Stretching, mirroring, and format conversion |
| TransparentBlt | 0 | 21 | Only from `TransparentImage` |
| GradientFill | 0 | 2 | Only from `GradientFill` |
| SetPointerShape | 0 | 26 | A7300 only |
| MovePointer | 0 | 397 | A7300 only |
| GetModes | 2 | 2 | Size query, then fill |
| RealizeColor | 260 | 500 | |
| UnrealizeColor | 85 | 152 | |
| ContrastControl | 3 | 3 | |
| PowerHandler | 2 | 2 | Suspend and resume |
| Escape | 14375 | 5321 | Mostly unsupported queries |

Never called on either device, so they can be omitted (NULL) or return failure: DisablePDEV, DisableSurface, Paint, CopyBits, SetPalette, GetMasks, StartDoc, EndDoc, StartPage, HALInit. MaskBlt produces no DDI call at all. Keep Paint and CopyBits as thin wrappers over BitBlt anyway, in case another app reaches them.

## Setup

### DrvEnablePDEV

- At boot: `cjCaps` 152, `cjDevInfo` 296, `pdm` a CE `DEVMODEW` (192 bytes, `dmDisplayOrientation` at offset 188).
- On rotation it is called again with the rotated width and height (A7300: 320x480 after 480x320) and **`cjDevInfo` 0**. Don't fail. Fill `GDIINFO` and skip `DEVINFO`.
- No DisablePDEV follows, and EnableSurface is not called again. The driver keeps one PDEV and one primary surface, and changes their geometry in place.

`GDIINFO` as the replaced driver fills it, in words (A7300; on the SH2 words 4 and 5 are 800 and 480):

```
0x00040001 0x00000001 0x00000040 0x0000003c 0x000001e0 0x00000140 0x00000010 0x00000001
0x00010000 0x00012801 0x00000060 0x00000060 0x01000500 0x00000017 0 0
0 1 1 1 0 ...
```

That is version 0x40001, `DT_RASDISPLAY`, 64 x 60 mm, the resolution, 16 bpp, 1 plane, 65536 colours, `flRaster` 0x12801, 96 dpi, word 12 the same value as `DEVINFO.flGraphicsCaps`, `flTextCaps` 0x17, aspect 1/1/1.

`DEVINFO.flGraphicsCaps` is 0x01000500. Start with the same values. Changing caps can change what GWES sends.

### DrvEnableSurface

Creates the primary with `EngCreateDeviceSurface`, format 4 (16 bpp). The palette comes from `EngCreatePalette(PAL_BITFIELDS, 0, NULL, 0xF800, 0x07E0, 0x001F)`. The primary `SURFOBJ` reports `iType` 0. GWES passes it back to every drawing call, so the driver tells the primary apart from DIBs by `dhsurf`, not by `iType`.

Measured (PW-A7300): `EngCreateDeviceSurface(dhsurf, 480x320, format 4)` returns an `HSURF`, and `DrvEnableSurface` returns that same `HSURF`. But in the `SURFOBJ` GWES passes to drawing calls, `hsurf` is **not** that returned handle; only `dhsurf` matches the value the driver gave `EngCreateDeviceSurface`, and it is stable. On the primary, `pvBits`, `pvScan0`, `lDelta` and `cjBits` are all 0: GWES knows nothing about the pixels. **The driver owns the framebuffer and finds it through `dhsurf`.** Detect the primary by `dhsurf`, not `hsurf`.

Device bitmaps (if the driver creates them) are the same: `dhsurf` set, `pvBits`/`pvScan0`/`cjBits` 0, but `lDelta` can be nonzero (64x16 -> 0x80, 128x32 -> 0x100). They are driver-owned too, found by `dhsurf`. DIBs have `dhsurf` 0 and carry their own `pvScan0`/`lDelta` (usually bottom-up, negative `lDelta`).

### DrvGetModes

One mode. Called with `cjSize` 0 (return the size, 216 bytes), then with a buffer. The mode is the panel's landscape size, 16 bpp, 60 Hz, `dmDeviceName` "GPE", 24 bytes of driver extra. Copy the field values from the `GetModes DEVMODEW` dump in the traces.

### DrvCreateDeviceBitmap and DrvDeleteDeviceBitmap

Always format 4 (16 bpp). Measured (PW-A7300): this GWES only treats the return value `0xFFFFFFFF` as "decline"; it uses any other value, 0 included, as a bitmap object and dereferences it (returning 0 crashed GWES in Explorer with a NULL+0x24 write). **Return `(HBITMAP)0xFFFFFFFF` to decline** and have GWES create an ordinary bitmap, not 0. Device bitmaps are only worth having if they speed up blits from offscreen to screen (measure).

## Drawing

See [rasteriser.md](rasteriser.md) for what each drawing call must handle. Every drawing call can target the primary or a DIB of any format, and can read from any format. Each drawing call to the primary marks its destination rectangle dirty for the refresh path ([hardware.md](hardware.md)).

## Colour

- `DrvRealizeColor(iDstType, cEntries, pPalette, rgbColor)` maps a COLORREF to a pixel value for a palette type: `iDstType` 1 (`PAL_INDEXED`) with 2, 4, 16 or 256 entries, 2 (`PAL_BITFIELDS`) with 3 masks, 8 (`PAL_BGR` in NT's numbering) with none. Return the nearest index for indexed, the packed value for bitfields.
- `DrvUnrealizeColor(iSrcType, cEntries, pPalette, iRealizedColor)` is the inverse. It was seen with `PAL_BITFIELDS` and 3 entries.

## Pointer (A7300)

- `DrvSetPointerShape(pso, psoMask, psoColor, pxlo, xHot, yHot, x, y, prcl, fl)`: `psoMask` is a 1 bpp surface twice as tall as the cursor (32x64: AND mask above, XOR mask below), `psoColor` NULL, `fl` 0. A NULL `psoMask` hides the cursor. Return 2 (`SPS_ACCEPT_NOEXCLUDE`): the driver draws the cursor itself and GWES never hides it around blits.
- `DrvMovePointer(pso, x, y, prcl)`: never called with `x` == -1 here, but treat -1 as hide.
- The cursor must never be drawn into the primary surface. Compose it at refresh time (PXP overlay, or the CPU on the staging copy). A move marks the old and new cursor rectangles dirty.

## Power

`DrvPowerHandler(dhpdev, bOff)`: `bOff` TRUE on suspend, FALSE on resume. The full sequence on both devices:

1. Escape 100151 (unsupported, return -1)
2. `DrvPowerHandler(TRUE)`
3. suspend
4. `DrvPowerHandler(FALSE)`
5. Escape 100151
6. `DrvContrastControl(0, ...)`, then `DrvContrastControl(1, ...)`

`DrvContrastControl(cmd, pValue)` is also called once at boot with `cmd` 1. There is no contrast hardware. Return TRUE and leave `*pValue` alone, unless measurement shows the replaced driver does something else.

`DrvPowerHandler` runs in GWES's power path, and the SD card is already suspended by the time it is called. Do no filesystem I/O from `DrvPowerHandler`, the panel suspend/resume sequence, or any thread it touches: a write to `\Storage Card` blocks forever on the suspended SD driver and wedges GWES (measured on PW-SH2: suspend hung, USB died, needed a reset; moving all power-path logging to a RAM buffer flushed after resume fixed it). On resume the LCDIF loses `LCDIF_MASTER` and `DATA_SELECT` (PW-SH2 CTRL read 0x04000000 at resume entry); save CTRL/CTRL1/CTRL2/TIMING at init and restore them, re-ungate the CLKCTRL clock, then `SLEEP_OUT`, a settle delay, re-send COLMOD/MADCTL, push a full frame, and only then `DISPLAY_ON`.

## Escapes

See [interfaces.md](interfaces.md).

## Trace Key

The trace files group each call by a "shape" key:

- `target`, `source`, `mask`: `iBitmapFormat | iType << 8 | fjBitmap << 16`, or 0xffffffff for NULL. Formats: 1 = 1 bpp, 2 = 4 bpp, 3 = 8 bpp, 4 = 16 bpp, 5 = 24 bpp, 6 = 32 bpp, 9 = 2 bpp (CE only). `fjBitmap` 0x08 appears on some app bitmaps (meaning unconfirmed).
- `clip`: `CLIPOBJ.iDComplexity` (0 trivial, 1 rect, 3 complex), 0xffffffff for NULL.
- `brush`: 0 solid (`iSolidColor` set), 1 pattern, 0xffffffff NULL.
- `flXlate`, `xlateTypes`: `XLATEOBJ.flXlate`, and the source and destination palette types packed as `iSrcType | iDstType << 16`. Values that look like pointers come from calls with no source and mean nothing.
- `phase N:` lines count calls per probe phase. The phase numbers match the `pNNN` lines in the probe logs.
- `events:` are timestamped (ms since boot) records of EnablePDEV (width, height, `cjDevInfo`), ContrastControl, PowerHandler and escapes (`iEsc`, `cjIn`, return value).
