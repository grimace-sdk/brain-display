# brain-display

A clean-room Windows CE 6.0 display driver for Sharp Brain PW-SH2 and PW-A7300 (i.MX28), built with [grimace](https://github.com/grimace-sdk/grimace). It replaces the `ddraw_mx28.dll` slot. Drawing goes through its own software rasteriser, tuned for the RGB565 primary surface, and frames go to the command-mode panel through the LCDIF and PXP, timed by the panel's tearing effect (TE) pulse.

Status: alpha. The driver boots both boards to a full, responsive desktop (correct colours, menus, borders, icons, filled shapes, gradients, transparency, pattern fills, a touchpad cursor) and runs the whole pipeline:

- A host-tested software rasteriser (all ROP3/ROP4, AnyBlt stretch/mirror/convert, TransparentBlt, GradientFill, StrokePath, FillPath, format conversions), checked against a slow reference over random rectangles, formats, clips and ROPs.
- A PXP + TE refresh: the dirty region is transposed by the PXP into a staging buffer and written over the LCDIF on the tearing-effect pulse, driven by a fixed-N cadence scheduler on a dedicated TE interrupt thread. Tear-free, and faster than the stock driver on every benchmark case.
- Four-angle rotation, suspend/resume, and a touchpad cursor composited at refresh time.
- Present mode for full-screen apps: RGB565 buffers presented on TE, with up to 8 PXP overlays (colour-keyed RGB565 or ARGB1555), centred sub-screen buffers, and the 454 MHz CPU hold.

The primary target is the PW-SH2. The PW-A7300 shares the code and builds; its device validation is ongoing.

## Known limitations

- `imagelist` colour-to-mono masks and the polygon fill rule have minor differences from the replaced driver (open).
- Anti-aliased text and some edges differ from Sharp by a least-significant bit, by design (the driver keeps its own rounding).
- Night light is PW-A7300 only; the PW-SH2's ILI9805 night light is a later stage.
- Present mode serves one process at a time and does not rotate while an app presents.

## Performance

Measured on the PW-SH2 at 800x480, 454 MHz, against the stock driver it replaces (`drawbench`, microseconds per operation, lower is better). All 22 drawbench cases are faster than the stock driver; a representative selection:

| Case | Stock | brain-display | Speedup |
| --- | --- | --- | --- |
| Full-screen solid fill | 11504 | 3943 | 2.9x |
| Full-screen 16 bpp copy | 11241 | 9461 | 1.2x |
| 28 lines of ClearType text | 81757 | 53524 | 1.5x |
| StretchBlt 64 to 256 px | 46162 | 2608 | 17.7x |
| MaskBlt sprite | 3769 | 126 | 29.9x |
| Open and close a listbox | 62720 | 44923 | 1.4x |

Ten of the fill, blit, caret and window cases are pixel-identical to the stock driver; text and stretch paths that read 555-format sources differ by a least-significant bit, by design. Refresh is tear-free.

Present mode is a new capability (the stock driver has none): a full-screen 800x480 app presents at 51.7 fps, a 400x240 centred buffer at the full 59.3 Hz, and a software-rendered 3D racing game runs at 40 fps with 6.4 ms from present to panel.

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

grimace's CMake toolchain, ARM, CE 6, platform `brain`, pinned to grimace `13e9240` (present mode needs its CE6 cross-process and physical-memory calls):

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

## Present mode

Full-screen apps can present their own RGB565 frames straight to the panel. See [docs/present.md](docs/present.md) for the guide and [examples/present_minimal.c](examples/present_minimal.c) for a worked example.

## License

MIT. See [LICENSE](LICENSE).
