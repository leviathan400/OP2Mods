# AutoRepair

An **op2ext module** for Outpost 2 **OPU 1.4.1** that **auto-repairs your damaged units**: every
`Interval` game cycles it sends an idle **ConVec** to each damaged building and an idle **Repair
Vehicle** or **Spider** to each damaged vehicle that nobody is repairing yet. Each kind is switched
on or off in the ini.

[`ui\`](ui/README.md) is a small window that shows the damaged
buildings and vehicles, their health and the ConVec / Repair Vehicle / Spider on each, live next to the
game.

## What it does

- Every `Interval` cycles (default 100 = 1 mark) it lists your buildings and vehicles. One with
  at least `MinDamage` damage needs repair.
- **Buildings** (`ConVec=1`): each gets the **nearest idle, empty ConVec**. A ConVec carrying a
  structure kit, or doing anything else, is never taken.
- **Vehicles** (`RepairVehicle=1`): each vehicle **below 50% health** gets the **nearest idle Repair
  Vehicle**. A Repair Vehicle only repairs a vehicle up to half its hit points, so one at or above
  50% is left alone. A Repair Vehicle never repairs itself, but a damaged one can be repaired by
  another. Because of that 50% limit not everyone will want it - switch it off with
  `RepairVehicle=0`.
- **Spiders** (`Spider=1`): Spiders repair vehicles the same way, under the same 50% rule. They
  share one pool with the Repair Vehicles, so a damaged vehicle gets whichever idle Repair Vehicle
  or Spider is nearest. `Spider=0` leaves your Spiders alone (e.g. to keep them for reprogramming).
- A unit someone is already repairing is left to them - never two workers on one. One worker per
  damaged unit per check.
- How it picks: it goes through the damaged units in the order the game lists them (not worst
  first) and gives each one the nearest worker still free in that check.
- The worker drives there and repairs it, as if you had ordered it.
- If a repair ends without lowering the unit's damage:
  - the worker **got to it** (within 3 tiles) - the game refused the repair: a building with only a
    little damage (seen in testing: a Tokamak at 1 damage stayed at 1), or **not enough ore** (a
    Tiger needs rare ore; each refusal shows "Repairs incomplete"). The unit is left alone for
    **10 marks** (building) or **1 mark** (vehicle - its repair may just have been cut short by a
    fight) instead of sending workers to it every check;
  - the worker **never got there** (you moved the unit, or gave the worker other orders) - one is
    sent again at the next check, to wherever the unit is now.

Damaged vehicles moving around (e.g. in a fight) get chased by their Repair Vehicle or Spider.

## How it works

1. **Hooks the game cycle.** `TethysGame::ProcessGameCycle` (`0x00489EA0`) is called from one
   place (`0x0049C4BD`). The module points that call at its own function, which calls what was
   there before (the engine, or another module's hook) and then runs its pass.
2. **Buildings:** `PlayerBuildingEnum(player, 0)` (all buildings). Damage is the `short` at
   `Unit+0x1E`; hit points = max HP - damage. Max HP is the unit type's stats for the player who
   built it: type table `0x004E1348[map_id]`, then `+0x08 + creator * 0x44` (creator = high 4 bits
   of `Unit+0x1D`).
3. **Vehicles:** `PlayerVehicleEnum(player)` - the damaged ones (same damage and max HP as above;
   only below 50% count) and the workers. Idle = `Unit::IsBusy() == 0` (for a ConVec also
   `GetCargo() == 0`). A ConVec, Repair Vehicle or Spider whose command (`Unit+0x21`) is `ctMoRepairObj`
   (`0x1C`) is repairing the unit at `Unit+0x48`.
4. **The order:** a `ctMoRepairObj` command packet sent through `Player::ProcessCommandPacket`
   (`0x0040E300`) for the worker's owner - the path the game's own orders take. The same command
   works for a ConVec on a building and a Repair Vehicle or Spider on a vehicle. Layout from the mission
   SDK's HFL `UnitEx::DoRepair`:
   ```
   CommandPacket { int type=0x1C; short dataLength=9; int timeStamp; int unknown; cmdRepair data }
   cmdRepair     { char numUnits=1; short unitId=worker; short unknown1=0; short unitToRepair=target;
                   short unknown2=0xFFFF }
   ProcessCommandPacket(&playerArray[owner], /*edx*/0, &packet)   // __thiscall, ECX = the Player
   ```
5. **New game / loaded save** (the game tick goes back or jumps ahead): the per-game counters, the
   retry bookkeeping and the one-time building list start again.

Notes:
- `0x0054F848` holds a **pointer** to the unit pool; a unit's record is
  `*(void**)0x0054F848 + index * 0x78`.
- `ProcessCommandPacket` is `__thiscall` (Player in ECX); calling it as `__stdcall` crashes.

## UI feed (AutoRepair-UI)

Every 4 game cycles the module writes a snapshot into the named shared memory
`Local\OP2AutoRepair`: the three switches, your ConVecs, Repair Vehicles and Spiders (total /
idle / repairing), repairs sent this game, cycles to the next check, and up to 32 damaged units of the
switched-on kinds (worst first) with damage, max HP, tile, the worker repairing each, and the time
until a left-alone unit is tried again.

- The layout is in [`ar_shared.h`](ar_shared.h), included by both the DLL and the viewer, with a
  version number so a mismatched pair warns instead of showing garbage.
- A counter (`seq`) is odd while the game is writing; the reader only keeps a copy taken while it
  was even and unchanged. The game never waits on the reader.
- It only reads game state. If the mapping can't be created the module works as before.

## Install (OPU 1.4.1)

1. Copy `AutoRepair.dll` into the `OPU` folder.
2. In `OPU\outpost2.ini`:
   ```ini
   [ExternalModules]
   AutoRepair=yes

   [AutoRepair]
   Dll=AutoRepair.dll
   Enabled=1
   ConVec=1
   RepairVehicle=1
   Spider=1
   Interval=100
   MinDamage=50
   DebugDamage=0
   ```
   - `AutoRepair=yes` - OPU's loader only accepts `yes`/`no` here.
   - `Enabled` - `1` on, `0` = the DLL loads but patches nothing.
   - `ConVec` - `1` = ConVecs repair damaged buildings, `0` = off.
   - `RepairVehicle` - `1` = Repair Vehicles repair vehicles below 50% health, `0` = off.
   - `Spider` - `1` = Spiders repair vehicles below 50% health too, `0` = off.
   - `Interval` - game cycles between checks (100 = 1 mark).
   - `MinDamage` - repair a unit once it has at least this much damage (default 50). Tokamaks
     decay a little all the time; a low value keeps a ConVec busy topping them up.
   - `DebugDamage` - test only: if above 0, once per game one Tokamak gets this much damage, so
     you can watch a repair without a fight. A direct write - single player only; keep it `0`.
   - Settings are read once at startup; restart the game after changing them.
3. **Other modules** that patch the same game-cycle call (BuildQueue, for one) chain the same way,
   so they run together in any load order.

## Log

`OPU\logs\AutoRepair.log`, each line with the time and game tick: startup settings, the hook,
the UI feed, the full building list once per game (damage / max HP), then a line per repair sent
(`repair: ConVec #x -> Tokamak #y (damage, ~tiles away)`, `repair: Repair Vehicle #x -> Lynx #y`, `repair: Spider #x -> ...`),
`skip:` when a worker reached a unit but couldn't help (and how long it waits), `resend:` when the
worker never got there, a summary for each check that found damage, and any `*** FAULT`. A fault is logged and caught, so the game keeps running; after 10
the module stops for the session.

## Build

```
build.bat
```
Finds Visual Studio via `vswhere` and produces `AutoRepair.dll` (32-bit).

## Files

| File | |
|---|---|
| `autorepair.cpp` | The module (fully commented). |
| `ar_shared.h` | Shared-memory snapshot layout, shared with AutoRepair-UI. |
| `build.bat` | Build script (vswhere, x86). |
| `AutoRepair.dll` | Built module. |

## Limits / ideas

- **Multiplayer:** the orders are ordinary commands for your own ConVecs, Repair Vehicles and Spiders, so
  they should stay in sync, but this hasn't been tried in a real multiplayer game. `DebugDamage`
  must be `0` there.
- A worker anywhere on the map can be sent, however far away. A Repair Vehicle chasing a vehicle
  that keeps moving or fighting can take a while to catch it.
- Tested in game on OPU 1.4.1 (2026-11-27): ConVecs repairing Tokamaks, Repair Vehicles repairing
  Lynx (EMP, Rail Gun) below 50%, the resend after you move a unit away, all alongside
  BuildQueue. Spiders repairing Lynx, Tigers and Panthers, and the 1-mark wait while out of rare
  ore (2026-11-27).
- Ideas: a maximum distance; worst-damaged first; repair only after a chosen research.
