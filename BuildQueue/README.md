# BuildQueue

An **op2ext module** for Outpost 2 **OPU 1.4.1** that adds **factory batches**.

- **BUILD IT** -> 1 unit, as in the unmodded game.
- **Shift + BUILD IT** -> `BatchCount` of that unit (default 5), built one after another with
  no more clicks.

Works at **Vehicle**, **Arachnid** and **Structure Factories**, each switched on or off in the ini;
by default Vehicle and Arachnid Factories only.

Tested in game on OPU 1.4.1 (2026-11-26): Shift + BUILD IT at a Vehicle Factory built 5
Robo-Surveyors in a row, and BuildQueue-UI showed a Scout batch counting down live. With the
per-factory switches (defaults), a Vehicle Factory batch of 5 Microwave Lynx. Not yet tried:
Arachnid Factory batches, and Shift + BUILD IT at a switched-off Structure Factory.

Single player / skirmish only - not for multiplayer (see Limits).

## How it works (capture and replay)

A factory build is a `ctMoProduce` command (type `9`, data `{u16 factory, u16 item,
u16 weapon, s16 unused}`). The game has no player build queue: a busy factory takes no new
order. So the module:

1. **Detours `Player::ProcessCommandPacket`** (`0x0040E300`), which every command the player
   issues goes through. When the **local player** issues a `ctMoProduce` while **Shift** is held,
   it copies the raw command packet and arms a counter of `BatchCount - 1` more builds for that
   factory. AI orders (same function) are ignored.
2. **Hooks `TethysGame::ProcessGameCycle`** (its one call site, `0x0049C4BD`). After each game
   cycle, for every armed factory that is idle (`Unit::IsBusy == 0`) it sends the saved packet
   again through the original `ProcessCommandPacket` - the same path the UI uses - and counts
   one down.

Replaying the game's own packet means the command is never built by hand. A batch is dropped
when its factory is destroyed, when the factory never starts a build (no ore, etc.) within
600 cycles, and all batches are cleared when a new game starts or a save is loaded.

The UI takes no new build order while a factory is building, and the next order of a batch goes
in the cycle the factory turns idle, so a batch runs to the end.

Which factories batch is up to the ini (`VehicleFactory`, `ArachnidFactory`, `StructureFactory`).
A build order is the same command at all three, so the mechanism is identical. At a kind that is
switched off - and at any other building that takes build orders, like a Spaceport - Shift +
BUILD IT builds one, as in the unmodded game (the log notes a `skip:`).

## UI feed (BuildQueue-UI)

Every 4 game cycles the module also writes a snapshot of your factories - which kinds have
batches switched on, how many of each kind,
busy/idle, the last unit you ordered at each, and batch progress - into the named shared memory
`Local\OP2BuildQueue`. **BuildQueue-UI** ([`ui\`](ui/README.md)) is a small Dear ImGui window
that reads it and shows it live next to the game.

- The layout is in [`bq_shared.h`](bq_shared.h), included by both the DLL and the viewer, with a
  version number so a mismatched pair warns instead of showing garbage.
- It also carries the game's process id, so the viewer can close itself when the game exits.
- A counter (`seq`) is odd while the game is writing; the reader only keeps a copy taken while it
  was even and unchanged, so it never sees a half-written snapshot. The game never waits on the
  reader.
- Read-only towards the game: the snapshot only reads game state through the exported
  `Unit` / `PlayerBuildingEnum` methods. If the mapping can't be created the module works as
  before, without the feed.

Safety: both patches check the game's bytes first and are skipped (with a log line) if they
aren't what the module expects; the per-cycle code runs behind an exception guard, so a fault
is logged and the module stops instead of crashing the game.

## Install (OPU 1.4.1)

1. Copy `BuildQueue.dll` into the `OPU` folder.
2. In `OPU\outpost2.ini`:
   ```ini
   [ExternalModules]
   BuildQueue=yes

   [BuildQueue]
   Dll=BuildQueue.dll
   Enabled=1
   BatchCount=5
   VehicleFactory=1
   ArachnidFactory=1
   StructureFactory=0
   ```
   - `BuildQueue=yes` - OPU's loader only accepts `yes`/`no` here.
   - `Enabled` - `1` on, `0` = the DLL loads but patches nothing.
   - `BatchCount` - units built by Shift + BUILD IT (1-99; 1 = batches off).
   - `VehicleFactory` / `ArachnidFactory` / `StructureFactory` - `1` = Shift batches at that kind
     of factory, `0` = it builds one. Defaults `1` / `1` / `0` (a missing key uses its default).
     BuildQueue-UI lists only the kinds that are on.
   - Settings are read once at startup; restart the game after changing them.
3. **Other modules** that patch the same game-cycle call (`0x0049C4BD`) can run alongside it:
   BuildQueue chains to whatever that call already points at, so modules that do the same run
   together in any load order.

## Log

`OPU\logs\BuildQueue.log`: startup settings, each hook installed, the UI feed (`ui feed:`), then per batch
`batch:` (factory, unit type, count), `build:` (each replayed order, how many left) and
`drop:` if a batch is abandoned, `skip:` for a Shift order at a factory kind that is switched off.

## Build

```
build.bat
```
Finds Visual Studio via `vswhere` and produces `BuildQueue.dll` (32-bit).

## Files

| File | |
|---|---|
| `buildqueue.cpp` | The module (fully commented). |
| `bq_shared.h` | Shared-memory snapshot layout, shared with BuildQueue-UI. |
| `build.bat` | Build script. |
| `BuildQueue.dll` | Built module. |
| `ui\` | BuildQueue-UI, the live viewer (its own README, build script and `BuildQueue-UI.exe`). |

## Limits / ideas

- **Multiplayer:** not supported - the Shift state and the pacing are read on one machine.
- **STOP** on a factory mid-batch probably only stops the current unit; the module then sends
  the next one. Not yet tested.
- Ideas: click-to-queue (mix unit types); buttons in BuildQueue-UI that send requests back for
  the module to issue as real build orders.
