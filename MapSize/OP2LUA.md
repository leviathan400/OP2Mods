# OP2Lua on maps taller than 512 - what needs changing

Status: **not done yet** (report only, 2026-11-28). Nothing in OP2Lua has been changed.

## The problem

On a map taller than 512 tiles MapSize switches the game to a **tall waypoint layout**:

| Layout | pixel x | pixel y | tolerance |
|---|---|---|---|
| stock (maps up to 512 tall) | bits 0-14 | bits 15-28 (14 bits) | bits 29-31 |
| tall (maps over 512 tall) | bits 0-14 | bits 15-29 (15 bits) | bits 30-31 |

Everything that packs a waypoint *through the game* gets the tall layout: the mouse, the game's
own `Unit::DoMove` / `DoBuild` (the mission SDK functions), AI groups, network orders.

OP2Lua doesn't go through the game for most orders. It builds the command packet itself with the
bundled TethysAPI and passes it straight to `Player::ProcessCommandPacket`, and TethysAPI's
waypoint is a fixed bitfield (`third_party/Tethys/API/Location.h`):

```cpp
union Waypoint {
  struct {
    uint32 x : 15;  ///< In pixels (max = 1024 tiles)
    uint32 y : 14;  ///< In pixels (max = 512 tiles)
    uint32   :  3;
  };
  uint32 u32All;
};
...
constexpr Waypoint AsWaypoint(bool xCentered = true, bool yCentered = true) const
  { return { uint32(GetPixelX(xCentered)), uint32(GetPixelY(yCentered)) }; }
```

So pixel y loses its top bit before the game sees it: a move to row 760 (pixel 24336) is packed
as pixel 7952, which the game reads as row 248. Seen in the BigMap test mission: Lynx sent by
`unit:move` to (790, 760) on the 1024 x 1024 map stopped at about row 251. The same Lynx ordered
with MapSize's debug orders (the game's `Unit::DoMove`) or with the mouse drove to row 760 and to
(1007, 1012).

Maps up to 512 tall are not affected (y fits in 14 bits and the layouts are the same there).

## Where OP2Lua packs waypoints

All in OP2Lua's own copy of TethysAPI, plus one in `src/Bindings.cpp`:

| Lua call (OP2Lua) | C++ path | Packet / field |
|---|---|---|
| `unit:move`, deferred orders (`Order` / `IssueOrderGuarded`) | `Unit::DoMove` (`API/Unit.h` ~line 264) | `Move`, `move.waypoint[0] = where.AsWaypoint()` |
| `unit:dock` | `Unit::DoDock` (~278) | `Dock`, `move.waypoint[0] = d.AsWaypoint()` |
| `unit:dock_at_garage` | `Unit::DoDockAtGarage` (~568) | `DockEG`, `move.waypoint[0] = ...AsWaypoint(true, true)` |
| `unit:stand_ground` | `Unit::DoStandGround` (~582) | `StandGround`, `move.waypoint[0] = where.AsWaypoint()` |
| `unit:build` (ConVec) | `Unit::DoBuild` (~482) | `Build`, `build.waypoint[0] = bottomRight.AsWaypoint(true, false)` |
| `unit:deploy` (Robo-Miner etc.) | `Unit::DoDeploy` (~498) | `Build`, `build.waypoint[0] = center.AsWaypoint(false, false)` |
| `unit:mining_route` | `Unit::DoMiningRoute` (~640) | `CargoRoute`, `cargoRoute.waypoint[0..2] = ...AsWaypoint()` |
| `unit:build_raw` | `BuildRaw` (`src/Bindings.cpp` ~1389) | `Build`, a `Tethys::Waypoint` filled field by field (`w.x = px; w.y = py`) |

(Line numbers as of OP2Lua 0.8.18; search for `AsWaypoint` and `Waypoint w` to find them.)

**Not affected** - these hand the game tile coordinates and the game packs them itself, with the
layout MapSize set:

- `group:set_patrol` (`FightGroup::SetPatrolMode`, a `PatrolRoute` of `Location`s - the game's
  `CombatBase.SetWaypoints` packs them);
- the AI groups (`ai.*`: build groups, mining groups, fight groups) - the game's ScGroup code;
- attacks and guards (`DoAttack`, ...) - a unit target, no waypoint;
- `unit:launch` / `unit:launch_at_pixel` (`LaunchAtPixel`) - pixel x/y as two 16-bit fields, which hold every pixel of a
  1024 x 1024 map (up to 32767).

OP2Lua doesn't read waypoints back anywhere (no use of `u32All`, `waypoints_[]` etc. outside the
packet code), so only the packing needs to change.

## The change

1. **One packing function** that knows the layout, next to `AsWaypoint` in
   `third_party/Tethys/API/Location.h` (or in an OP2Lua header that wraps it):

   ```cpp
   // The waypoint layout the running game decodes: MapSize's tall layout on maps over 512 tall.
   // Ask the game rather than the map height: the decoder in Player.ProcessCommandPacket
   // (`and ecx, 1FFF8000h` at 0x0040E431, imm32 at +2) holds 0x3FFF8000 when MapSize has
   // switched it - so a game without MapSize, or with its waypoint patch left out, keeps the
   // stock layout.
   inline bool TallWaypoints() { return *reinterpret_cast<const uint32*>(0x0040E433) == 0x3FFF8000u; }

   inline Waypoint MakeWaypoint(int px, int py) {
     Waypoint w;
     w.u32All = TallWaypoints() ? (uint32(px) & 0x7FFF) | ((uint32(py) & 0x7FFF) << 15)    // y 15 bits, tolerance 0
                                : (uint32(px) & 0x7FFF) | ((uint32(py) & 0x3FFF) << 15);   // stock: y 14 bits
     return w;
   }
   ```

   `AsWaypoint` becomes `return MakeWaypoint(GetPixelX(xCentered), GetPixelY(yCentered));` (it can't
   stay `constexpr`: it now reads game memory; check nothing uses it in a constant expression).
   The tolerance bits stay 0, as they are now.

   Keying on the game's decoder instead of the map height keeps OP2Lua and the game always in
   step: whatever layout the game reads, OP2Lua writes. (Reading the map height, `0x0054F810`,
   would also work while MapSize is installed and its waypoint patch is in.)

2. **`BuildRaw`** (`src/Bindings.cpp`): `packet.data.build.waypoint[0] = Tethys::MakeWaypoint(px, py);`
   instead of setting `w.x` / `w.y`.

3. Leave the `Waypoint` bitfield as it is (it describes the stock layout and other code may use
   it to read fields); just stop writing `.y` through it.

4. **Build** `OP2LuaCore.dll` as usual. On every map up to 512 tall the packets are byte for byte
   what they were.

## How to test

1. Install the new `OP2LuaCore.dll` in the test install (MapSize on).
2. Run the BigMap mission on the 1024 x 1024 bounded map (`op2launcher.exe cBigMap.dll`):
   the Lynx sent to (790, 760) and to (100, 1000) should get there (`arrive:` lines in
   `logs\OP2Lua-cBigMap.log`; with MapSize `Debug=1`, `logs\MapSize.log` shows each one's move
   state destination as (789, 759) / (99, 999), not row 247).
3. A stock-size mission (PathTest on Nostalgia, or any campaign mission) still plays the same.
4. Optional: try `unit:build`, `unit:deploy`, `unit:dock`, `unit:dock_at_garage`,
   `unit:stand_ground` and `unit:mining_route` past row 512.

## Beyond OP2Lua

The 14-bit `Waypoint` is TethysAPI's, so any other mission DLL built with TethysAPI that sends
its own command packets has the same limit on tall maps; the same fix belongs in TethysAPI
itself. Missions built on the original Outpost2DLL SDK call the game's own functions, which
MapSize already switches, so they work as they are.
