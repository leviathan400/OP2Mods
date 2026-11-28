# Rally-UI

A small **Dear ImGui** desktop window that shows where the [Rally](../README.md) module's
two rally points are, **live**, next to Outpost 2 (OPU 1.4.1).

## What it shows

- **Status:** waiting for Outpost 2 / Outpost 2 running, start a game / in game (with the mark -
  the game's clock, 100 ticks - and local player) / paused or not in a game (tick not moving).
- **The two rally points:** where the military and colony points are, as the game's status bar
  numbers them, and whether each was set in game or came from the ini - or "not set".
- **On top:** keeps the window above the game.

## How it gets the data

Rally.dll, inside the game, writes a snapshot every 4 game cycles into the named shared memory
`Local\OP2Rally`. This app opens it **read-only** and copies it about 10 times a second:

1. The layout is in [`..\rl_shared.h`](../rl_shared.h), included by both sides. Its
   version number must match, or the app says to rebuild both.
2. A counter (`seq`) is odd while the game is writing. The app keeps a copy only if `seq` was even
   and unchanged across the copy, so it never shows a half-written snapshot.
3. Nothing goes back to the game, and the game never waits on the app - start and close it any
   time. With no game running it just waits.

## Quits with the game

The snapshot carries the game's process id. Once the app has seen a game it closes itself when
Outpost 2 exits - a normal quit or a crash - or when Rally reports the game shutting down.
Started before the game, it waits for one.

## Use

1. Install Rally in the game (see its README) - the feed is part of `Rally.dll`.
2. Run `Rally-UI.exe` (from anywhere), before or after starting the game.

It can run beside [BuildQueue-UI](../../BuildQueue/ui/README.md); each reads its own module's feed.

## Build

```
build.bat
```
Finds Visual Studio via `vswhere` and builds `Rally-UI.exe` (32-bit like the module, Direct3D 11, static CRT -
no extra DLLs needed).

## Files

| File | |
|---|---|
| `main.cpp` | The app (fully commented). |
| `build.bat` | Build script. |
| `app.rc`, `Tech_Vehicle.ico` | The app icon (window, taskbar and the .exe). |
| `Rally-UI.exe` | Built app. |
| `imgui\` | Dear ImGui 1.91.5 (core + Win32/DX11 backends), MIT licence - `imgui\LICENSE.txt`. |
