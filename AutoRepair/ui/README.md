# AutoRepair-UI

A small **Dear ImGui** desktop window that shows what the [AutoRepair](../README.md)
module sees - your damaged buildings and vehicles and the ConVecs / Repair Vehicles / Spiders repairing
them - **live**, next to
Outpost 2 (OPU 1.4.1).

## What it shows

- **Status:** waiting for Outpost 2 / Outpost 2 running, start a game / in game (with the mark -
  the game's clock, 100 ticks - and local player) / paused or not in a game.
- **ConVecs**, **Repair Vehicles** and **Spiders:** how many you have, how many are idle (free to send) and how
  many are repairing - or "auto-repair off in the ini" for a kind that is switched off.
- **Repairs sent this game**, and the cycles to AutoRepair's next check.
- **Damaged units** of the switched-on kinds (buildings for ConVecs, vehicles below 50% health
  for Repair Vehicles / Spiders), worst first (up to 32): name and unit number, tile, a health bar (hit points / max,
  green - amber - red), and the ConVec / Repair Vehicle / Spider repairing it - or "next check" (an idle
  ConVec will be sent then) / "no idle ConVec" or "no idle repairer" / "retry in N marks" (a worker reached it but its
  damage didn't go down - too little damage, or not enough ore - so AutoRepair waits 10 marks for a
  building, 1 for a vehicle).
- **On top:** keeps the window above the game.

## How it gets the data

AutoRepair.dll, inside the game, writes a snapshot every 4 game cycles into the named shared
memory `Local\OP2AutoRepair`. This app opens it **read-only** and copies it about 10 times a
second:

1. The layout is in [`..\ar_shared.h`](../ar_shared.h), included by both
   sides. Its version number must match, or the app says to rebuild both.
2. A counter (`seq`) is odd while the game is writing. The app keeps a copy only if `seq` was even
   and unchanged across the copy, so it never shows a half-written snapshot.
3. Nothing goes back to the game, and the game never waits on the app - start and close it any
   time. With no game running it just waits.

## Quits with the game

The snapshot carries the game's process id. Once the app has seen a game it closes itself when
Outpost 2 exits - a normal quit or a crash - or when AutoRepair reports the game shutting down.
Started before the game, it waits for one.

## Use

1. Install AutoRepair in the game (see its README) - the feed is part of `AutoRepair.dll`.
2. Run `AutoRepair-UI.exe` (from anywhere), before or after starting the game.

It can run beside BuildQueue-UI; each reads its own module's feed.

## Build

```
build.bat
```
Finds Visual Studio via `vswhere` and builds `AutoRepair-UI.exe` (32-bit like the module, Direct3D 11, static
CRT - no extra DLLs needed).

## Files

| File | |
|---|---|
| `main.cpp` | The app (fully commented). |
| `build.bat` | Build script. |
| `app.rc`, `Tech_Vehicle.ico` | The app icon (window, taskbar and the .exe). |
| `AutoRepair-UI.exe` | Built app. |
| `imgui\` | Dear ImGui 1.91.5 (core + Win32/DX11 backends), MIT licence - `imgui\LICENSE.txt`. |
