# Hardware

Both devices are i.MX28 (ARM926EJ-S, up to 454 MHz) with a command-mode (MIPI DBI type B, 8080) panel that holds its own frame memory. The LCDIF writes pixels to the panel only when the driver starts a transfer, so the driver decides when the panel changes. There is no scan-out from system memory.

Register names and offsets are from the i.MX28 Reference Manual. Check each one there before relying on it. i.MX28 registers have `_SET`, `_CLR` and `_TOG` aliases at +4, +8 and +0xC.

## Per Board

| | PW-SH2 | PW-A7300 |
| --- | --- | --- |
| Desktop (GDI, angle 0) | 800x480 landscape | 480x320 landscape |
| Panel frame memory | 480x800 portrait | 320x480 portrait |
| Panel controller | Ilitek ILI9805 (public datasheet) | Unidentified (no public match) |
| LCDIF clock | 48 MHz | 24 MHz |
| `HW_LCDIF_TIMING` | 0x01010101 | 0x01010101 |
| TE pin | GPIO 3_30 (pad `LCD_RESET`), rising edge | GPIO 4_16 (pad `ENET_CLK`), falling edge |
| TE period | about 16.9 ms | 20.1 ms (49.7 Hz) |
| Full-frame write | about 16 ms | about 6.4 ms |
| Pointer | none | yes (touchpad on the second screen) |
| Panel state left by the bootloader | Portrait (`MADCTL` 0x48), TE on (`35 00`), 16 bpp | Landscape with row/column exchange (`MADCTL` 0x28), TE off, 16 bpp |

Screen to panel mapping at angle 0:

- PW-SH2: panel column = 479 - screen y, panel page = screen x (columns mirrored).
- PW-A7300: panel column = screen y, panel page = screen x (a plain transpose), after the driver sends `MADCTL` 0x08.

Both mappings are worked out from how a portrait write must look on a landscape screen. Confirm each with a test pattern before building on it.

The full-frame write on the PW-SH2 takes about as long as one panel scan. So a transfer started on TE only just stays ahead of the scan line. Partial updates finish well inside a frame.

## LCDIF clock source

The bootloader leaves the LCDIF clocked from the 24 MHz crystal, so a full-frame write takes about 40 ms (about 2.4 TE periods). The replaced driver runs the same panel from `ref_pix` (the PLL `FRAC1` PIX fractional divider) at about 16 to 20 ms per full frame. Three registers differ (device reads, ours vs the replaced driver):

| Register | Offset | Ours | Replaced |
| --- | --- | --- | --- |
| `DIS_LCDIF` | 0x120 | 0x00000001 (DIV 1) | 0x00000009 (DIV 9) |
| `FRAC1` | 0x1C0 | 0x00929294 (PIX gated, frac 20) | 0x00929254 (PIX ungated, stable, frac 20) |
| `CLKSEQ` | 0x1D0 | 0x00004164 (bit 14 set) | 0x00000164 (bit 14 clear) |

`CLKSEQ` bit 14 `BYPASS_DIS_LCDIF`: 1 selects the crystal, 0 selects `ref_pix`. `FRAC1` low byte is the PIX fractional divider: bit 7 `CLKGATEPIX`, bit 6 `PIXSTABLE`, bits 5:0 `PIXFRAC`. With PIXFRAC 20, `ref_pix` = 480 MHz * 18 / 20 = 432 MHz, divided by `DIS_LCDIF` 9 = 48 MHz.

Clearing only the bypass would switch the LCDIF to a gated `ref_pix` (wedge) or, once ungated, to 432 MHz / 1 (far out of spec). `clock_enable` does the full sequence while the LCDIF is idle and still on bypass: set `DIS_LCDIF` DIV to 9 and wait for its BUSY bit (29) to clear, clear `CLKGATEPIX` via `FRAC1_CLR` (0x1C8, write 0x80) and wait for `PIXSTABLE`, then clear the bypass via `CLKSEQ_CLR` (0x1D8, write 0x4000). It re-applies on recover and resume.

Field meanings are from the i.MX28 Reference Manual (verify the bit layout there); register values are device reads. The per-board clock figures in the table above predate this finding; treat them as the target (PLL-sourced) rate.

## Panel Commands

Standard MIPI DCS, as 8-bit commands and parameters over the 16-bit bus:

| Command | Use |
| --- | --- |
| `2A` CASET, `2B` PASET | Set the panel window (start and end, 16-bit big-endian each) |
| `2C` RAMWR | Start writing pixels into the window |
| `35 00` / `34` | Tearing effect line on (V-blank only) / off |
| `36` MADCTL | Address order. Leave the PW-SH2's as the bootloader set it; set the PW-A7300's to 0x08 |
| `3A 55` | 16 bpp (already set) |
| `10` / `11` | Sleep in / out (wait 120 ms after `11` before writing) |
| `28` / `29` | Display off / on |

The driver should only need these. The panels' vendor registers (power, gamma, frame rate) are set by the bootloader. See plan.md M3 for what happens across suspend.

## LCDIF (0x80030000)

Already in system (8080) mode, 16-bit, bus master, when CE starts.

Offsets are the i.MX28 layout (verified against the RM and a device dump). Note `CTRL2` at 0x020: the i.MX23 layout has no `CTRL2`, which shifts `TRANSFER_COUNT`/`CUR_BUF`/`NEXT_BUF` down by one slot. Each register also has `_SET`/`_CLR`/`_TOG` aliases at +4/+8/+0xC.

| Register | Offset | Fields used |
| --- | --- | --- |
| `HW_LCDIF_CTRL` | 0x000 | `RUN` (0), `MASTER` (5), `DATA_SELECT` (16: 0 command, 1 data), `VSYNC_MODE` (18), `WAIT_FOR_VSYNC_EDGE` (27) |
| `HW_LCDIF_CTRL1` | 0x010 | `BYTE_PACKING_FORMAT` (19:16): 0xF for pixels, 0x3 for command bytes. `CUR_FRAME_DONE_IRQ` / `_EN` (13) |
| `HW_LCDIF_CTRL2` | 0x020 | Leave at the bootloader's value (0x00070010). Writing it here breaks transfers |
| `HW_LCDIF_TRANSFER_COUNT` | 0x030 | `V_COUNT` << 16 \| `H_COUNT` |
| `HW_LCDIF_CUR_BUF` | 0x040 | Physical address of the pixels |
| `HW_LCDIF_NEXT_BUF` | 0x050 | Physical address of the pixels (set same as `CUR_BUF`) |
| `HW_LCDIF_TIMING` | 0x060 | Leave as found (0x01010101) |
| `HW_LCDIF_DATA` | 0x180 | PIO writes for commands and parameters |
| `HW_LCDIF_STAT` | 0x1B0 | FIFO and busy state: `BUSY` (25), `TXFIFO_FULL` (27), `TXFIFO_EMPTY` (26), `PRESENT` (31) |
| `HW_LCDIF_DEBUG2` | 0x1F0 | Current DMA read address (useful for diagnosing a stall) |

Register state seen while running (PW-A7300): `CTRL` 0x04010021, `CTRL1` 0x000f2201, `CTRL2` 0x00070010, `TIMING` 0x01010101; `TRANSFER_COUNT` and `CUR_BUF` change per partial update.

Commands: wait for `RUN` clear, byte packing 0x3, `TRANSFER_COUNT` 1x1, `DATA_SELECT` clear for the command byte (set for parameters), `RUN`, write the byte to `DATA`, wait, then restore byte packing 0xF.

Pixels: after `2A`, `2B`, `2C`, set `CUR_BUF`/`NEXT_BUF` to the physical address of the band (in panel order, packed), `TRANSFER_COUNT` to rows << 16 | pixels per row, set `DATA_SELECT`, `MASTER` and `RUN`. In MPU master mode the LCDIF clears `RUN` itself when the frame is sent and sets `CUR_FRAME_DONE_IRQ` (`CTRL1` bit 9); wait on that, then clear `RUN`. PIO command bytes do not reliably clear `RUN`, so clear it explicitly before and after each byte, or the next command corrupts the window.

With 16 bpp and byte packing 0xF the LCDIF packs **two pixels per 32-bit word**, so `H_COUNT` (pixels per row, the column span after the transpose) must be even and each staging row must start on a word boundary. An odd `H_COUNT` shears every row and the image skews diagonally. Round the band so the column span (panel columns = the screen axis that becomes H_COUNT) is even.

A transfer can stick: `RUN` stays set with the FIFOs empty and an underflow flagged. Seen on the PW-SH2 after about an hour of audio playback. Stop it after 100 ms (clear `RUN`, clear the FIFOs). If it happens again, soft-reset the LCDIF and restore its registers. On the third time in a row, put the panel through sleep out, `MADCTL` and TE on again.

Save every LCDIF register at init and restore them on resume.

## TE

Use the TE pin as a GPIO interrupt: set its `PINCTRL` `IRQEN`, `IRQLEVEL` (edge) and `IRQPOL` bits for the edge in the table, clear `IRQSTAT`, and get a SYSINTR for it with `IOCTL_HAL_REQUEST_SYSINTR`. GPIO pins appear as OAL IRQ 128 + bank * 32 + pin. Each interrupt: clear `IRQSTAT` for the pin, then `InterruptDone`. Mask it while idle.

The `PINCTRL` register layout and the measured pin states (`PIN2IRQ`, `IRQLEVEL`, `IRQPOL`, `IRQEN`, `MUXSEL`, `DOE`) are from device register reads. Two values here are accepted **interface facts** (see CLEANROOM.md Exceptions): the numeric `IOCTL_HAL_REQUEST_SYSINTR` function code 0x01010098 (function 38), and the OAL IRQ numbering rule 128 + bank * 32 + pin (254 on the PW-SH2, 272 on the PW-A7300). These came from Platform Builder's `pkfuncs.h` and NXP's `mx28_irq.h` via the measurement session (outside the clean room), and the user accepted them as non-protectable platform constants. Confirmed working on the PW-SH2 (SYSINTR 0x1E, `te: ready`, TE synced).

If TE stops, time out after two periods, write anyway, and count it. After five in a row, set the pin up again and put the panel through the recovery above. TE stopping until a cold boot was seen once on the PW-SH2.

## PXP (0x8002A000, IRQ 39)

The pixel pipeline reads a source plane, blends up to 8 overlays, and writes the result rotated and/or flipped, in 8x8 or 16x16 blocks. It is the natural way to move a dirty rectangle from the landscape primary into a portrait staging buffer.

Found on the devices (several differ from the reference manual):

- `OUTSIZE` is the size **before** rotation. The manual says after, and following it gives a doubled, line-interleaved image.
- Flips apply before rotation. `VFLIP` + `ROTATE` 90 is an exact transpose. `ROTATE` 90 alone mirrors panel columns.
- The source stride is `S0PARAM.WIDTH` (in blocks). There is no separate stride register. For a partial copy, point `S0BUF` at the rectangle's corner, keep the full screen width in `S0PARAM`, and `CROP` to the rectangle.
- It writes its whole output frame, filling cropped-out blocks with the background colour. It cannot update part of a larger buffer in place, so each rectangle goes to its own staging buffer.
- RGB sources cannot be scaled ("only YUV/YCbCr images may be scaled").
- Overlays: RGB565 with a colour key, ARGB1555, or ARGB8888 with alpha. Positions are in blocks relative to the output. Where overlays overlap, only the frontmost (lowest number) counts.
- Timing (PW-SH2): an 800x480 copy takes 6.9 ms; a 640x400 overlay adds 2.2 ms.

Confirmed on the PW-SH2 (device observation): the dirty-rectangle transpose with `ROTATE` 90, no `VFLIP`, `OUTSIZE` set to the before-rotation size, `S0BUF` at the rectangle corner, full screen width in `S0PARAM`, and `S0CROP` to the rectangle produces a correct picture (full and partial rectangles, no PXP timeouts). The PW-A7300 (no column mirror) needs `VFLIP` + `ROTATE` 90 for the exact transpose; untested on device.

Rectangles must be widened to 8-pixel multiples, with an even left edge.

## Memory and Cache

- The data cache does not allocate on writes. Stores to uncached lines go straight to DRAM through the write buffer, one bus transfer per store, so use wide stores.
- DIB sections are mapped into apps uncached, so reading them is about 4x slower than heap memory (57 ms against 13.5 ms for a 1.5 MB pass on the PW-SH2). The rasteriser reads them through the kernel mapping it is given. Check whether that mapping is cached.
- Physically contiguous buffers (primary, staging, present): `AllocPhysMem`. If the primary is cached, clean the dirty range (`CacheRangeFlush`) before the PXP or LCDIF reads it.

## CPU Speed

The board driver (`BRD1:`, pwsh2-boot `ce/drivers/board`) scales the CPU between 240 and 454 MHz and can hold it at 454 MHz. Present mode asks for that ([interfaces.md](interfaces.md)).
