# brain-display

A clean-room Windows CE 6.0 display driver for Sharp Brain PW-SH2 and PW-A7300 (i.MX28), built with [grimace](https://github.com/grimace-sdk/grimace). It replaces the `ddraw_mx28.dll` slot. Drawing goes through its own software rasteriser, tuned for the RGB565 primary surface, and frames go to the command-mode panel through the LCDIF and PXP, timed by the panel's tearing effect (TE) pulse.

Status: stages 1 and 2 working on the PW-A7300. The driver boots the device to a full, responsive desktop (correct colours, menus, borders, icons, filled shapes, gradients, transparency, pattern fills, a touchpad cursor) and is converged close to pixel-identical with the driver it replaces, checked phase by phase with the GDI probe. The host-testable rasteriser (all ROP3/ROP4, AnyBlt stretch/mirror/convert, TransparentBlt, GradientFill, StrokePath, FillPath, format conversions) passes randomised tests against a slow reference. Refresh is a CPU transpose of the dirty region to a staging buffer written over the LCDIF (no PXP or TE sync yet). Remaining: a few cosmetic probe diffs (exact colour-to-index rule, XO_TO_MONO masks, gradient dithering, dashed lines), and stages 3 to 7 (PXP and TE timing, rotation, power, PW-SH2 board, present mode, performance).

## Documents

| File | Contents |
| --- | --- |
| [CLEANROOM.md](CLEANROOM.md) | What the implementation may and may not be based on |
| [docs/ddi.md](docs/ddi.md) | The DDI surface GWES uses on these devices, measured, and what each call must do |
| [docs/rasteriser.md](docs/rasteriser.md) | Formats, ROPs, clipping, lines, paths, text and the fast paths that matter |
| [docs/hardware.md](docs/hardware.md) | Panels, LCDIF, PXP, TE, per-board values |
| [docs/interfaces.md](docs/interfaces.md) | Escapes, registry and app ABIs the driver must keep |
| [docs/plan.md](docs/plan.md) | Stages, tests, performance targets, open questions |
| [measurements/](measurements/) | Raw DDI traces from both devices |
| [abi/brain_present.h](abi/brain_present.h) | Present mode ABI used by full-screen apps |

## Build

grimace's CMake toolchain, ARM, CE 6, platform `brain`, pinned to grimace v0.1.0 (`e977bdb`):

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$GRIMACE/cmake/toolchain.cmake" \
    -DGRIMACE_ARCH=arm -DGRIMACE_CE_VERSION=6 -DGRIMACE_PLATFORM=brain \
    -DGRIMACE_LLVM_ROOT="$GRIMACE_LLVM"
cmake --build build
```

Outputs: `build/brain-display-pwsh2.dll` and `build/brain-display-pwa7300.dll`, each exporting `DrvEnableDriver` and `HALInit`. The board is chosen at compile time (`BRAIN_BOARD_pwsh2`, `BRAIN_BOARD_pwa7300`).

## Host tests

The rasteriser compiles natively and is tested against a slow reference over random rectangles, formats, clips and ROPs:

```sh
cmake -S test -B build-test -G Ninja
cmake --build build-test
cd build-test && ctest --output-on-failure
```

## Device testing

`tools/bundle-pwsh2.sh` builds a PW-SH2 `boot.nkp` with the driver in the `ddraw_mx28.dll` slot, using pwsh2-boot's `make-boot-nkp.mjs`. Copy it to the card over ce-debug, keeping the existing `boot.nkp` as a rollback, and reboot.
