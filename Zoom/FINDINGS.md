# op2zoom - findings: how OP2's main-view zoom works

Goal: add a **new, wider zoom level** (the user's "zoom 4") so the main game viewport shows **more
map** than the current widest level. Delivered as an **op2ext module**.

Source: the retail 1.3.6 `Outpost2.exe` code. The game the user runs is **OPU 1.4** (GDI renderer) - see the "must verify" note.

## The zoom variable
- The main-view zoom is a single global **`int zoom` at `0x576264`** - the persisted `[Game] Zoom`
  outpost2.ini setting (Game-settings struct offset `0xBA4`).
- Allowed values are **1, 2, 3** (clamped by the handler). The engine genuinely supports 3 levels.

## What a zoom value means (the mechanism)
- `AllocSurface @ 0x004A9170` builds the map back-buffer (a DIB section) at size
  **`(windowWidth / zoom) x (windowHeight / zoom)`** - i.e. **zoom is a divisor**.
- The world is drawn into that back-buffer at a fixed **32 px / tile**, then the back-buffer is
  presented to the window scaled **up by `zoom`** (retail: `StretchDIBits(windowDC, 0,0,
  width*zoom, ...)`).
- Net effect:
  - `zoom = 1` -> back-buffer = full window -> most tiles visible -> **widest view** (see most).
  - `zoom = 2` / `zoom = 3` -> smaller back-buffer, stretched 2x/3x -> **magnified**, fewer tiles
    (see less).
- The screen<->world coordinate transforms **divide by zoom** (`/ DAT_00576264`) in ~6 places
  (view-centre, scroll, mouse-to-tile pick), e.g. `all_functions.c:6456`
  `local_18 = param_1 - (clientRect.right / 2) / zoom;`. Higher zoom -> fewer world units edge-to-
  centre -> narrower view. This confirms the direction.

So **`zoom = 1` is already the widest the engine can show.** Seeing *more* than that needs an
effective zoom **< 1** (a back-buffer **larger** than the window, shrunk on present) - which the
integer divisor cannot express. That is the crux of this project.

> **Direction CONFIRMED (user, 2026-06-23):** pressing zoom-in magnifies (tiles get bigger, less
> map). So lower engine value = wider. The new level is effectively **"zoom 0"** - wider than the
> current widest (`zoom = 1`) - an effective sub-1 scale. The label is cosmetic; what matters is
> "render more map than `zoom = 1`".
>
> **Build (user, 2026-06-23):** OPU **1.4.1** and retail **1.3.6** are the same exe apart from
> patches, and **the zoom code is unchanged**. So the retail addresses in this doc are valid on the
> user's build. The one thing OPU did patch is the **renderer/present** (forces GDI) - which is
> precisely the present-blit our larger back-buffer must shrink through. That is the only Step 0
> unknown left.

## The +/- handler and direct-set commands
In the command dispatcher (the big switch around `all_functions.c:84030+`, `FUN_004868f1`):
- **Zoom in** - command `0x9D16`: capture view centre (`FUN_004082f0`), `zoom++`, clamp to 1..3,
  `FUN_00407880` (re-alloc the Detail-pane surface at the new zoom), `FUN_00408210` (re-centre).
- **Zoom out** - command `0x9D17`: same but `zoom--`, clamp 1..3.
- **Direct set** - `0x9CF7/0x9CF8/0x9CF9` set zoom to `1/2/3` then `FUN_00407880`.

Clamp logic (both directions): `if (zoom > 2) zoom = 3; if (zoom < 2) zoom = 1;` -> snaps to
{1,2,3}.

## Key functions / addresses (retail 1.3.6)
| What | Address |
|---|---|
| `zoom` global (`[Game] Zoom`) | `0x576264` |
| Surface factory `AllocSurface(.., w, h, hWnd, bpp, zoom)` | `0x004A9170` |
| `IWnd:Pane.ReallocSurface(int zoom)` | `0x0049D030` |
| `IWnd:Pane:Detail.ReallocSurface(int zoom)` (main map view) | `0x00407900` |
| Re-alloc-after-zoom helper (`FUN_00407880`) | `0x00407880` |
| Re-centre view (`FUN_00408210`) | `0x00408210` |
| Get view centre (`FUN_004082f0`) | `0x004082F0` |
| Zoom command handler (switch) | `~0x004868F1` |
| **Present blit** `FUN_004a9410` (GDI) | `0x004A9410` |
| GFX surface struct: `int zoom` field | `GFXClippedSurface + 0x10C` |

## Present path - VERIFIED (Step 0)
`FUN_004a9410 @ 0x004A9410` is the map present. It is **GDI** (so "OPU forces GDI" = it always
takes this function, not the DirectDraw alternative - our retail address is very likely valid on
OPU 1.4; verify the bytes at load):
- `zoom > 1`: **`StretchDIBits`** - src = back-buffer `(0,0, bbW, bbH)` unscaled; dest =
  `(0,0, bbW*zoom, bbH*zoom)`. **dest = src x zoom** (upscale).
- `zoom == 1`: **`BitBlt`** 1:1 fast-path (no scaling).
- Surface struct fields used: `+0x3C` bbW, `+0x40` bbH, `+0x80` dest HDC, `+0x34` pixels,
  `+0xF4` BITMAPINFO ptr, `+0x10C` zoom.

## AllocSurface - VERIFIED (Step 0)
`FUN_004a9170(p1, palFlags, &out, clientW, clientH, hWnd, bpp=0x10, zoom)`:
- back-buffer **width = `(clientW / zoom) & ~1`**, **height = `clientH / zoom`** (16bpp top-down DIB
  via `CreateDIBSection`).
- **`clientW`/`clientH` come from the CALLER's `GetClientRect(hWnd)`** - AllocSurface does not read
  the window itself.

## Size reads are funnelled through GetClientRect(viewportHWND) - VERIFIED (Step 0)
There is **no cached client-rect global**. AllocSurface's caller and every coordinate transform
(`0x408210`, `0x4082F0`, `0x408540`, `0x407990`) each call **`GetClientRect` on the same viewport
HWND**, then divide by zoom. This makes **approach 3a viable**: a single `GetClientRect` hook keyed
on that HWND, returning `window x WIDEN`, makes AllocSurface build a wider buffer AND every transform
treat the view as wider - from one hook. Coordinate-divide sites confirmed at all_functions.c lines
6456/6457 (`FUN_00408210`), 6487/6489 (`FUN_004082f0`), 6618/6621 (`FUN_00408540`), 5926/5935
(`FUN_00407990`, this one reads zoom from surface `+0x10C` not the global).

## MUST VERIFY before building
The decompile here is **retail 1.3.6**; the user runs **OPU 1.4**, which "forces GDI mode" and
patches the present from DirectDraw `StretchDIBits` to **`BitBlt`**. The exact OPU present path
decides how we make a larger-than-window back-buffer shrink to the window. Step 0 of the plan is to
confirm OPU 1.4's renderer (the `BitBlt`/`StretchBlt` present and whether the back-buffer is
window-sized or window/zoom there). Retail 1.3.6 is the
matching build; OPU's NetFix/renderer patches live in its addon DLLs.

## Coordinate transforms + present (decompiled bodies, Step 1.5)
All read `GetClientRect(*(HWND*)(in_ECX+8))` (the viewport pane HWND) then use the zoom global
`DAT_00576264` or surface field `+0x10C`. World is `<<5` = 32 px/tile.

- **`FUN_00408210` set view-centre** - inputs are WORLD coords:
  `local_18 = param_1 - (rect.right/2)/zoom; local_14 = param_2 - (rect.bottom/2)/zoom;`
  clamps via `FUN_00407990`, stores at `pane+0x140/0x144`. (rect /zoom -> at zoom=1 a wide rect
  widens the centre correctly.)
- **`FUN_004082f0` get view-centre** - `out = origin + (rect/2)/zoom`.
- **`FUN_00408370` visible tile range** - `out = (origin + rect.right) >> 5` etc. - adds `rect.right`
  straight into the world span (NO /zoom here), so a wide rect = wider draw span (matches a wide
  buffer).
- **`FUN_00407990` clamp** - bounds use `rect.right / *(pane+0x14 +0x10c)` (the surface zoom field).
- **Present `FUN_004a9410`**: `if (surface.zoom==1) BitBlt(dst,0,0,bbW,bbH,src,0,0,SRCCOPY)` (full)
  or partial `BitBlt` of a clamped dirty rect (also 1:1); `else StretchDIBits(dst, 0,0, bbW*zoom,
  bbH*zoom, 0,0, bbW,bbH, ...)`. So our wide level (zoom field = 1) hits the **BitBlt** path - which
  we intercept at the GDI IAT and redirect to `StretchBlt` to shrink the wide buffer to the window.
- **`AllocSurface FUN_004a9170(_, pal, &out, clientW, clientH, hWnd, 0x10, zoom)`**: back-buffer
  `w=(clientW/zoom)&~1, h=clientH/zoom`, 16bpp top-down DIB via `CreateDIBSection`; stores zoom at
  surface `+0x10C` (`piVar2[0x43]`). `clientW/H` come from the CALLER's `GetClientRect`.
- **Mouse-to-tile** (`~FUN_0048d930`): converts screen px using an internal viewport origin (not
  obviously `clientRect`-based); unclear whether it divides by the zoom global. **Test live**; if the
  pick lands off by WIDEN, scale the incoming mouse px by WIDEN.
- Note: `FUN_00407900` does NOT exist (208-byte gap 0x407880..0x407a50); the realloc-on-zoom helper
  is `FUN_00407880`.

## Screenshots
The three zoom levels (user-provided) belong in `screenshots/`:
- `zoom_in.png`  - most magnified (fewest tiles).
- `zoom_mid.png` - middle level.
- `zoom_wide.png` - current widest (what the user plays); the new level should show more than this.

(They were pasted into chat; drop the PNG files here - inline images can't be written to disk by
the tooling.)
