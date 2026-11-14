# Clean Room

The driver must be independently written. The spec in `docs/` was written by a separate session from black-box measurement and public documents; the implementation is written from the spec and public sources only.

## Allowed

- This repository: `docs/`, `measurements/`, `abi/`.
- grimace: headers (`winddi.h` and the rest), libraries, tests, docs. Its `winddi.h` was written from Microsoft's public CE 5.0 DDI reference and mingw-w64.
- Public documentation: Microsoft Learn / MSDN (Windows CE and Windows NT display driver DDI, GDI, ROP codes), the NXP i.MX28 Reference Manual (register descriptions), MIPI DCS, the Ilitek ILI9805 datasheet.
- General graphics knowledge and published algorithms (Bresenham, scanline polygon fill, and so on).
- Running things on the devices and measuring them, including through `ddi-trace.dll` (pwsh2-boot `ce/drivers/ddi-trace`).
- Our own tooling: pwsh2-boot's `make-boot-nkp.mjs`, ce-debug, pw-sh2's bundle scripts.

## Not allowed

Do not open, search or copy from:

- A prior prototype of this driver (built from NXP's BSP and Microsoft's GPE sources), including its `install/` tree.
- NXP's WinCE BSP or PDK sources in any form (`bsplcdif.cpp`, `ddlcdif*.cpp`, `lcdif.c`, `common_pxp.h`, and so on), and `nxp-docs/*DOCKIT*`.
- Microsoft's GPE, `ddgpe`, `emulrotate` or any Platform Builder source, and the WinCE-src mirrors on GitHub.
- Sharp's `ddraw_mx28.dll`, disassembled or otherwise.
- pwsh2-boot `ce/a7300-display` (a patch written against Sharp's driver).
- Research notes based on the above: pw-sh2 `docs/research/display.md`, pw-a7300 `docs/boot.md` and `docs/display-driver-plan.md`, and the prototype's docs.

If something you need is only in one of these, write it down as a question in `docs/plan.md` instead. It can be measured on the device.

## Exceptions

Specific constants the user has accepted as non-protectable platform interface facts, despite their source, because re-deriving them cleanly was judged not worth the effort:

- `IOCTL_HAL_REQUEST_SYSINTR` = 0x01010098 (function 38) and the GPIO OAL IRQ numbering 128 + bank * 32 + pin (254 for the PW-SH2 TE pin, 272 for the PW-A7300 TE pin). Obtained from Platform Builder's `pkfuncs.h` and NXP's `mx28_irq.h` by the measurement session (outside the clean room). Confirmed working on the PW-SH2. See docs/plan.md M9.
- `CACHE_SYNC_WRITEBACK` = 0x004, the `CacheRangeFlush` flag used to clean the cached primary framebuffer's dirty range to DRAM before the PXP reads it. From Platform Builder's `pkfuncs.h`; the flag name is public (MSDN `CacheSync`/`CacheRangeFlush`) but its value is not. Accepted as a platform interface constant to enable the cached primary (a performance win for read-heavy cases).

## Authorised sources

Present mode only. On 2026-11-12 the user stated that the prior prototype's present-mode implementation was the project's own addition, written independently of Sharp's driver and NXP's BSP, and authorised it as a reference for implementing present mode here:

- the prototype's `panel_present.cpp`
- its `brain_present.h` (matches our `abi/brain_present.h`)
- the present-mode parts of its `panel_refresh.cpp`
- the present-mode section of its board documentation

Scope is limited to the present-mode code. The rest of the prototype's display driver was built alongside Sharp's driver and NXP BSP material and stays off-limits: do not take its PXP, LCDIF, panel setup, recovery or rotation code beyond what has already been established on the device.

Static analysis of Sharp's EBOOT (the bootloader), as written up in `pw-sh2/docs/research/panel-handoff.md`, is authorised as a source for the PW-SH2 panel bring-up: the LCD pin mux and control/power GPIO states, the reset pulse and its timings, and the ILI9805 controller init list, used for the deep-sleep panel re-init. The user accepted EBOOT's observed state plus the ILI9805 datasheet for this. EBOOT is a distinct binary from Sharp's display driver `ddraw_mx28.dll`, which stays off-limits.

## Registers

Hardware register offsets and fields are from the public i.MX28 Reference Manual, with device measurement where the manual and the silicon disagree (noted in `docs/hardware.md`):

- LCDIF (0x80030000), PXP (0x8002A000), PINCTRL (0x80018000), CLKCTRL (0x80040000), and DIGCTL (0x8001C000, the `HW_DIGCTL_MICROSECONDS` free-running microsecond counter at 0xC0, used as the timebase in place of `QueryPerformanceCounter`).
- ICOLL interrupt source numbers (LCDIF 38, PXP 39) are from the public Linux `imx28.dtsi` device tree.
- The present-mode PXP overlay registers (`HW_PXP_OLn` at 0x200 + n*0x40, and the overlay colour-key registers) were read on the device from the prototype's present mode through a read-only PXP register dump and decoded against the RM. This is device measurement of runtime register state, the same category as the CLKCTRL reads, not use of off-limits source.
