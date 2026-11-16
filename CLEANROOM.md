# Clean room

brain-display is a clean-room reimplementation: a Windows CE 6.0 display driver
for the Sharp Brain, written independently of the original driver and of any
vendor BSP. The behaviour it has to match was captured by black-box measurement
on the device and from public documentation; the implementation is written from
that spec and from public sources only. This document records what went into it,
and what deliberately did not.

## Written from

- Public documentation: Microsoft Learn / MSDN (the Windows CE and Windows NT
  display driver DDI, GDI, ROP codes), the NXP i.MX28 Reference Manual (register
  descriptions), the MIPI DCS specification, and the Ilitek ILI9805 datasheet.
- grimace and its headers (`winddi.h` and the rest), themselves written from
  Microsoft's public CE DDI reference and mingw-w64.
- General graphics knowledge and published algorithms (Bresenham, scanline
  polygon fill, and so on).
- Measurement on the device: running the driver and observing the DDI calls,
  register state and timing it has to reproduce.

## Not used

To keep the implementation clean, it was written without reference to:

- The original Sharp display driver, disassembled or otherwise.
- NXP's Windows CE BSP or PDK sources.
- Microsoft's GPE and Platform Builder display sources.
- A throwaway prototype we built earlier to prove the concept out. It drew on the
  NXP BSP and Microsoft GPE, so it is off-limits here, with one exception noted
  below.

Where something needed was only available in one of these, it was left as a
question to answer by measuring the device instead.

## Exceptions

A few platform interface constants were accepted despite their source, because
they are non-protectable interface facts rather than design, and re-deriving them
cleanly was not worth the effort:

- `IOCTL_HAL_REQUEST_SYSINTR` = 0x01010098 (function 38) and the GPIO OAL
  interrupt numbering 128 + bank * 32 + pin (254 for the PW-SH2 tearing-effect
  pin, 272 for the PW-A7300). Confirmed on the device. See docs/plan.md M9.
- `CACHE_SYNC_WRITEBACK` = 0x004, the `CacheRangeFlush` flag used to clean the
  framebuffer's dirty range to DRAM before the PXP reads it. The flag name is
  public (MSDN); its value is not.

## Prototype present mode and bootloader analysis

Two narrow exceptions to "not used":

- The prototype's present-mode code was our own addition, written independently
  of the original driver and the NXP BSP, and is used as the reference for present
  mode here. That is limited to present mode; the rest of the prototype stays
  off-limits.
- Static analysis of the device's bootloader (a distinct binary from the display
  driver) is used for the panel bring-up on each board: the LCD pin mux, control
  and power GPIO states, the panel reset sequence and timing, and the controller
  init sequence, for restoring the panel after deep sleep. These are the panel's
  own register values, a hardware fact. On the PW-SH2 the ILI9805 datasheet backs
  the same facts. The per-board facts are recorded in the panel-handoff documents
  (one per board). The PW-SH4 document (`pwsh4-panel-handoff.md`) tags each fact
  by source; the `pwsh4` board was written from its bootloader-analysis, public
  datasheet and device-measurement facts only. That document also records facts
  taken from Sharp's display driver and backlight driver, which, like those
  drivers themselves, were not used.

## Registers

Hardware register offsets and fields come from the public i.MX28 Reference Manual,
with device measurement where the manual and the silicon disagree (noted in
docs/hardware.md):

- LCDIF (0x80030000), PXP (0x8002A000), PINCTRL (0x80018000), CLKCTRL
  (0x80040000), and DIGCTL (0x8001C000, the `HW_DIGCTL_MICROSECONDS` free-running
  microsecond counter at 0xC0, used as the timebase in place of
  `QueryPerformanceCounter`).
- The interrupt source numbers (LCDIF 38, PXP 39) are from the public Linux
  `imx28.dtsi` device tree.
- The present-mode PXP overlay registers were read from the device at runtime and
  decoded against the Reference Manual.
