# Interfaces

What apps, Control Panel applets and other components already rely on. Everything here is our own ABI or standard CE.

## Escapes (DrvEscape)

`DrvEscape(pso, iEsc, cjIn, pvIn, cjOut, pvOut)`. Return -1 for anything unsupported. `QUERYESCSUPPORT` (8): `pvIn` points to an escape code; return 1 if the driver handles it.

| Code | Name | Seen | Required behaviour |
| --- | --- | --- | --- |
| 6150 | (unnamed) | A7300, around rotation | Return -1 |
| 6151 | (unnamed) | Thousands of times on both | Return -1, as cheaply as possible |
| 6301 | `DRVESC_SETSCREENROTATION` | Rotation | `cjIn` is the new angle as a DMDO value: 0, 1 (90), 2 (180), 4 (270). Note that CE's `DMDO_270` is 4; one `wingdi.h` copy says 3. Return 0 (`DISP_CHANGE_SUCCESSFUL`) |
| 6302 | `DRVESC_GETSCREENROTATION` | Boot, rotation | `cjOut` 4. Return 0 and (per CE's docs) write the current angle and the supported set to `pvOut`. Exact format: plan.md M4 |
| 6303, 6304 | (unnamed) | After each rotation | Return -1 |
| 100150 | Sharp's | Boot and often on the A7300 | Return -1 |
| 100151 | Sharp's | Before suspend and after resume | Return -1 |
| 100152 | `DISPLAY_SET_PANEL_MODE` | Not seen | A7300: DWORD mode from Sharp's stack. Optional |
| 100153 | `DISPLAY_PANEL_COMMAND` | Tools | Raw panel command: command byte then parameters. Only when the registry enables it |
| 100154 | `DISPLAY_PANEL_READ` | Tools | Command, word count; output words then status. Only when the registry enables it |
| 100155 | `DISPLAY_NIGHT_LIGHT_PREVIEW` | Night Light applet | Two DWORDs: active, strength (10 to 100) |
| 100158 to 100163 | Present mode | full-screen apps | See below |
| 0x44444900 | Trace phase marker | ddi-probe | Not for the driver. `ddi-trace.dll` uses it |

Rotation sequence (both devices): 6301 with the angle, 6302, `DrvEnablePDEV` with the new size, 6303, 6304, then GWES redraws everything. The angle also goes to `HKLM\Software\Brain\Display` `Orientation` (DMDO value), which the rotate applet and `autorotate` read. The PW-SH2 rotates at all four angles. The A7300 has no rotation in its UI, but the probe rotated it fine.

## Present Mode

Full-screen apps draw into their own RGB565 buffers and have them written to the panel on TE, while GDI keeps its normal mode. The ABI is [../abi/brain_present.h](../abi/brain_present.h). Keep it byte for byte. Behaviour apps depend on:

1. `BRAIN_PRESENT_OPEN` (in `BrainPresentOpen`, out `BrainPresentInfo`, 92 bytes): allocates one or two physically contiguous buffers (width and height multiples of 8, at most the screen size), maps them **cached** into the caller, and holds GDI's panel updates. One process at a time. `BRAIN_PRESENT_FAST_CPU` holds the CPU at 454 MHz (board driver) until present mode ends. `Scale` must be 1.
2. `BRAIN_PRESENT` (in a DWORD buffer index, or a 36-byte `BrainPresentFrame`): cleans the buffer from the cache in the caller's context and queues it. The newest frame is written on the next TE pulse. A newer frame replaces one still waiting (counted as `Replaced`).
3. `BRAIN_PRESENT_WAIT` (in a DWORD index): blocks until the driver no longer reads that buffer (about 7 ms after its PRESENT at 800x480).
4. `BRAIN_PRESENT_CLOSE`, or the owner process exiting (checked every 250 ms), ends present mode, restores the CPU setting, and redraws the whole screen from the primary.
5. Overlays (up to 8, each with one or two buffers): blended over each frame by the PXP (RGB565 opaque or colour-keyed, ARGB1555 with the top bit as alpha). Positions in 8-pixel blocks relative to the app's frame, not scaled. Lower numbers are in front, and where overlays overlap only the front one counts: its transparent pixels show the frame, not the overlays behind it (a colour-keyed overlay 0 hides overlays 1 to 7 under its transparent area). `BRAIN_PRESENT_WAIT_OVERLAY` returns once a buffer is no longer shown or being read.
6. `BRAIN_PRESENT_STATS`: counters in `BrainPresentStats`.

The first frame after OPEN, a rotation or a resume clears the panel to `Background`. Frames smaller than the screen are centred.

Measured with the current driver on the PW-SH2 at 800x480 (to match or beat): WAIT plus PRESENT cost the app 0.23 ms per frame; the PXP copy is 6.9 ms of wall time; present to transfer start is about 15 ms; the transfer about 16 ms. A screenshot through GDI while an app is presenting times out. The debug agent's screenshot needs another route (open question).

## Night Light (PW-A7300)

`HKLM\Software\Brain\NightLight`: `Enabled`, `Start` and `End` (minutes after midnight), `Transition` (minutes), `Strength` (10 to 100), and optional `WarmGreen` and `WarmBlue` (gains at full warmth, defaults 0x18 and 0x00). Re-read the key when the applet sends escape 100155, and on a timer for the schedule. The warmth is applied through the panel's own gamma registers, found on the device by probing: the gain is byte 1 of `E2`/`E3` (green) and `E4`/`E5` (blue), each pair the two drive polarities. Change both of a pair together, or the dot inversion flickers. A 6-bit value: red is already at its maximum (0x3F), and warming pulls green and blue down.

The PW-SH2's ILI9805 has a public datasheet. Night light there is a later stage.

## Registry Settings

All under `HKLM\Software\Brain\Display`, all DWORD. The refresh thread re-reads them once a second, so "live" settings take effect without a reboot.

| Name | Default | Effect | When |
| `Cadence` | 1 | 1 picks N per pending send length (every TE for partial updates, every second TE for full frames); 0 forces every-TE sends. | Live |
| `TeOffsetMs` | 2 | Milliseconds to wait after the TE edge before starting the send, to keep the send clear of the panel scan. | Live |
| `TeOffsetUs` | 0 | Non-zero overrides `TeOffsetMs` with a QPC-spun sub-millisecond offset. Test knob for the tear sweep; leave 0 in production. | Live |
| `Settle` | 4000 | Microseconds to hold a new dirty region (from its first mark) before pushing, so a multi-step draw such as a popup menu item's fill, text and icon coalesces into one frame instead of showing half-drawn when a TE edge lands mid-draw. Bounded by the first mark, so continuous drawing is not held back. 0 disables. | Live |
| `Snapshot` | 1 | 1 holds a draw lock across the flush and PXP so a frame is tear-free; 0 drops the lock for raw throughput. | Live |
| `Log` | 0 | 1 emits per-second refresh stats and enables `OutputDebugString`; 0 is silent apart from one-shot init and anomaly lines, which are still batched to the SD log. Debug output blocks on the debug UART, so keep it 0 for timing. | Live |
| `PxpFlip` | absent | Test knob for tuning the per-angle PXP rotate/flip on a board. 0-3 forces the flip bits for every angle (bit 0 HFLIP, bit 1 VFLIP); absent or above 3 uses the built-in per-angle mapping. Leave unset in production. | Live |

The driver also writes `Orientation` (the DMDO rotation value, read by the rotate applet and `autorotate`) and counters for stuck transfers and TE timeouts under the same key. `PanelEscapes` (DWORD, non-zero) enables escapes 100153 and 100154.

## Load and Swap

GWES loads the DLL named by `HKLM\System\GDI\Drivers` `Display` (`ddraw_mx28.dll`). brainsurgeon's `tools/make-boot-nkp.mjs --replace-module ddraw_mx28.dll=brain-display.dll` puts ours in that slot and renames the ROM's copy to `ddraw_mx2_.dll`. The bootloader loads the module from `modules/` beside `boot.nkp` on the card, so rolling back means restoring the previous `boot.nkp` and `modules/` files.
