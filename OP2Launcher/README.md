# OP2Launcher

Boots Outpost 2 (OPU 1.4.1) straight into **any mission, scenario or colony DLL**, or a **saved
game**, with no menu navigation and no keystroke automation. Use it by hand, from scripts, or from
agents.

It replaces the old keystroke-driven debug menu, which could only start what it listed.

Current version: **0.9.6** (set in [`launcher/version.h`](launcher/version.h), shared by both parts).

## Two parts

| Folder | Builds | What it is |
|---|---|---|
| [`launcher/`](launcher/) | `Launcher.dll` | The **op2ext module** that runs inside Outpost 2. Once the shell window is up, it starts the configured mission or save, or shows a picker, using the engine's own launch path. |
| [`exe/`](exe/) | `op2launcher.exe` | The **command-line front-end**. It registers the module in `outpost2.ini`, sets what to launch, and starts `OPULauncher.exe`. Runs on Windows and on Linux under Wine. |

The two only talk through `outpost2.ini`. The exe writes the `[Launcher]` section and the module
reads it at startup. Every launch is one-shot: the module sets `Enabled=0` again once it has fired,
so the next normal start of OPU is unmodded.

## Quick start

Copy `Launcher.dll` and `op2launcher.exe` into the OPU folder (e.g. `D:\Outpost 2\OPU`), next to
`OPULauncher.exe`, then:

```
op2launcher.exe e04.dll                     boot straight into a mission DLL
op2launcher.exe --picker                    boot into the in-game "Launch a DLL" picker
op2launcher.exe --load saves\SGAME0.OP2     boot into a saved game
op2launcher.exe --uninstall                 remove Launcher from outpost2.ini
```

The first run registers the module in `outpost2.ini` by itself. Mission DLL names can be at most
31 characters, the size of the engine's `scriptName` field. Lua-mission saves keep their script
running under OP2Lua 0.8.8 and later.

## Building

| Part | Command | Notes |
|---|---|---|
| `Launcher.dll` | `launcher\build.bat` | x86, static CRT |
| `op2launcher.exe` | `exe\build.bat` | x86, with the ConVec icon and version info |
| `op2launcher` (Linux) | `exe/build.sh` | native `g++`; starts OPU with `wine` |

Both `build.bat` scripts find Visual Studio through `vswhere` (any edition with the C++ x86 tools,
including Build Tools and Insiders). They **copy the result into `D:\Outpost 2\OPU`** if that folder
exists.

To release a new version, change the number in `launcher/version.h` and rebuild both parts.

## More detail

| Doc | Covers |
|---|---|
| [`launcher/README.md`](launcher/README.md) | how the module works: engine addresses, the launch and load sequences, ini keys, the picker and its OP2 skin |
| [`launcher/CHANGELOG.md`](launcher/CHANGELOG.md) | `Launcher.dll` release history |
| [`exe/README.md`](exe/README.md) | `op2launcher.exe` usage, what each mode writes to the ini, exit codes |
| [`exe/CHANGELOG.md`](exe/CHANGELOG.md) | `op2launcher.exe` release history |
