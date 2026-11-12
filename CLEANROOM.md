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

- The ce600 repository (built from NXP's BSP and Microsoft's GPE sources), including its `install/` tree.
- NXP's WinCE BSP or PDK sources in any form (`bsplcdif.cpp`, `ddlcdif*.cpp`, `lcdif.c`, `common_pxp.h`, and so on), and `nxp-docs/*DOCKIT*`.
- Microsoft's GPE, `ddgpe`, `emulrotate` or any Platform Builder source, and the WinCE-src mirrors on GitHub.
- Sharp's `ddraw_mx28.dll`, disassembled or otherwise.
- pwsh2-boot `ce/a7300-display` (a patch written against Sharp's driver).
- Research notes based on the above: pw-sh2 `docs/research/display.md`, pw-a7300 `docs/boot.md` and `docs/display-driver-plan.md`, and ce600's docs.

If something you need is only in one of these, write it down as a question in `docs/plan.md` instead. It can be measured on the device.

## Exceptions

Specific constants the user has accepted as non-protectable platform interface facts, despite their source, because re-deriving them cleanly was judged not worth the effort:

- `IOCTL_HAL_REQUEST_SYSINTR` = 0x01010098 (function 38) and the GPIO OAL IRQ numbering 128 + bank * 32 + pin (254 for the PW-SH2 TE pin, 272 for the PW-A7300 TE pin). Obtained from Platform Builder's `pkfuncs.h` and NXP's `mx28_irq.h` by the measurement session (outside the clean room). Confirmed working on the PW-SH2. See docs/plan.md M9.
- `CACHE_SYNC_WRITEBACK` = 0x004, the `CacheRangeFlush` flag used to clean the cached primary framebuffer's dirty range to DRAM before the PXP reads it. From Platform Builder's `pkfuncs.h`; the flag name is public (MSDN `CacheSync`/`CacheRangeFlush`) but its value is not. Accepted as a platform interface constant to enable the cached primary (a performance win for read-heavy cases).
