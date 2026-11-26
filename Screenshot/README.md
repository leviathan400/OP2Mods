# op2shot - Outpost 2 screenshots (op2ext module)

A small op2ext module for **OPU 1.4.1** that saves a `.bmp` on a hotkey. Three modes:
- **F12 - viewport only** (`op2shot_view_*.bmp`): the rendered game view, no UI panels.
- **F11 - full window** (`op2shot_full_*.bmp`): the whole window - title bar, borders, viewport,
  command pane, minimap and message/status panels.
- **F10 - full map** (`op2shot_map_*.bmp`): the **whole playable map** rendered from the in-memory
  tile grid - not limited to the viewport, and reflecting **live game state** (laid tubes, walls,
  bulldozed/levelled terrain, lava, scorch - everything the engine has baked into the tile data).

Every image comes out at its final size, with no black or padded edges to crop afterwards.

## Status
Tested in game on **OPU 1.4.1** (2026-11-26, Windows display scaling 125%, Zoom module off):

| Key | Result |
|---|---|
| F12 | full 1824x1088 view, including right after a scroll (when the game only redraws a strip) |
| F11 | 2048x1152 - exactly the game window |
| F10 | playable area only: 64x63 tiles on the tutorial, 256x254 on a 512x256-stored map; no padding |

Two shots in the same second both saved (`_2` suffix). The hooks are removed cleanly on exit.

## Install (OPU 1.4.1)
1. `build.bat` -> `op2shot.dll` (32-bit; it hooks the 32-bit `Outpost2.exe` in-process).
2. Copy `op2shot.dll` to `OPU\op2shot.dll`.
3. In `OPU\outpost2.ini`:
   ```ini
   [ExternalModules]
   Screenshot=yes

   [Screenshot]
   Dll=op2shot.dll
   ```
   Optional settings, all under `[Screenshot]`. Numbers are **decimal** (the ini reader doesn't
   understand `0x..`):

   | Key | Default | Meaning |
   |---|---|---|
   | `HotKey` | `123` | viewport-only key (virtual-key code; 123 = F12) |
   | `WindowKey` | `122` | full-window key (122 = F11) |
   | `MapKey` | `121` | full-map key (121 = F10) |
   | `MapScale` | `1` | full map: `1` = 32 px/tile, `2` = 16, `4` = 8 (smaller files) |
   | `MapLight` | `0` | full map light level: `0` = full daylight, higher = darker |
   | `MapDiag` | `0` | `1` = log tile/palette diagnostics on F10 |
   | `MinWidth` / `MinHeight` | `400` / `240` | smallest surface counted as the viewport (panels are smaller) |

   Settings are read once at startup.

## Use
In a running mission: **F12** saves the viewport, **F11** the whole window, **F10** the full map.
Files go to `OPU\screenshots\op2shot_{view|full|map}_YYYYMMDD_HHMMSS.bmp`; a second shot in the same
second gets `_2`, `_3`, ... so nothing is overwritten. Activity is logged to `OPU\logs\op2shot.log`.

Under OPU the game is started with the `OPU` folder as its current directory, while `Outpost2.exe`
sits one level up next to an old, unused `outpost2.ini`. op2shot uses the folder that holds the live
`outpost2.ini` (the current directory), falling back to the exe's folder.

## How it works
OPU runs the renderer in **GDI windowed mode**. A thread keyboard hook on the game's own thread
watches for the hotkeys, so all three captures run on the thread that owns the window, the view
and the map, and never race the engine.

### Full window (F11)
The composited frame is already on the window, so op2shot just `GetWindowDC(gameWindow)` and
`BitBlt`s the **entire window** (title bar + borders + client) into a 24bpp DIB. The game window
handle comes from the engine global `0x005756C8`, falling back to the foreground window.

**Display scaling:** the game window is DPI-unaware. With Windows scaling at 125%, the game's
thread sees the window as 2560x1440 while the window itself only has 2048x1152 pixels, so a naive
capture has a black margin. op2shot reads the window size with its thread switched to the window's
own DPI mode, so the image is exactly the window's pixels. The log shows both sizes and the window's
DPI.

### Viewport only (F12)
The back-buffer holds *only* the viewport; the game presents it with
`BitBlt(windowDC, ..., viewSurfaceDC, ...)`. op2shot **IAT-hooks `BitBlt`**, arms a one-shot capture
on the key, and on the next present whose **source surface is viewport-sized** (>= `MinWidth` x
`MinHeight`, so small panels are skipped) saves that **whole surface** - the pure game view.

It saves the whole surface, not just the area that present copies: the game often presents only
the part of the view that changed (e.g. 1634x225 after a scroll), so the copied area alone would be
a fragment. The view surface is exactly the viewport size (e.g. 1824x1088). The hook chains to
whatever was in the slot before (the real `BitBlt`, or another module's hook) and is removed on
unload.

**With the Zoom module** the view surface is Zoom's wider buffer. When a map caps the zoom below
Zoom's maximum, part of that buffer is unused and F12 shows it as a black border; tested without
Zoom.

### Full map (F10)
The back-buffer only ever holds the viewport, so F10 doesn't screen-grab at all. It reads the data
the engine keeps in memory for the *whole* map and renders the image itself, using the engine's own
layout (`GetTile @ 0x476d00`, tile pass `@ 0x4a89a0` caller). The fields live in the engine's static
Map object at `0x0054F7F8`:

```
Map fields:  tileXMask 0x54F804  tileWidth 0x54F808  tileHeight 0x54F810
             clip rect 0x54F814 x1, 0x54F818 y1, 0x54F81C x2, 0x54F820 y2 (playable area, inclusive)
             lgTileHeight 0x54F82C  tileData 0x54FC5C  tileSetArray 0x54FC60
             terrainTypeMgr 0x54FC64 (+4 = TileMapping[] base)
for each tile (x, y) in the clip rect (grid stored in 32-tile-wide column strips):
    word        = tileData + (((x>>5) << lgTileHeight) + y) * 128 + (x & 31) * 4
    mapIdx      = (word & 0xFFE0) >> 5
    TileMapping = TileMapping[mapIdx]           ; {u16 tilesetIndex, tileGraphicIndex, animCount, animDelay}
    TileSet*    = tileSetArray[tilesetIndex]    ; BmpManager:TileSet (0xFC bytes)
    tilePixels  = TileSet[+0xF8] + tileGraphicIndex * TileSet[+0xF4]   ; 8bpp indices, 32x32
    palette     = TileSet[+0x2C] + lightLevel*0x200                    ; 16bpp 555 R-high LUT
    emit RGB per pixel
```

**Playable area:** maps narrower than 512 tiles are stored at double width with 32 unused columns
on the left (and the rest on the right), so they don't wrap; the engine's clip rect covers only the
real columns (x 32 .. width+31). Those columns are all that is rendered, so the padding never
appears. 512-wide maps wrap, have no x clip, and render in full.

**Light level:** the per-tileset LUT array runs `0 .. numLightLevels` (`TileSet[+0x20]-1`, = 32).
Level **0 is full daylight (true colours)**; higher index = darker (the engine's `lightByte` is
really a *darkness* index). The **last** level (32) is a special "fully-lit" LUT that washes
everything pale. So the map renders at level 0 (override with `MapLight`). The back-buffer is a
plain `BI_RGB` 16bpp DIB (AllocSurface @ 0x4a9170), i.e. 555 R-high, which is why the LUT entries
are read R=bits14-10, G=9-5, B=4-0.

**Output:** the image is the clip rect's tiles x `32/MapScale` px each, streamed to a 24bpp BMP one
tile row at a time, so even a 512x256-tile map (16384x8192 px at full size) renders with only a
one-scanline buffer in RAM. It runs synchronously on the game thread, so it's a sim-consistent
snapshot (brief freeze on big maps). Terrain only - no units/fog.

### Files and errors
If any write fails (e.g. disk full) the cut-off file is deleted and the log says so, rather than
leaving a broken `.bmp`. New files are created with `CREATE_NEW`, so an existing screenshot is never
overwritten.

## Notes / tuning
- **Output is BMP** (no encoder dependency); convert to PNG afterwards if needed.
- **Viewport identification** is by source-surface size. If F12 grabs the wrong pane (e.g. a wide
  message log) or nothing, adjust `MinWidth`/`MinHeight` - the main view is the largest surface.
- **Hotkeys** are any virtual-key code via `HotKey`/`WindowKey`/`MapKey`, in decimal
  (e.g. `44` = PrintScreen, `119` = F8).
- **Big maps:** set `MapScale = 4` (8 px/tile) for a manageable overview; full size (`MapScale = 1`)
  of a 512x256-tile map is a 384 MB BMP.
- **Map colours look off?** First check `MapLight` (0 = full daylight). To re-derive the tile/palette
  layout on a different game version, set `MapDiag = 1` and press F10 - it logs the centre tile's
  tileset/graphic indices, the `TileSet` struct fields, raw tile bytes and the palette entry at
  several light levels (this is how the format above was pinned down).
- **Full map is terrain only** (no units/fog). The live state still shows, because tubes/walls/dozed/
  lava are baked into each cell's `tileMappingIndex`. Overlaying units is a possible future layer.

## Files
- `op2shot.c` - the module (full-window grab + IAT BitBlt viewport hook + in-memory full-map renderer
  + keyboard hook + BMP writer), fully commented.
- `op2shot.def` - op2ext exports.
- `build.bat` - 32-bit MSVC build; finds Visual Studio via `vswhere`, links the shared
  `..\modver.rc` version block.
