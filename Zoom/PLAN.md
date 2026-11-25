# op2zoom - plan: an op2ext module that adds a wider zoom level

**Goal:** add a new main-view zoom level that shows **more map than the current widest** (`zoom = 1`)
- the user's "zoom 4" - shipped as an op2ext addon DLL for OPU 1.4.

**Why it's not trivial (from FINDINGS):** zoom is an integer **divisor** - back-buffer =
`window / zoom`, presented scaled `x zoom`. `zoom = 1` is already the widest. Seeing *more* needs an
effective scale **< 1**: a back-buffer **larger** than the window, **shrunk** on present, with the
screen<->world math scaled to match. The engine's own math is internally consistent for a fractional
zoom; only the integer storage + the present blit block it.

## Step 1.5 findings (decompiled bodies) - refine the approach
Read the actual bodies of the transforms + present (see FINDINGS.md "Coordinate transforms"). Key
corrections to the original 3a sketch:
- **Force engine `zoom = 1` for the wide level** (don't use a sentinel/0 - that div-by-zeros every
  `/zoom` site). With `zoom = 1`, lying `GetClientRect` x WIDEN is *consistent*: view-centre
  (`FUN_00408210`: `world - (rect/2)/zoom`), visible-tile span (`FUN_00408370`: `origin + rect.right`)
  and `AllocSurface` (`(rect/zoom)&~1`) all widen correctly. (The Explore "breaks centering" verdict
  assumed zoom stayed 2/3; at zoom=1 it's fine.)
- **Present = IAT-hook `BitBlt`, not a detour.** At zoom=1 the present (`FUN_004a9410`) is a plain
  `BitBlt` (full + dirty-rect partials), src = the map memDC holding the wide DIB. Hook `BitBlt`:
  when the *source bitmap* is wider than the true client (i.e. our wide buffer), redirect to
  `StretchBlt` mapping buffer-space -> real-window-space (scale = trueClient/bufferDim). Uniform for
  full and partial blits; no trampoline, no calling-convention guess. SAFE.
- **Activation (MVP): zoom level 1 *becomes* the wide level**, gated on `[Game] WideZoom`. The user
  plays on 3; zooming out to 1 already fires the engine's realloc (`FUN_00407880` -> GetClientRect
  -> AllocSurface), so no clamp patch / new-level / div-by-zero. A true 4th level + hotkey can come
  later.
- **Open risk - mouse-to-tile pick.** The pick path (`FUN_0048d930`/detail pane) does not clearly run
  through `clientRect`; it may divide screen px by the zoom global (=1 now) and so land off by
  WIDEN. Build render-side first, test, then add a mouse-input x WIDEN scaler if needed.

## Approach (LOCKED after Step 0): 3a "client-rect illusion", two hooks
Step 0 verified all size reads funnel through `GetClientRect(viewportHWND)` (no cached rect global),
and the present is the GDI `FUN_004a9410 @ 0x004A9410`. So:

**Engine zoom stays `1`** (widest the divisor allows). Our new wide level is gated on our own flag
and a scale factor `WIDEN` (start 2). When the flag is on:

1. **Hook `GetClientRect`** (IAT or inline) - when called for the viewport HWND, multiply the
   returned `right`/`bottom` by `WIDEN`. Effect, all from this one hook:
   - `AllocSurface` builds the back-buffer at `(window*WIDEN)/zoom` = **window*WIDEN** (bigger buffer,
     more map rendered).
   - every coordinate transform (`0x408210`, `0x4082F0`, `0x408540`) sees `(window*WIDEN)/zoom` -
     **treats the view as WIDEN x wider** (scroll, view-centre, mouse-to-tile all consistent).
2. **Hook the present `FUN_004a9410`** - the back-buffer is now `window*WIDEN`, but with `zoom==1`
   the stock present takes the 1:1 `BitBlt` branch (no shrink) and would overflow the window. Force
   the **StretchDIBits** path with **dest = the REAL window rect** (the un-lied `GetClientRect`),
   src = full back-buffer -> shrinks the wide buffer down into the window. This is the only site that
   must see the true window size.

So: **lie in `GetClientRect`, tell the truth in the present.** Net effect = effective sub-1 zoom
("zoom 0") with consistent input/scroll math. The user-facing label is cosmetic.

Watch items carried into Step 3/4: `FUN_00407990` reads zoom from the surface `+0x10C` field (not
the global) for scroll bounds - confirm it still scales right with the wider buffer; and the present
hook must special-case ONLY the viewport surface (side panes keep BitBlt 1:1).

## Phases

### Step 0 - verify the renderer - DONE
- Present = GDI `FUN_004a9410 @ 0x004A9410` (`StretchDIBits` when zoom>1, `BitBlt` 1:1 when zoom==1).
- AllocSurface `FUN_004a9170 @ 0x004A9170` builds back-buffer = `(clientW/zoom)&~1 x clientH/zoom`,
  dims from the caller's `GetClientRect(hWnd)`.
- All size reads funnel through `GetClientRect(viewportHWND)` -> **approach 3a chosen**.
- Direction confirmed by user (zoom-in magnifies; zoom=1 widest).
- **Remaining check (do at module load, Step 1):** confirm OPU 1.4's bytes at `0x4A9410` / `0x4A9170`
  still match retail (read/disasm in-process, or just log on first hook). User says zoom code is
  unchanged 1.3.6->1.4.1, so expected to match; the renderer patch is what we verify.

### Step 1 - op2ext module skeleton
- Module `op2zoom.dll` (C DLL). Entry points op2ext expects: `InitMod` /
  `mod_init` (`/ mod_run` / `mod_destroy`). Modelled on the existing addons (scrollfix, rally).
- Load via `outpost2.ini`: `[Game] LoadAddons="... Zoom"` + `[Zoom] Dll="Zoom\op2zoom.dll"`.
- In `InitMod`, install in-process hooks (trampoline/detour or direct byte-patch at known VAs).
- Build x86 (the game is 32-bit).

### Step 2 - allow a 4th level + key wiring
- Patch the zoom clamp (the `1..3` snap in the +/- handler at `~0x4868F1` / commands `0x9D16`/`0x9D17`)
  so it accepts the new level, and/or add a hotkey/command that selects it directly (mirroring the
  `0x9CF7/8/9` direct-set commands).
- Ensure selecting it calls the re-alloc (`FUN_00407880`) + re-centre (`FUN_00408210`) so the surface
  rebuilds at the wider size.

### Step 3 - implement the wider render
- Hook the Detail-pane surface alloc (`IWnd:Pane:Detail.ReallocSurface @ 0x00407900` ->
  `AllocSurface @ 0x004A9170`) to build the larger back-buffer for our level.
- Hook the present to stretch-shrink to the real window.
- Apply the coordinate scaling (3a or 3b).
- Keep the side panes (minimap, command pane, objectives) untouched - they're separate surfaces.

### Step 4 - test + iterate
- Build base first (no behaviour change) -> confirm the module loads and the game runs.
- Enable the level -> check: more map visible; **mouse-to-tile picking** lands correctly; **edge
  scroll**; **dirty-rect redraw** (no smearing); **fog/vision**; minimap unaffected; text/units just
  smaller (expected).
- Tune `WIDEN` (2 first; maybe 1.5 if 2 is too small/illegible).

## Risks / open questions
- **OPU 1.4 present differs from retail** - Step 0 must pin it; the stretch-shrink hook depends on it.
- **Coordinate math breadth** - if size reads aren't centralized, 3b touches several functions.
- **Dirty-rect redraw** assumes back-buffer == window/zoom in places; a larger buffer may need the
  redraw rects rescaled or full-redraws forced.
- **Legibility** - WIDEN=2 makes 32 px tiles render at 16 px; units/text get small. Acceptable per the
  goal (see more), but tune.
- **MP determinism** - zoom is local/view-only (not a command-packet), so a client-side zoom level is
  **safe in multiplayer** (cosmetic, no desync). Confirm it isn't tied to any synced state.

## Build / tooling
- VS toolchain, **x86** (`vcvars32`). op2ext headers/sources from the
  op2ext project (OutpostUniverse/op2ext).
- Reference: the retail 1.3.6 `Outpost2.exe` code (function names and addresses as used below).

## Status - WORKING (2026-06-23)
Steps 0-4 done. The wide zoom level renders ~2x more map in the detail viewport with the sidebar
untouched, live-tested on retail 1.3.6 (OPU bytes MATCH). See TESTLOG.md for the four test iterations.
Final mechanism: activate at engine zoom 1 (`[Game] WideZoom`/`ZoomWiden`); IAT-hook GetClientRect to
x WIDEN only the map callers (alloc `FUN_0049d030` when w>=400, + transforms), leaving layout/panels
truthful; IAT-hook BitBlt to StretchBlt-shrink the over-sized detail buffer to its window.

Remaining: verify mouse-to-tile pick + scroll at the wide level; tune WIDEN; optionally promote to a
real 4th level + hotkey; confirm on the OPU 1.4 install.
