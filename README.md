# OP2Mods

[![Build](https://github.com/leviathan400/OP2Mods/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/leviathan400/OP2Mods/actions/workflows/build.yml)

Some mods for Outpost 2: Divided Destiny

## Mods

| Mod | What it does |
|---|---|
| [OP2Launcher](OP2Launcher/) | Boots Outpost 2 (OPU 1.4.1) straight into any mission, scenario or colony DLL, or a saved game, with no menu navigation. An op2ext module (`Launcher.dll`) plus a command-line front-end (`op2launcher.exe`, Windows and Linux/Wine). |
| [Zoom](Zoom/) | A wider main-view zoom level: zoom all the way out and the map view shows up to 2x more map (less on small maps, which it caps automatically), with the side panels untouched. An op2ext module (`Zoom.dll`) for OPU 1.4.1; render/UI-only, so multiplayer-safe. |
| [UIExt](UIExt/) | Double-tap a control-group key to centre the camera on that group, and double-click one of your vehicles to select all of that type on screen. A rebuild of TH300's UIExt mod as an op2ext module (`UIExt.dll`) for OPU 1.4.1, plus our double-click select; camera and selection only, so multiplayer-safe. |
| [Screenshot](Screenshot/) | Saves a `.bmp` on a hotkey: F12 the game view, F11 the whole window, F10 the whole playable map rendered from memory (live terrain, any size). An op2ext module (`op2shot.dll`) for OPU 1.4.1; read-only, so multiplayer-safe. |
| [MapSize](MapSize/) | Lets Outpost 2 run maps up to 1024 x 1024 tiles (stock: 512 x 256): fixes the parts of the engine sized for 512 x 256 (tube/power connections, the route planner's corridor, Blight and lava spread, and waypoints on maps taller than 512). Wide maps can wrap round or have hard edges, with the view stopping at the last column. An op2ext module (`MapSize.dll`) for OPU 1.4.1, plus a script that builds a big test map from a stock one. Stock-size maps play exactly as without it; on a big map every player needs it. |

[`modver.rc`](modver.rc) is the shared version and branding resource that the mod DLLs link in.
