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

- S0 = the GDI primary framebuffer (the desktop).
- The app's frame = one overlay, placed at the window rect.
- Output = desktop with the app frame laid over it.

Do not re-compose the whole frame every TE. Like the GDI path, compose only the
changed region: `(window rect) union (GDI dirty rects)`. S0 is cropped to that
region and the overlay is clipped to it. Cost then scales with window size - a
320x240 window is about a fifth of a frame, not ~8.8 ms per TE - and the
snapshot-lock stall shrinks to the region read (see open question 3). A full-frame
pass is needed only for the first frame and after rotation or a window move.

## ABI

- New OPEN flag `BRAIN_PRESENT_OVER_DESKTOP`.
- Window position: `Left`/`Top` in `BrainPresentOpen`, screen space, 8-pixel block
  aligned; the app frame is placed there, not centred. `Width`/`Height` stay the
  app frame size.
- Window moves without re-opening: the app updates `Left`/`Top` through a
  `BrainPresentFrame` field per present. The driver adds the old rect to the dirty
  region so the desktop under the vacated position is restored.
- Overlays: OPEN with `BRAIN_PRESENT_OVER_DESKTOP` and `Overlays > 0` fails,
  rather than silently ignoring them (see "Layering limit"). The app composes any
  sub-layers into its own frame and presents the finished frame.

## Open questions

### 1. Occlusion by windows above the app

The PXP overlay is always in front of S0, so a naive composite draws the app
frame on top of everything, even windows stacked above it. Correct occlusion
needs the app's visible region, which GWES owns, not the driver.

- Phase 1, foreground-only: the app signals whether it is unoccluded (it sees
  activation and z-order changes). When unoccluded, composite the frame; when it
  flips to occluded, add the window rect to the dirty region so the desktop
  underneath is redrawn that TE (otherwise the last app frame stays stale in
  staging), and stop compositing until it is unoccluded again. Correct for the
  common case of one active app.
- Phase 2, clip-rect list: the app submits its visible sub-rects and the driver
  overlays only those. Accurate partial occlusion, more work on both sides, and
  the 8-pixel overlay grid limits the rect granularity.

Recommendation: phase 1 first. Document that without occlusion info the frame
renders on top.

### 2. Rotation

Two parts:
- Window rect: it is screen space; the PXP composite maps screen to panel by
  angle. Transform the rect with the same screen-to-panel mapping the primary
  uses (`brain_compose_window` + `g_angle`), then align to 8-pixel blocks.
- Rotation while open: the driver ends over-desktop present on a rotation and the
  app re-opens, matching the full-screen rule that there is no rotation while
  presenting. Simpler than transforming live state across the re-entrant
  EnablePDEV.

### 3. Snapshot-lock interaction with GDI draws

The per-TE PXP reads the primary (S0) while GWES draws to it. For a consistent
frame, take the snapshot lock around the S0 read, the same contract as tear-free
GDI refresh (the `Snapshot` setting). Because the read is cropped to the compose
region (window union dirty), the stall is bounded by the region size, not a full
frame - which answers most of the concern. The app overlay is always consistent
(it is the app's own buffer).

## Layering limit (PXP overlap rule)

The PXP's rule is that where overlays overlap, only the front one counts, and its
transparent pixels show S0, not the overlays behind. With S0 = desktop, a
transparent pixel in a front app-overlay would show the desktop, not the app
frame behind it. So the app frame and the app's own overlays cannot be stacked as
separate PXP overlays over a desktop S0 and composite correctly. Hence the app
presents one finished frame in this mode (its own sub-layers baked in), and OPEN
rejects `Overlays > 0`.

## Phasing

1. Region compose over desktop, per-frame window position, foreground-only
   occlusion, rotation ends present, snapshot-locked region read. Full-frame pass
   only on first frame / rotation / window move.
2. Clip-rect occlusion for partial coverage, if the single-app case proves too
   limiting.

## Cost summary

Per composited TE: one PXP pass over the compose region (window union GDI dirty)
plus the LCDIF send, so cost scales with window size rather than a fixed
full-frame ~8.8 ms. Static desktop plus a moving window stays cheap; the
full-frame pass is reserved for first frame, rotation and window moves.
