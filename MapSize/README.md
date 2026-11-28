# MapSize

An **op2ext module** for Outpost 2 **OPU 1.4.1** that lets the game run **maps up to 1024 x 1024
tiles** - four times the largest stock map in each direction (stock: 512 x 256).

- Maps up to 1024 wide either **wrap round** east-west like the stock 512-wide maps, or are
  **bounded**: hard edges on all four sides, like the small stock maps, so the view scrolls to the
  last column and stops.
- Maps up to 1024 tall. On a map taller than 512 the game's waypoints use a wider layout.
- `tools\make_bigmap.py` builds a big test map from a stock one; `mission\` holds **BigMap**, an
  OP2Lua test mission for it.

Every change is either needed only on a big map or makes no difference on a stock-size one: on maps
up to 512 x 512 the game simulates exactly as it does without the module, so multiplayer games on
stock maps stay in sync with players who don't have it. On a big map every player needs it.

Status: **work in progress** (2026-11-28).
- Tested in game on a bounded 1024 x 512 map: the map loads and plays, the view stops at both
  edges (1,1 and 1008,511), tube power works past column 511, Lynx cross it both ways (far west by
  the long way, not across the edge; to the east edge; south past row 256), Blight spreads.
- 1024 x 1024 (bounded): the map loads, the tall waypoint layout is on, and Lynx ordered to row
  759 (through the game's `Unit::DoMove`, from the module's debug orders) drove past row 512
  (rows 529-531 at tick 8000); a pair ordered to the far south-west corner (99,999) is planned in
  legs and on its way. A Lynx given one mouse order to the far bottom-right corner (1007,1012)
  planned the whole route and drove there; with Zoom on, the view stops at the map's edges.
  Not yet tried: patrols / ore routes / build orders past row 512, saving and loading a tall map.
- Known: OP2Lua missions can't send units past row 511 yet - OP2Lua packs waypoints itself (TethysAPI
  `Waypoint`, y in 14 bits). Orders from the game's own UI and from mission DLLs that call the game's
  `Unit::DoMove` etc. are fine. What OP2Lua needs to change: [OP2LUA.md](OP2LUA.md).
- Known, minor: on a 1024-tall map the distance key the game sorts a group move by
  (`SortUnitsByDistance`, `0x00410300`) can overflow, so the order units spread out in may be off.
  No crash.
- MapSize fixes only what a big map breaks in the game's own route planner (the corridor rows,
  the routes in legs); how good the routes are is the planner's. The stock planner struggles over
  long distances (it follows obstacle edges and can give up short of the goal), and on a big map
  that shows more.

## What limits the map size, and what the module does

The loader never checks the size (`Map.Load` `0x00435DC0`, `Map.AllocateSpaceForMap`
`0x004354F0`): the width is a power of two, the height is rounded up to one. The limits are in the
parts sized for 512 x 256:

| Limit | Where | What breaks on a big map | MapSize |
|---|---|---|---|
| Light tables (per column) | Map `+0x5C` / `+0x25C`, 512 bytes each | over 512 wide: the fill runs past them into the map's tile-data pointer - crash on load | Nothing: **OPU 1.4.0 already doubled them** ("allowing a potential map size of 1024x512"). The module checks they are in place. |
| Tube / power flood fill | visited bitmap `0x0054AFB8`, 512 x 256 bits | columns past 511 alias the next row (wrong power); rows past 255 overwrite what follows | Moved into the module, 1024 x 1024 bits: the base in its 14 instructions, the row stride (`shl 6`->`7`, `shl 4`->`5`) and the clear (`0x1000`->`0x8000` dwords). |
| Corridor A\* rows | `SearchStraightCorridor` `0x00440770` / `RefineCoarseSteps` `0x00440D60`: a 256-row table on the stack | rows past 255 overwrite the search's node pool | Rows indexed by `row & 0xFF` (a corridor spans ~50 rows at most, so no two collide; on maps up to 256 tall that is the row itself). Four small stubs + the numbering loop bounded at 256. |
| Blight / lava spread | `BlightOrLava.Spread` `0x0049DCF0`: a 17-bit LFSR (taps `0x12000`) | over 2^17 tiles most tiles can never be picked | Over 2^17 tiles: taps `0x90000` (x^20 + x^17 + 1, maximal). Stock taps otherwise. |
| Wrap-around | only maps narrower than 512 have edges (stored at double width with a 32-tile pad) | a 1024-wide map would need 2048 columns in memory: waypoints, light tables and 16-bit pixel fields can't hold that | **Bounded** wide maps instead: see below. |
| Waypoints | packed dword: pixel x 15 bits, pixel y 14 bits, tolerance 3 bits | over 512 tall: orders to the lower rows land 512 rows up | On maps taller than 512: pixel y 15 bits (15-29), tolerance 2 bits (30-31). The 52 instructions that pack or unpack it are switched as the map is made. |

### Bounded wide maps

A map 512 or more wide whose file sets an x clip rect (`x1 >= 0`) has hard edges:

- **Map file**: the clip rect keeps everything on the playable columns - vehicles never step out
  of it (`Unit:Vehicle.StartNextTileStep`) and the planner keeps to it
  (`PathFinder.PlanCoarsePath`). `make_bigmap.py` makes the last 16 columns impassable rock outside
  the clip, so Blight, weapons and sensors never reach across the seam.
- **View**: `IWnd:Pane:Detail.ClampViewPos` (`0x00407990`) wraps the view on every map without a
  pad. The module replaces it with the same function plus this case: the view is clamped to the
  clip rect.
- **Long routes**: the pathfinder measures every east-west distance the short way round, so on a
  1024-wide map a goal more than 512 columns away looks like it is the other way, across the
  seam. `PathFinder.PlanPath` (`0x00440300`) is wrapped: a move of 448 columns or more is planned
  to a leg goal 384 tiles along the true direction, and the vehicle plans the next leg when it
  gets there.

### Waypoints on tall maps

| Layout | pixel x | pixel y | tolerance |
|---|---|---|---|
| stock (maps up to 512 tall) | bits 0-14 | bits 15-28 | bits 29-31 (0-7) |
| tall (maps over 512 tall) | bits 0-14 | bits 15-29 | bits 30-31 (0-3) |

The tolerance is the "close enough" box for passing a waypoint on the way. The game writes 0 or 3,
except for an attack / guard target, which carries the weapon range - but that is always the
order's last waypoint, which uses the move state's own range field instead, so it is capped at 3
with no effect. The layout is switched when each map is allocated (a hook in
`AllocateSpaceForMap` at `0x00435728`), before the mission or a saved game places anything, so
orders, network packets and saved games on stock-size maps are exactly as stock.

## Install (OPU 1.4.1)

1. Copy `MapSize.dll` into the `OPU` folder.
2. In `OPU\outpost2.ini`:
   ```ini
   [ExternalModules]
   MapSize=yes

   [MapSize]
   Dll=MapSize.dll
   Enabled=1
   Debug=0
   DebugEvery=200
   ```
   - `Enabled` - `1` on, `0` = the DLL loads but patches nothing.
   - `Debug` - `1` = log every vehicle every `DebugEvery` ticks, and run test orders from
     `OPU\logs\MapSize-cmd.txt` (lines `move <unit index> <x> <y>`, `moveall <x> <y>`, `log`; tiles are
     the game's own, 0-based). Orders go through the game's `Unit::DoMove`.
3. Log: `OPU\logs\MapSize.log` - the patches installed and each map's size, edges, spread taps and
   waypoint layout.

Every patch checks the bytes it replaces first and is left out, with a log line, if they aren't
what it expects.

## Making a big map

```
python tools\make_bigmap.py <source.map> <out.map> [width] [height] [bounded|wrap]
```

Repeats a stock map across (a 512-wide source wraps, so the seams match) and mirrors its inner
rows down, keeping its top and bottom edges. Maps wider than 512 are bounded by default. Everything
after the tile grid (tilesets, terrain types) is copied, so the map uses the source's tilesets. Made
with the source's own size it writes the source back byte for byte.

## Test mission (BigMap)

`mission\` - an OP2Lua mission on `bigmap.map` (put both in `OPU\maps\BigMap\`; needs
`OP2LuaCore.dll`). A base at column 800 (Command Center, Tokamak, Structure Factory, joined by
tubes: power past column 511), Lynx sent far west, to the east edge and south, and Blight at column
600. `OPU\logs\OP2Lua-cBigMap.log` logs power and progress every 5 marks. Start it with
`op2launcher.exe cBigMap.dll` from the `OPU` folder.

## Build

`build.bat` (Visual Studio C++ x86 tools, found with vswhere) -> `MapSize.dll`.
