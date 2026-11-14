# Present over desktop (design proposal, post-alpha)

Status: proposal for review, not implemented. Pairs with M7 (both treat the
desktop and the present frame as layers). Target consumer: windowed SDL apps.

## Idea

Today present mode is full-screen: the app's buffer replaces the desktop and GDI
updates are held. "Over desktop" keeps GDI running and composites the app's frame
at a window rect on top of the live desktop, refreshed on TE.

## Mapping to the existing pipeline

The PXP pass already does S0 (base) plus up to eight overlays into the staging
buffer, then the LCDIF sends staging to the panel. Full-screen present uses the
app buffer as S0. Over-desktop reuses the same pass with a different layering:

- S0 = the GDI primary framebuffer (the whole desktop), transposed as in normal
  GDI refresh.
- The app's frame = one overlay, placed at the window rect.
- Output = desktop with the app frame laid over it.

So the per-TE work is one full-frame PXP pass with S0 = primary and one overlay,
about 8.8 ms on the SH2 (measured for a full-frame pass with overlays).

## ABI

- New OPEN flag `BRAIN_PRESENT_OVER_DESKTOP`.
- Window position: `Left`/`Top` in `BrainPresentOpen`, screen space, 8-pixel block
  aligned (the app frame is placed there, not centred). `Width`/`Height` stay the
  app frame size.
- The app frame takes the overlay slot, so in this mode the app does not get the
  eight PXP overlays (see "Layering limit"). An app that wants sub-layers composes
  them into its own frame buffer and presents the finished frame.

## Open questions

### 1. Occlusion by windows above the app

The PXP overlay is always in front of S0, so a naive composite draws the app
frame on top of everything, even windows stacked above it. Correct occlusion
needs the app's visible region, which GWES owns, not the driver.

- Phase 1, foreground-only: the app tells the driver whether it is unoccluded
  (it sees activation and z-order changes). When unoccluded, composite the frame;
  when occluded, stop compositing and show the plain desktop that TE. Correct for
  the common case (one active app), and the app simply vanishes to desktop when
  covered. Signalled by a per-frame flag or a small escape.
- Phase 2, clip-rect list: the app submits its visible sub-rects and the driver
  overlays only those. Accurate partial occlusion, more work on both sides, and
  the 8-pixel overlay grid limits the rect granularity.

Recommendation: phase 1 first. Document that without occlusion info the frame
renders on top.

### 2. Rotation

The window rect is screen space; the PXP composite maps screen to panel by angle.
Transform the rect with the same screen-to-panel mapping the primary uses
(`brain_compose_window` + `g_angle`), then align the result to 8-pixel blocks
(round or pad). Expect up to a 7-pixel placement quantisation from the grid.

### 3. Snapshot-lock interaction with GDI draws

The per-TE PXP reads the primary (S0) while GWES draws to it. For a consistent
frame, take the snapshot lock around the S0 read, the same contract as tear-free
GDI refresh (the `Snapshot` setting). The cost is that GDI draws stall for the
read window (up to ~8.8 ms per composited TE), heavier than normal GDI refresh
because the whole frame is re-read rather than a dirty region. The app overlay is
always consistent (it is the app's own buffer).

Mitigation: only re-composite when the primary is dirty or the app presented a
new frame; when neither changed, resend the last staging buffer and skip the PXP.
With a static desktop and a 60 fps app, that is still a full-frame pass per app
frame.

## Layering limit (PXP overlap rule)

The PXP's rule is that where overlays overlap, only the front one counts, and its
transparent pixels show S0, not the overlays behind. With S0 = desktop, a
transparent pixel in a front app-overlay would show the desktop, not the app
frame behind it. So the app frame and the app's own overlays cannot be stacked as
separate PXP overlays over a desktop S0 and composite correctly. Hence the app
presents one finished frame in this mode (its own sub-layers baked in), and the
driver overlays that single frame over the desktop.

## Phasing

1. Full frame over desktop, foreground-only occlusion, no app-side overlays,
   rotation handled, snapshot-locked S0 read, re-composite only on change.
2. Clip-rect occlusion for partial coverage, if the single-app case proves too
   limiting.

## Cost summary

Per composited TE: one full-frame PXP pass (~8.8 ms SH2) plus the LCDIF send.
Feasible to about 60 fps; heavier than dirty-region GDI refresh. The
re-composite-on-change rule keeps a static desktop cheap.
