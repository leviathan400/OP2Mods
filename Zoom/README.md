# Zoom (op2zoom)

An **op2ext addon** that adds a **new, wider main-view zoom level** for Outpost 2 - wider than the
engine's current widest - so the map viewport shows **more map**. Render/UI-only, so it is
**multiplayer-desync-safe**. Live-tested on retail 1.3.6 (addresses also valid for 1.4.1 - same
vanilla `Outpost2.exe`).

## Why it isn't a one-line clamp change

The engine's zoom is an integer **divisor**: `AllocSurface @ 0x004A9170` builds the map back-buffer at
`(clientW / zoom) x (clientH / zoom)`, and the present `@ 0x004A9410` blits it scaled **up** by `zoom`
(`StretchDIBits`, or `BitBlt` 1:1 when `zoom == 1`). So `zoom = 1` is already the widest the divisor
can express. Seeing **more** needs an effective **sub-1** scale: a back-buffer **larger** than the
window, **shrunk** on present, with the screen<->world math scaled to match.

## How it works

Engine `zoom` stays `1`; the wide level is gated on `[Game] WideZoom` and engages when you zoom all
the way out to level 1. Five in-memory, reversible hooks and patches (no `Outpost2.exe` edit):

1. **`GetClientRect` (IAT)** - returns `client x scale` **only for the map callers**, scoped by
   caller return address: the back-buffer alloc `FUN_0049d030` (only when the surface is large, so the
   *shared* minimap/command-pane allocs are left alone) + the view transforms `FUN_00407990` /
   `FUN_00408210` / `FUN_004082f0` / `FUN_00408370` / `FUN_00408540`. The panel-layout caller
   `FUN_004999a0` (reads the **main** window and right-aligns the sidebar) stays truthful, so the
   **side panels keep their place**. The map subsystem reads a different window (~800x608 detail) than
   the layout (full game window).
2. **`BitBlt` (IAT)** - when the source bitmap is **our over-sized detail buffer** (the exact size the
   alloc hook handed out, +/-32 px) and wider than its destination window, the mod shrinks that dirty
   rect itself: it resamples straight from the buffer's DIB bits into its own window-sized DIB, then
   presents with a plain 1:1 `BitBlt` (the vanilla zoom-1 fast path). One **global** source mapping is
   used for every rect, so a redrawn tile always matches its neighbours (GDI `StretchBlt` per rect
   shimmered and cost 25-33% of each second). With `ZoomSmooth=1` (default) each screen pixel is the
   **average** of the block it covers, so 1 px lines stay continuous; `ZoomSmooth=0` copies the nearest
   pixel (sharper, cheaper, thin lines break up). Any other bitmap is passed through untouched; a
   non-DIB or unexpected format falls back to `StretchBlt` (logged once).
3. **`ScrollWindowEx` (IAT)** - `ScrollScreen` (`0x004A9890`) shifts the on-screen picture by the
   scroll distance in **buffer** pixels, ~2x too far on the shrunk view. At the wide level the mod
   skips that shift and repaints the whole view from the (already scrolled) buffer instead.
4. **Mouse: `IWnd::Pane::Detail::GetAbsolutePos` (vtable slot 10)** - the map mouse position
   (`0x0057B7B4`) is client pixel -> map pixel through `GetAbsolutePos` (`0x00408070`). The mod
   replaces the pane vtable's slot 10 (`0x004CF5E8`, the function's only reference; verified before
   patching) with a wrapper that scales the client pixel by the scale while wide-active. Both callers
   go through it: `Detail::CallFilters` (`0x00407A50`, each mouse message's `lParam`) **and**
   `Dans_RULE_UIFrame::OnIdle` (`0x0049C310`), which re-reads the cursor with `GetCursorPos` **every
   frame**. Scaling `lParam` in the window procedure (the old approach) missed the `OnIdle` path, so
   the map position was overwritten unscaled each frame and a selection box's moving corner sat at
   cursor / scale (Test 10). Mouse messages themselves are passed through unchanged.
5. **Detail-window subclass** - a 250 ms timer that repaints the whole view from the buffer once a
   second (a safety net for stale screen pixels), and the `ZoomStats` click diagnostics.

**Scale limit (crash fix).** `Viewport::SetSurface` (`0x0046F160`) sizes five per-tile redraw bit
vectors from the surface size, but each is a fixed `bitVectorBuf[400]` inside the `Viewport`. A
bigger surface overruns them into the pointers after them and crashes at `0x0046F26C` (e.g. a
~990x700 view at exactly 2x needs 414 bytes). The mod uses the engine's own formula to pick the
largest scale up to `ZoomWiden`, in 1/16 steps, that fits - e.g. **x31/16 (1.94x)** for a 992x704
view - and logs `wide buffer: WxH -> W'xH' (x../16, reduced to fit ...)`. If even a small widen does
not fit, that size stays vanilla.

At load it **verifies** the present / `AllocSurface` prologue bytes against the retail reference and
logs a `MATCH`/`DIFFER` line, plus the live `zoom` global. If **either** differs it installs **no
hooks** and the game runs with vanilla zoom - a wrong build is detected, never corrupted. The
`BitBlt` hook goes in first and `GetClientRect` only after it, so the over-sized buffer is never
built without the shrink that undoes it. It also runs a read-only zoom-watcher that logs each zoom
change.

**Game folder.** Settings are read from, and the log written to, the game's runtime folder: the
current directory when it holds an `outpost2.ini` (under OPU, OPULauncher starts the game there), else
the folder of `Outpost2.exe` (retail). Log: `<game folder>\logs\op2zoom.log`, with timestamps.

## Install

**OPU 1.4.1** (op2ext loads modules from `[ExternalModules]`): copy `op2zoom.dll` into the OPU
folder (next to `OPUPatch.dll`) and add to the OPU folder's `outpost2.ini` (the live one - not the
`outpost2.ini` next to the GOG `Outpost2.exe`):

```ini
[ExternalModules]
Zoom = yes

[Zoom]
Dll = op2zoom.dll

[Game]
WideZoom   = 1               ; 1 = on (default), 0 = off
ZoomWiden  = 2               ; max scale 2..8; 2 = ~2x more map (reduced automatically to fit, see above)
ZoomSmooth = 1               ; 1 = averaged shrink (default), 0 = nearest pixel
ZoomBlit32 = 1               ; 1 = 32bpp screen copy (default, faster), 0 = 16bpp copy
ZoomStats  = 0               ; 1 = log present statistics every 5 s (diagnostics)
```

**Retail 1.3.6 with a newer op2ext** (`[Game] LoadAddons`): copy to `<OP2>\Zoom\op2zoom.dll` and add:

```ini
[Game]
LoadAddons = "... Zoom"      ; append to the existing list
WideZoom   = 1
ZoomWiden  = 2
[Zoom]
Dll = "Zoom\op2zoom.dll"
```

**Retail 1.3.6 with the original 2006 op2ext** (no `LoadAddons` support - it loads exactly one mod
from the command line): copy the DLL as `<OP2>\Zoom\op2mod.dll` and start the game with
`Outpost2.exe /loadmod Zoom` (e.g. a `Play-Zoom.bat` next to the exe). The `[Game]` keys above still
apply. op2ext writes `[DEBUG] ART_PATH` into the ini while a mod is loaded.

To use it: start a game and **zoom all the way out** with the **-** button beside the minimap (zoom
level 1) - the view widens to show more map. Zoom back in (2/3) for vanilla behaviour.

## Build

```
build.bat        (x86 -> op2zoom.dll)
```

`build.bat` finds Visual Studio through `vswhere`: any edition with the C++ x86 tools, including
Build Tools and prerelease/Insiders builds.

Must be 32-bit - it patches the 32-bit `Outpost2.exe` in-process.

## Status / notes

- **Working / playable** on retail 1.3.6 (live-tested 2026-11-23, Test 6): ~1.94x more map at a
  1192x734 window, no crash, smooth frame rate, clean scrolling, sidebar/minimap untouched, mouse
  correct (drag-select, click, right-click-move).
- **Working on OPU 1.4.1** (live-tested 2026-11-23, Test 7): GOG install + OPU, maximized
  2024x1118 window. The GOG `Outpost2.exe` is the same 1997 build (timestamp `34493B6A`) with
  identical code at every hooked address; loads via `[ExternalModules]` and reads `OPU\outpost2.ini`.
- **Known issue - selection-box lines (retail):** white selection-box lines can stay on screen while
  the units they belong to remain selected; they disappear as soon as those units are deselected.
  They are redrawn by the engine every frame (they survived engine full-background redraws), so
  screen repaints cannot clear them. Cause not found. **Under OPU they do not linger:** OPUPatch's
  `[Game] ForceFullRedraw=1` (written by default) redraws the whole view each frame.
- **Cost with `ForceFullRedraw=1`:** every frame presents the whole buffer, so the mod shrinks all of
  it each frame. With the packed-pixel fast path for scales <= 2 (each block is 1 or 2 pixels per
  axis; `(a & b) + (((a ^ b) & M) >> 1)` averages all three channels of two 16bpp pixels at once,
  exact for 555 and 565) this measured **105-143 ms/s** at 1.19x (was 264-433 ms/s with the
  per-channel box filter). Of that only ~30-45 ms/s was the resample; ~75-108 ms/s was the final
  1:1 `BitBlt` (16bpp -> 32bpp screen conversion), which unmodded zoom 1 also pays.
- **32bpp copy (`ZoomBlit32=1`, default):** for a 16bpp buffer the mod's window-sized copy is 32bpp;
  each resampled pixel goes through a 65536-entry table that **GDI itself builds** (all 16bpp values
  blitted into a 32bpp DIB), so colours are exactly GDI's conversion. The final blit is then a plain
  32->32 copy: ~0.7 ms per Mpx instead of ~2.2; total ~2.0 ms per Mpx instead of ~3.1. In the test the
  game used the time to present ~2.4x more pixels per second. `ZoomBlit32=0` restores the 16bpp copy.
- **Window size limits the zoom-out:** the engine's 400-byte tile bit vectors cap the scale by view
  size. A maximized 2024x1118 window (1824x1088 view) only gets **x19/16 (1.19x)**; a ~1192x734
  window gets 1.94x. Lifting it would mean giving the `Viewport` larger bit vectors (patching
  `Viewport::SetSurface`).
- **Activation** currently repurposes engine zoom level 1 (the widest). A future version could add a
  true 4th level + hotkey so vanilla zoom 1 stays reachable.
- **Edge/arrow scroll** uses `GetCursorPos` (not `lParam`), so it works at the wide level though the
  world step may feel different; revisit only if needed.
- Render/UI-only -> **MP-safe** (not a command packet; no simulation state touched).

## Key addresses (retail 1.3.6 / 1.4.1)

| What | Address |
|---|---|
| `zoom` global (`[Game] Zoom`) | `0x00576264` |
| `AllocSurface` (back-buffer factory) | `0x004A9170` |
| Present blit (`StretchDIBits` / `BitBlt`) | `0x004A9410` |
| Detail back-buffer alloc (`GetClientRect` -> AllocSurface) | `FUN_0049d030` |
| View transforms (clamp/centre/span/scroll) | `0x00407990`, `0x00408210`, `0x004082f0`, `0x00408370`, `0x00408540` |
| Panel layout (main window, right-aligns sidebar) | `FUN_004999a0` |
| Detail pane client pixel -> map pixel (`GetAbsolutePos`, vtable slot 10 @ `0x004CF5E8`) | `0x00408070` |
| Map mouse position global (`gameFrame.mapMousePos`) | `0x0057B7B4` |
| Per-frame cursor re-read (`GetCursorPos` -> `GetAbsolutePos`) | `Dans_RULE_UIFrame::OnIdle` `0x0049C310` |
| Minimap pixel -> map (not the detail pane; unaffected) | `MiniMap::PixelToMapLocation` `0x0048D930` |
| Game window HWND global | `0x005756C8` |
| `Viewport::SetSurface` (sizes the 400-byte tile bit vectors) | `0x0046F160` |
| `GFXClippedSurface::ScrollScreen` (`ScrollWindowEx`) | `0x004A9890` |

Full reverse-engineering notes and the test-iteration log live alongside this README in this folder:
`FINDINGS.md` (how the zoom/render/coordinate code works), `PLAN.md` (approach + phases), `TESTLOG.md`
(the live-test iterations), and `screenshots\`.
