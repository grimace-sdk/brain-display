# Rasteriser

GWES hands all drawing to the driver, including DIB to DIB, so the rasteriser has to be complete over the formats below and fast on the few paths that carry almost all the traffic.

## Formats

| `iBitmapFormat` | Pixel | Seen as |
| --- | --- | --- |
| 1 | 1 bpp | Source, destination, mask, brush pattern |
| 9 | 2 bpp (CE) | Source, destination, brush pattern |
| 2 | 4 bpp | Source, destination, mask, brush pattern |
| 3 | 8 bpp | Source, destination, mask, brush pattern |
| 4 | 16 bpp | Primary, device bitmaps, DIBs, brush pattern |
| 5 | 24 bpp | Source, destination, brush pattern |
| 6 | 32 bpp | Source, destination, brush pattern |

The probe reached every source and destination pair across these seven formats through BitBlt. `lDelta` may be negative (bottom-up DIBs, for example a 56x36 2 bpp bitmap with `lDelta` -16). Rows are 32-bit aligned.

Format conversion goes through the `XLATEOBJ`: `flXlate` (`XO_TRIVIAL`, `XO_TABLE`, ...), `pulXlate` for indexed sources, and `XLATEOBJ_cGetPalette` for bitfields and RGB sources. `iSrcType`/`iDstType` pairs seen: indexed to bitfields, bitfields to bitfields, BGR to bitfields, and indexed to indexed.

## Where the Traffic Is

From the PW-A7300 run (18,881 BitBlts, 1,349 AnyBlts, 847 StrokePaths):

| Path | Share | Notes |
| --- | --- | --- |
| BitBlt, no source, 16 bpp destination, ROP 0xF0 (PATCOPY), solid brush | ~70% | Fills: backgrounds, rectangles, `ExtTextOut` opaque boxes |
| BitBlt, ROP4 0xAAF0, 1 bpp mask, solid brush, no source | ~28% | Text: brush colour where the mask bit is 1, destination unchanged where 0 |
| BitBlt 16 bpp to 16 bpp, ROP 0xCC (SRCCOPY) | ~2% | Scrolling, window moves, offscreen to screen, cursor-free copies |
| AnyBlt 1/4/24 bpp to 16 bpp, no stretch | | Icons and images: format conversion through the XLATEOBJ |
| StrokePath, width 1 | | Mostly mix 0x0B07 (XOR, NOP background): drag outlines and focus lines. Then 0x0D0D (`R2_COPYPEN`) |

Make these fast first: word-aligned 32-bit stores, two RGB565 pixels at a time, `stm` for runs. The data cache does not allocate on writes, so a store to an uncached line goes straight to DRAM, one bus transfer per store. 16-bit stores in a loop are the slowest way to fill.

Everything else must be correct but can be generic.

## BitBlt

`DrvBitBlt(psoTrg, psoSrc, psoMask, pco, pxlo, prclTrg, pptlSrc, pptlMask, pbo, pptlBrush, rop4)`

- All 256 ROP3s reach the driver (the probe drew each one). The low byte is the foreground ROP3 and the high byte the background ROP3, used where the mask bit is 0. Without a mask, both bytes are equal.
- Masks: `psoMask` 1 bpp for normal text. 4 bpp (A7300) and 8 bpp (PW-SH2 probe, 528 calls) masks were also seen with ROP4 0xAAF0, which is antialiased text. How the coverage values are meant to be applied is an open question (plan.md, M2).
- `pptlBrush` is the brush origin for patterns.
- Source and destination may be the same surface and overlap (`ScrollDC`, `ScrollWindowEx`, window moves). Choose the copy direction from the overlap.
- `prclTrg` is in destination coordinates, bottom-right exclusive. It is not always ordered: normalise it.

## AnyBlt

`DrvAnyBlt(psoDst, psoSrc, psoMask, pco, pxlo, pptlHTOrg, prclDst, prclSrc, pptlMask, pbo, pptlBrush, rop4, iMode, bltFlags)`

- `bltFlags` 8 (`BLT_STRETCH`) when the sizes differ. Mirroring comes as `prclDst` or `prclSrc` with left > right or top > bottom.
- `iMode` 1, 3 and 4 seen (`BLACKONWHITE`, `COLORONCOLOR`, `HALFTONE` in NT's numbering). Nearest neighbour is acceptable for all three. Shrinking 1 bpp with `BLACKONWHITE` should keep black pixels, ANDing source pixels that map to one destination pixel.
- Without stretch it is used for format conversions (`StretchDIBits`, `SetDIBitsToDevice`, icons).
- ROP4 seen: 0xCCCC (SRCCOPY), 0x8888 (SRCAND), 0x6666 (SRCINVERT), 0xEEEE (SRCPAINT). Icons are drawn as an AND pass and then an XOR pass.

## TransparentBlt

`DrvTransparentBlt(psoDst, psoSrc, pco, pxlo, prclDst, prclSrc, iTransColor, ulReserved)`: from `TransparentImage` only, every source format to 16 bpp. `iTransColor` is in the source format. Pixels equal to it are skipped. May stretch.

## GradientFill

`DrvGradientFill(psoDst, pco, pxlo, pVertex, nVertex, pMesh, nMesh, prclExtents, pptlDitherOrg, ulMode)`: modes 0 (`GRADIENT_FILL_RECT_H`) and 1 (`GRADIENT_FILL_RECT_V`) seen. The triangle request in the probe (mode 2) never reached the driver. `TRIVERTEX` colours are 16-bit per channel.

## Lines: StrokePath

`DrvStrokePath(pso, ppo, pco, pxo, pbo, pptlBrushOrg, plineattrs, mix)`

- `mix` holds R2 codes, foreground in the low byte and background (for style gaps) in the high byte. All 16 codes were seen as 0x0101 to 0x1010. 0x0D0D is `R2_COPYPEN`. 0x0B07 (`R2_XORPEN`, `R2_NOP` background) was the most common on the A7300, from window drag outlines.
- `LINEATTRS`: `fl` 2 for solid lines. Styled lines come with `fl` 4 or 0 and a 2-entry style array. Check the flag meanings against `winddi.h` on the device. Width is always 1 (cosmetic), and wide geometric lines never arrive.
- `plineattrs` NULL was also seen. Treat it as solid, width 1.
- Points come from the `PATHOBJ`. `PATHOBJ_vEnumStart`/`PATHOBJ_bEnum` exist in the callback table, but the replaced driver never called them (plan.md, M1). Coordinates are 28.4 fixed point (`POINTFIX`).
- Endpoint rule: the last pixel is excluded, as in GDI.

## Areas: FillPath

`DrvFillPath(pso, ppo, pco, pbo, pptlBrushOrg, mix, flOptions)`: polygons, ellipses, chords, pies and rounded rectangles arrive as paths, already flattened as far as we know (to be confirmed with M1). `flOptions` 2 (`FP_WINDINGMODE`) or 0 (alternate). Solid brushes only in the traces. All 16 mixes.

## Brushes

`DrvRealizeBrush(pbo, psoTarget, psoPattern, psoMask, pxlo, iHatch)`: 8x8 patterns in formats 1, 2, 3, 4, 5, 6 and 9, plus a 64x64 16 bpp pattern. `psoMask` NULL, `iHatch` 0. Store a 16 bpp 8x8 (or the given size) copy with `BRUSHOBJ_pvAllocRbrush` and fetch it with `BRUSHOBJ_pvGetRbrush`, or keep your own cache. These callbacks were never called by the replaced driver, so check they work (M1). Solid brushes come with `iSolidColor` already in the destination format and never reach RealizeBrush.

## Clipping

`CLIPOBJ.iDComplexity`: 0 (`DC_TRIVIAL`), 1 (`DC_RECT`, use `rclBounds`), 3 (`DC_COMPLEX`). Complex clips are about 4% of BitBlts. Enumerate them with `CLIPOBJ_cEnumStart`/`CLIPOBJ_bEnum`. Like the path callbacks, the replaced driver never called these (M1). A NULL `pco` means no clipping.

## Correctness Reference

drawbench (pw-a7300 `apps/drawbench`) hashes the screen after 48 seeded operations per case. The replaced driver produced the same hashes as Sharp's original, so matching them is the pixel-exact target. The GDI probe (`ddi_probe.c`) gives broader coverage, with screenshots compared across drivers.
