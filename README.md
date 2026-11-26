# OP2Mods

[![Build](https://github.com/leviathan400/OP2Mods/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/leviathan400/OP2Mods/actions/workflows/build.yml)

Some mods for Outpost 2: Divided Destiny

## Mods

| Mod | What it does |
|---|---|
| [OP2Launcher](OP2Launcher/) | Boots Outpost 2 (OPU 1.4.1) straight into any mission, scenario or colony DLL, or a saved game, with no menu navigation. An op2ext module (`Launcher.dll`) plus a command-line front-end (`op2launcher.exe`, Windows and Linux/Wine). |
| [Zoom](Zoom/) | A wider main-view zoom level: zoom all the way out and the map view shows up to 2x more map (less on small maps, which it caps automatically), with the side panels untouched. An op2ext module (`Zoom.dll`) for OPU 1.4.1; render/UI-only, so multiplayer-safe. |
| [UIExt](UIExt/) | Double-tap a control-group key to centre the camera on that group. A rebuild of TH300's UIExt mod as an op2ext module (`UIExt.dll`) for OPU 1.4.1; camera-only, so multiplayer-safe. |
| [Screenshot](Screenshot/) | Saves a `.bmp` on a hotkey: F12 the game view, F11 the whole window, F10 the whole playable map rendered from memory (live terrain, any size). Images come out at their final size, with no edges to crop. An op2ext module (`op2shot.dll`) for OPU 1.4.1; read-only, so multiplayer-safe. |

[`modver.rc`](modver.rc) is the shared version and branding resource that the mod DLLs link in.
