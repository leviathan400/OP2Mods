# op2zoom - live test log

## Test 1 (2026-06-23) - first wide-render cut
Build: IAT hooks GetClientRect (lie x WIDEN for game window when zoom==1) + BitBlt (StretchBlt-shrink
the over-sized map buffer to the real window). WideZoom=1, ZoomWiden=2.

**Result:** hooks installed OK (`GetClientRect=OK BitBlt=OK`), wide level engaged at engine zoom 1,
and the map **zoomed out / showed more map = the core mechanism WORKS.** BUT the right sidebar +
top status/command panels disappeared - the map (detail pane) rendered over the whole window,
covering where the panels should be.

**Diagnosis:** the `GetClientRect` lie was applied **globally** for the game window. The side panels
position themselves **right/top-aligned relative to client width/height** (e.g. `sidebarX =
clientWidth - sidebarW`). With `clientWidth x 2`, the panels were placed at ~2x width -> pushed off
the right edge of the real window, so they render off-screen and the map fills everything.

**Fix:** scope the lie to ONLY the map/detail callers (surface alloc + the view transforms at
~0x407990/0x408210/0x4082f0/0x408370/0x408540 + the draw-extent), and tell the TRUTH to the
panel-layout callers. Same HWND + same API, so distinguish by **caller return address**
(`_ReturnAddress`). Next build logs every distinct GetClientRect caller (address + size) so the
map-caller address range can be pinned exactly, then the lie is gated on that range.

**Confirmed working:** wide buffer render + StretchBlt shrink + zoom-1 activation gating.
**Still to verify after scoping:** panel placement restored; mouse-to-tile picking; edge scroll;
dirty-rect cleanliness.

## Test 2 (2026-06-23) - caller-address diagnostic
Logged each distinct GetClientRect caller while wide-active. Results (caller -> window/size/role,
resolved against the decompile):

| caller | window | size | role |
|---|---|---|---|
| 0x4999C4 (`FUN_004999a0`) | MAIN (0x5756C8) | 1000x638 | **LAYOUT** - sizes detail pane `cx=clientW-200`, right-aligns minimap/cmd pane at `x=cx+8`. **Lying here moved panels off-screen.** |
| 0x499CB9 (`FUN_00499c90`) | MAIN | 1000x638 | paint/frame draw |
| 0x49D047 (`FUN_0049d030`) | detail (`in_ECX[2]`) | (0x0 early) | **ALLOC** - feeds clientW/H into AllocSurface 0x4A9170 = the map back-buffer size |
| 0x4079A7 (`FUN_00407990`) | detail (`+8`) | 1792x1248 | clamp view pan (/zoom) |
| 0x40823E (`FUN_00408210`) | detail (`+8`) | 1792x1248 | centre view (/zoom) |
| 0x408307 (`FUN_004082f0`) | detail (`+8`) | 1792x1248 | get view-centre (/zoom) |
| 0x49DA3A (`FUN_0049da70`) | detail (raw) | 1792x1248 | cursor-bounds test |
| 0x458AC7 / 0x45CE5F / 0x41DD3B | own | 192x128 / 192x1142 / 326x132 | minimap / sidebar / dialog |

**Conclusion:** the map subsystem (alloc 0x49d030 + transforms 0x407990/0x408210/0x4082f0/...) reads
the DETAIL window; the panel LAYOUT (0x4999a0) reads the MAIN window. They are different windows. So
scope the x WIDEN lie to the MAP callers ONLY (by return-address range) and leave layout/paint/panels
truthful. Present auto-targets the destination window via `WindowFromDC` (with the alloc-captured
detail size as fallback) so the wide buffer shrinks to fit the detail pane, not the whole window.

## Test 3 (2026-06-23) - scoped lie - PANELS RESTORED, map wider
Scoped the x WIDEN to map callers only. **Result: sidebar/minimap/objectives/status bar all back in
place, and the detail viewport renders ~2x more map.** Big win. Log confirmed the scoping:
`0x4999C4` (layout) and panel callers = `(truthful)`; `0x49D047` = `[WIDEN:alloc]`; `0x407990/408210/
4082f0` = `[WIDEN:transform]` reading the detail window at 800x608.

**One leak:** `present shrink: buf=384x256 -> 192x128` = the MINIMAP. The minimap surface is allocated
through the SAME `FUN_0049d030` as the detail map, so `[WIDEN:alloc]` widened it too (192->384), then
the present shrank it back - needless super-sampling of a sidebar element. (Detail = 800, minimap =
192, command pane small.)

**Fix:** widen the alloc only for the LARGE detail surface (`w >= 400`); leave small panel surfaces
(minimap/command pane) truthful. Transforms only ever read the detail window so they stay as-is.

## Test 4 (2026-06-23) - minimap excluded - WORKING
**Success.** Viewport renders ~2x more map; minimap + sidebar + objectives + status bar all pixel-
clean (vanilla). Log: only `present shrink: buf=1600x1216 -> 800x608` (the detail map); NO 192x128
minimap shrink. All panel callers truthful, only alloc(detail)+transforms widened. User: "well done".

**Mechanism that works (retail 1.3.6, OPU bytes MATCH):**
- Activate at engine zoom 1 (gated on `[Game] WideZoom`), `ZoomWiden` = scale (2).
- IAT-hook GetClientRect: x WIDEN only for map callers by return-address - `FUN_0049d030` alloc
  (and only when `w>=400`, i.e. the detail surface, not the shared small panel surfaces) +
  transforms `FUN_00407990/00408210/004082f0/...`. Layout `FUN_004999a0` (main window) stays truthful
  -> panels keep their place.
- IAT-hook BitBlt: when a source bitmap is wider than its destination window (the over-sized detail
  buffer), StretchBlt-shrink it to fit (target via WindowFromDC). Uniform for full + dirty-rect blits.

## Test 5 (2026-06-23) - mouse scaling for the wide level
Test 4 result confirmed: zoom works but **not playable** - the selection drag-box (and pick) land in
the wrong place. Root cause: the detail WndProc `FUN_00458500` reads the mouse from message lParam
(client px 0-800); world conversion `FUN_0048d930` divides by the SURFACE width, which we doubled to
1600 -> a 0-800 mouse only addresses the left half of the buffer.

**Fix:** subclass the detail window (captured via WindowFromDC in the present hook) and multiply the
mouse-message lParam x/y by WIDEN when wide-active (WM_MOUSEMOVE + L/R/M button down/up/dblclk). Puts
the whole mouse path (rubber-band, unit pick, click-to-move) into the wide buffer's coord space.
Expect: drag-box and selection track the cursor; right-click move targets correctly.
(Edge-scroll uses GetCursorPos, not lParam - unaffected for now; revisit if needed.)

**Test 5 result: WORKING / PLAYABLE.** Drag-select box, single-click select, and right-click move all
track the cursor correctly at the wide level. User: "Its working".

## Status: PLAYABLE on retail 1.3.6
Wide zoom level functions end-to-end - renders ~2x more map, panels untouched, mouse fully usable.

## Optional follow-ups (not blocking)
- **Edge + arrow scroll** - uses GetCursorPos, not lParam; works but the world step may feel different
  at the wide level; revisit only if it bothers in play.
- **WIDEN tuning** - 2 = tiles at 16px; try other values via `[Game] ZoomWiden`.
- **A true 4th level + hotkey** instead of repurposing zoom 1 (so vanilla zoom 1 is still reachable).
- **Test on the OPU 1.4 install** (renderer bytes MATCH retail, so expected to work).

## Test 6 (2026-11-23) - retail 1.3.6, 1192x734 window - crash, speed and redraw fixes
Install: retail 1.3.6 with the **2006 op2ext**, which ignores
`LoadAddons` and loads one mod via `Outpost2.exe /loadmod Zoom` -> `Zoom\op2mod.dll`). Renderer
prologues `MATCH`; ini and log taken from the game folder as designed.

Iterations (each a live run):
1. **Crash ~1 s after start** at `Outpost2.exe+0x6F26C` (AV), also with the previous committed build
   (pre-existing; June tests used an 800x608 view). Cause: `Viewport::SetSurface` (`0x0046F160`) sizes
   five redraw bit vectors from the surface, each a fixed `bitVectorBuf[400]` (the decompile's
   `struct Viewport`). The 992x704 view at 2x needs 414 bytes -> overrun into the bit-vector pointers
   (`+0x804..+0x814`) -> NULL write. **Fix:** pick the largest scale <= ZoomWiden (1/16 steps) whose
   bit vectors fit 400 bytes -> x31/16 (1.94x, 1922x1364 buffer). Transforms + mouse use the same
   scale, and only once the buffer is widened. `WideZoom=0` isolated it (no hooks -> no crash).
2. **Works, but slow + constant redraw.** Stats: ~1600 small presents/s, only 4 full-view; GDI
   `StretchBlt` 230-330 ms/s, and per-rect resampling made redrawn tiles shimmer. **Fix:** own
   resample (global mapping) into a window-sized DIB + 1:1 `BitBlt`.
3. First own-resample build never ran: the buffer is **16bpp BI_BITFIELDS 555** (`compression=3`),
   not BI_RGB -> fell back to StretchBlt. **Fix:** match the buffer's own format.
4. **Map drawn upside down:** GetObject reports `biHeight=+1364` but the buffer is top-down. **Fix:**
   don't trust the sign; detect from pixels, default top-down (detection never decides; top-down is
   correct). Shrink cost now 56-84 ms/s.
5. **Scrolling smeared** until tiles redrew: `ScrollScreen` (`0x004A9890`) calls
   `ScrollWindowEx(dx, dy)` in buffer pixels. **Fix:** hook it; at the wide level repaint the whole
   view from the scrolled buffer instead. Scroll clean.
6. **1 px selection-box edges broke up / vanished** (nearest sampling skips ~every other row/column),
   and an early-exit dropped 1 px dirty rects on skipped columns. **Fix:** `ZoomSmooth=1` box-filter
   average (thin lines continuous) + process every small pixel whose block touches the rect. Cost
   brought back down (row order decided once; reciprocal table instead of per-pixel division).
7. **White selection-box lines linger.** Tried: periodic full screen repaints (from presents, then
   from a timer - they ran, lines stayed), and engine `Viewport::MarkEverywhereForRedraw`
   (`0x0046FCC0`) after each box release (ran 8-10x per 5 s with full presents - lines stayed). So
   they are **re-drawn by the engine every frame**; they **vanish when the units are deselected**.
   Cause not found (likely selected-unit selection boxes drawn at the wide size). Redraw-all removed
   again; a 1 s screen safety repaint kept.

**Result: PLAYABLE on retail 1.3.6.** ~1.94x more map, smooth, clean scrolling, panels untouched,
mouse correct. Known issue: lingering selection-box lines while units stay selected.
Final build adds `[Game] ZoomStats` (stats logging, off by default). Not yet run on OPU 1.4.1.

## Test 7 (2026-11-23) - OPU 1.4.1, maximized window
Install: fresh GOG + OPU 1.4.1. The GOG `Outpost2.exe` (1,144,832 B)
is the same 1997 build as retail 1.3.6 (timestamp `34493B6A`); bytes at every hooked/verified
address are identical. Installed `op2zoom.dll` next to `OPUPatch.dll` in `OPU\`, enabled with
`[ExternalModules] Zoom=yes` + `[Zoom] Dll=op2zoom.dll` in `OPU\outpost2.ini`, started via
`OPULauncher.exe`.

- Log in `OPU\logs\op2zoom.log`; ini read from `OPU\outpost2.ini` (game-folder rule works under OPU);
  both prologues `MATCH`; `hooks: BitBlt=OK  GetClientRect=OK  ScrollWindowEx=OK`.
- Maximized 2024x1118 window -> 1824x1088 view -> `wide buffer: 1824x1088 -> 2166x1292 (x19/16,
  reduced to fit the engine's 400-byte tile bit vectors)`: only **1.19x** at this size.
- Row order detected from pixels this time (`top-down, votes top=8 bottom=0`); fast shrink and
  scroll repaint active.
- OPU writes `[Game] ForceFullRedraw=1`: the whole view is redrawn and presented every frame. Result:
  **selection-box lines no longer linger** (user: "the redraw boxes are better. they dont linger").
  Cost: ~1300-1750 full-strip shrinks per 5 s, 160-223 Mpx, **264-433 ms/s** shrink time.

**Result: WORKING on OPU 1.4.1.** Optional follow-ups: lift the 400-byte limit (bigger Viewport bit
vectors) for ~2x on large windows; packed-pixel fast path for scales <= 2 to cut the shrink cost.

## Test 8 (2026-11-23) - OPU 1.4.1, packed-pixel fast path
Same setup as Test 7 (maximized, x19/16, `ForceFullRedraw=1`, `ZoomStats=1`). New: for scales <= 2
the box filter averages packed 16bpp pixels with `(a & b) + (((a ^ b) & M) >> 1)` (M = 0x7BDE for
555, 0xF7DE for 565; verified exact against a per-channel average for both). Stats now split the
final blit out.

- Shrink time **105-143 ms/s** (was 264-433 ms/s in Test 7), of which the 1:1 `BitBlt` to the
  window is 75-108 ms/s and the resample ~30-45 ms/s. 184-236 Mpx per 5 s.
- Next possible step: 32bpp small DIB (LUT conversion in the resample) so the blit needs no format
  conversion.

## Test 9 (2026-11-23) - OPU 1.4.1, 32bpp small copy (ZoomBlit32)
Same setup as Test 8. New: the window-sized copy is 32bpp; resampled 16bpp pixels are converted
through a lookup table built by GDI (log: `16->32bpp lookup table built by GDI (compression=3,
white 0x7FFF -> FFFFFF)`), so the final 1:1 blit is 32->32.

- The game presented ~2.4x more pixels per second than in Test 8 (330-505 Mpx per 5 s vs
  184-236), so totals rose to 93-304 ms/s; per Mpx it is cheaper: blit **~0.7 ms/Mpx** (was ~2.2),
  total **~2.0 ms/Mpx** (was ~3.1). The LUT adds ~0.4 ms/Mpx to the resample.
- `ZoomBlit32=0` keeps the previous 16bpp copy for comparison.

## Test 10 (2026-11-23) - OPU 1.4.1, selection box offset (125% display scaling)
User: on OPU 1.4.1 the selection box "doesn't start under the cursor" (photos: box up-left of the
cursor). Windows display scaling is 125% (OPU `DPIAwareness=-5`, desktop reported as 2048x1152); the
map pane is 1824x1088 logical, drawn at 2280x1360.

- `ZoomStats=1` click logging: for press/release, raw `lParam` = cursor (client), the scaled position
  and the game's map position (`0x0057B7B4` - view origin) were exact - so not a DPI mismatch.
- Scripted drag (SetCursorPos + mouse_event from a PowerShell script, screenshot while holding):
  the box's **anchor was exact**, its **moving corner at cursor / 1.19**.
- Moving the scaling from the window procedure into `Detail::CallFilters` (vtable slot 5) changed
  nothing - the drag position does not come from messages.
- Cause: `Dans_RULE_UIFrame::OnIdle` (`0x0049C310`) re-reads the cursor every frame (`GetCursorPos`
  -> `ScreenToClient` -> `GetAbsolutePos`) and overwrites the map mouse position unscaled.
- **Fix:** scale inside `Detail::GetAbsolutePos` (`0x00408070`) via vtable slot 10 (`0x004CF5E8`,
  only reference), which both the message path and `OnIdle` call; `lParam` is no longer touched.
  Scripted drag afterwards: the box runs exactly from the press point to the cursor.
