# Rally

An **op2ext module** for Outpost 2 **OPU 1.4.1** that adds **factory rally points**: a vehicle
that rolls out of one of your Vehicle or Arachnid Factories drives to a rally point instead of
sitting at the factory.

There are two rally points:

- **Military** - combat vehicles: Lynx, Panther, Tiger, Scorpion.
- **Colony** - everything else: ConVec, Cargo Truck, Robo-Miner, Robo-Dozer, Scout, Spider, ...

A group whose point isn't set stays at the factory, as in the unmodded game.

[`ui\`](ui/README.md) is a small window that shows where the two rally points are,
live next to the game.

## Setting rally points in game

1. **Select** a Vehicle or Arachnid Factory.
2. **Point the mouse** at the tile you want (the status bar shows its number).
3. Press **`[`** for the **military** point or **`]`** for the **colony** point.

You get a message in the message log ("Military rally point set: 59, 7") and a marker on the
tile for about 200 game ticks (`Marker` in the ini).

**Seeing the points:** while a Vehicle or Arachnid Factory is selected, each rally point that is
set gets a marker on its current tile, so you can see where that factory's vehicles will go. The
markers go away shortly after you deselect it. The keys do nothing while the mouse is
over the side panel or the game window doesn't have focus.

Points set in game last until the game ends. Each new game, or loaded save, starts again from
the ini values (usually none - every map is different).

## How it works

1. **Hooks the game cycle.** `TethysGame::ProcessGameCycle` (`0x00489EA0`) is called from one
   place (`0x0049C4BD`). The module points that call at its own function, which calls what was
   there before (the engine, or another module's hook) and then runs its passes.
2. **Spots new vehicles.** Each cycle it lists the local player's vehicles. One that wasn't
   listed the cycle before **and** stands within 5 tiles of one of your Vehicle/Arachnid
   Factories was just built. Vehicles that appear anywhere else - mission reinforcements, a
   loaded save, a unit leaving a garage - are left alone, and so is everything before game
   tick 100 (a new game's own units).
3. **Moves it.** `Unit::DoMove` (`0x00476240`) to its group's rally tile - a normal move order
   through the game's command queue.
4. **Notices a new game.** When the game tick goes back or jumps ahead (new game, loaded save),
   the rally points go back to the ini values and the vehicles already there are taken as they
   are.

The engine's tile numbers are offset from the status bar's by (+31 X, -1 Y). The ini, the
messages, the log and the UI use the status bar's numbers.

## UI feed (Rally-UI)

Every 4 game cycles the module writes a snapshot into the named shared memory
`Local\OP2Rally`: both rally points (and whether they came from the ini or the game), the
selected factory, your Vehicle/Arachnid Factories, how many vehicles were sent per group, and
the last 16 vehicles sent with where they are now. Rally-UI shows only the two points and the
status line; the rest is there for other readers.

- The layout is in [`rl_shared.h`](rl_shared.h), included by both the DLL and the viewer, with a
  version number so a mismatched pair warns instead of showing garbage.
- A counter (`seq`) is odd while the game is writing; the reader only keeps a copy taken while it
  was even and unchanged. The game never waits on the reader.
- It only reads game state. If the mapping can't be created the module works as before.

## Install (OPU 1.4.1)

1. Copy `Rally.dll` into the `OPU` folder.
2. In `OPU\outpost2.ini`:
   ```ini
   [ExternalModules]
   Rally=yes

   [Rally]
   Dll=Rally.dll
   Enabled=1
   Marker=1
   ```
   - `Rally=yes` - OPU's loader only accepts `yes`/`no` here.
   - `Enabled` - `1` on, `0` = the DLL loads but patches nothing.
   - `Marker` - the confirmation marker: `0` none, `1` circle (default), `2` DNA, `3` beaker.
     Use `0` in multiplayer (see Limits).
   - `RallyMilitaryX` / `RallyMilitaryY`, `RallyColonyX` / `RallyColonyY` - optional starting
     points, as the status bar shows them. Not set = no point until you set one in game.
   - `Debug` - `1` also logs selection changes and every new vehicle it left alone (and why).
   - Settings are read once at startup; restart the game after changing them.
3. **BuildQueue / AutoRepair:** they patch the same game-cycle call. All three chain, so
   they run together in any load order.

## Log

`OPU\logs\Rally.log`, each line with the time and game tick: startup settings, the hook,
the UI feed, `new game`, `set` (each rally point set), `rally:` (each vehicle sent, from which
factory, to where) and any `*** FAULT`. A fault in a pass is logged and caught, so the game
keeps running; after 10 the module stops for the session.

## Build

```
build.bat
```
Finds Visual Studio via `vswhere` and produces `Rally.dll` (32-bit).

## Files

| File | |
|---|---|
| `rally.cpp` | The module (fully commented). |
| `rl_shared.h` | Shared-memory snapshot layout, shared with Rally-UI. |
| `build.bat` | Build script. |
| `Rally.dll` | Built module. |
| `MP_PLAN.md` | Notes on making it multiplayer-safe. |

## Key addresses (OPU 1.4.1)

| Address | What |
|---|---|
| `0x00489EA0` / `0x0049C4BD` | `ProcessGameCycle` / its one call site (the hook) |
| `0x00477F10` | `TethysGame::LocalPlayer` |
| `0x0047A480` / `0x0047A4B0` | `PlayerVehicleEnum` ctor / `GetNext` |
| `0x0047A4E0` / `0x0047A510` | `PlayerBuildingEnum` ctor / `GetNext` (finding the factories) |
| `0x00475E20` | `Unit::Location` |
| `0x00476240` | `Unit::DoMove(LOCATION)` (the rally move) |
| `0x00475DD0` / `0x00475E60` / `0x00475EC0` / `0x00475EE0` / `0x00475F20` | `Unit::IsLive` / `IsBusy` / `OwnerID` / `GetType` / `GetWeapon` |
| `0x00547210` / `0x00547214` | selected-unit count / unit numbers |
| `0x0057B7AC` / `0x0057B7B4` / `0x0057B7B8` | mouse over the map / map pixel X / Y under the mouse |
| `0x00478BB0` / `0x004769F0` | `TethysGame::PlaceMarker` / `Unit::DoDeath` (marker) |
| `0x00439070` (ECX = `0x0054FCF0`) | `MessageLog::AddMessage` (the in-game message) |
| `0x0054F808` / `0x0054F810` / `0x0056EB1C` | map width / height in tiles / game tick |

## Limits / ideas

- **Multiplayer:** the moves are ordinary commands for your own units, so they should stay in
  sync, but this hasn't been tried in a real multiplayer game. The marker creates a real unit on
  one machine only and **will** desync - use `Marker=0`.
- On around-the-world (wrapping) maps the status-bar offset may differ; the moves are unaffected,
  but the numbers in messages / the ini could be off. Not checked.
- Kept simple on purpose: one military and one colony point, shared by every factory.
- Tested in game on OPU 1.4.1 (2026-11-26): Lynx (military), Spider and Earthworker (colony)
  rallied from a Vehicle and an Arachnid Factory, the markers on selecting a factory, together
  with BuildQueue and AutoRepair. Not yet tried: a loaded save, wrapping maps, multiplayer.
- Idea: setting the points from Rally-UI.
