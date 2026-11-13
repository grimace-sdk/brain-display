# Present Mode

Present mode lets a full-screen app draw into its own RGB565 buffers and have
them written straight to the panel on the tearing-effect (TE) pulse, bypassing
GDI. GDI keeps its normal mode; its updates are held while an app presents and
the whole screen is redrawn when the app closes or exits.

The ABI is [../abi/brain_present.h](../abi/brain_present.h); keep it byte for
byte. All calls go through `ExtEscape` on a screen DC. A worked example is in
[../examples/present_minimal.c](../examples/present_minimal.c).

## Lifecycle

1. **Open.** `ExtEscape(dc, BRAIN_PRESENT_OPEN, sizeof(BrainPresentOpen), &open,
   sizeof(BrainPresentInfo), &info)`. Fill `BrainPresentOpen` with the frame
   `Width` and `Height` (multiples of 8, at most the screen size), `Scale` 1,
   `Buffers` 1 or 2, a `Background` colour (RGB565, fills the border of a frame
   smaller than the screen), and `Flags`. On success the driver allocates the
   buffers, maps them cached into your process, and returns their pointers in
   `info.pBuffers[]` with the row `Stride`. One process may present at a time.

2. **Draw and present.** Draw a frame into `info.pBuffers[i]`, then
   `ExtEscape(dc, BRAIN_PRESENT, sizeof(DWORD), &i, sizeof(DWORD), &replaced)`.
   The newest presented frame is written on the next TE; a newer frame replaces
   one still waiting (returned in `replaced`, and counted as `Replaced` in the
   stats). With two buffers, draw into the one you are not presenting.

3. **Wait.** `ExtEscape(dc, BRAIN_PRESENT_WAIT, sizeof(DWORD), &i, 0, NULL)`
   blocks until the driver no longer reads buffer `i`, so it is safe to draw
   into again.

4. **Close.** `ExtEscape(dc, BRAIN_PRESENT_CLOSE, 0, NULL, 0, NULL)`, or just
   exit; the driver checks the owner every 250 ms and ends present mode when it
   is gone, restoring the CPU setting and redrawing the screen.

A typical loop with two buffers: present buffer A, draw buffer B, wait B,
present B, draw A, wait A, repeat.

## Flags

- `BRAIN_PRESENT_FAST_CPU` holds the CPU at 454 MHz (through the board driver)
  until present mode ends.

## Overlays

Up to 8 overlays can be blended over each frame by the PXP, so an app can keep a
HUD out of its frames. Declare them in `BrainPresentOpen.Overlay[]` (position
and size in 8-pixel blocks relative to the frame, `Format` RGB565 or ARGB1555,
1 or 2 `Buffers` each) and `Overlays`. `OPEN` returns their buffers in
`info.pOverlayBuffers[overlay][buffer]`. To change what an overlay shows, pass a
`BrainPresentFrame` to `BRAIN_PRESENT` (instead of a bare buffer index) with
`Overlay[n]` set to a buffer index, `BRAIN_PRESENT_KEEP`, or `BRAIN_PRESENT_HIDE`.

RGB565 overlays are opaque, or transparent where they match `ColourKey`
(RGB565, shared by all overlays) when the overlay has
`BRAIN_PRESENT_OVERLAY_COLOUR_KEY`; ARGB1555 overlays are transparent where the
top bit is clear. Lower overlay numbers are in front, and where overlays
overlap only the front one counts: its transparent pixels show the frame, not
the overlays behind it. `BRAIN_PRESENT_WAIT_OVERLAY` waits until an overlay
buffer is no longer shown or being read.

## Stats

`BRAIN_PRESENT_STATS` fills a `BrainPresentStats` with frame, replace, drop and
timing counters; pass a non-zero DWORD in to reset them.
