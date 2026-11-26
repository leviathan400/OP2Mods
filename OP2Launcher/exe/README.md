# OP2Launcher (`op2launcher.exe`)

A command-line front-end that boots Outpost 2 (OPU 1.4.1) straight into any mission, scenario or
colony DLL with no menu navigation. Use it by hand, or from scripts and agents.

It doesn't touch the game itself. It edits `outpost2.ini` to switch on the **Launcher** op2ext
module (`Launcher.dll`, built from the parent folder) and then starts `OPULauncher.exe`. The module
does the actual launch inside the game. See [`../launcher/README.md`](../launcher/README.md) for how the module works.

Runs on Windows natively and on Linux, where OPU runs under Wine.

## Install

Put these files in the **OPU folder** (e.g. `D:\Outpost 2\OPU`), next to `OPULauncher.exe` and
`outpost2.ini`:

- `op2launcher.exe`
- `Launcher.dll` (the module, from `..\launcher\build.bat`)
- `op2launcher.ini` (optional, sets a default mission)

The launcher finds everything relative to its own folder. On first run it registers the module in
`outpost2.ini` itself: `[ExternalModules] Launcher=yes` plus a default `[Launcher]` section. It never
overwrites a setup that's already there.

## Usage

```
op2launcher.exe <mission.dll>   boot straight into <mission.dll>     e.g. op2launcher.exe e04.dll
op2launcher.exe                 boot into [Launcher] Dll= from op2launcher.ini
op2launcher.exe --picker | -p   boot into the in-game "Launch a DLL" picker
op2launcher.exe --load | -l <save>  boot into a saved game   e.g. op2launcher.exe --load saves\SGAME0.OP2
op2launcher.exe --uninstall | -u  remove Launcher from outpost2.ini (Launcher.dll stays on disk)
op2launcher.exe --help | -h | -? | /?  show the usage
```

The command line is checked **before anything is changed**. An unknown option (anything starting
with `-`, e.g. a typo like `--pick`, or a short `/x`) or an extra argument prints an error and the
usage and exits with code 1. The ini is left alone and Outpost 2 is not started. A mission name
never starts with `-`.

Every launch is **one-shot**. The launcher sets `Enabled=1` and `OneShot=1`, and the module sets
`Enabled=0` again once it has fired. The next normal start of OPU is therefore unmodded, and the
launcher can't be left switched on by accident.

### What it writes to `outpost2.ini`

| Mode | Keys set under `[Launcher]` (plus `[ExternalModules] Launcher=yes`) |
|---|---|
| `<mission.dll>` | `Dll=Launcher.dll` `Enabled=1` `OneShot=1` `Auto=1` `Mission=<mission.dll>` |
| `--picker` | `Dll=Launcher.dll` `Enabled=1` `OneShot=1` `Auto=0` |
| `--load <save>` | `Dll=Launcher.dll` `Enabled=1` `OneShot=1` `Load=<save>` (the module clears `Load` once used) |

Every run first removes any `Load=` left over from a run that never reached the shell, so it can't
override what you asked for this time.

### Saved games

`--load <save>` takes a path relative to the OPU folder (OPU keeps saves in `saves\SGAMEn.OP2`) or an
absolute path. The launcher checks the file exists before changing anything. In-game, the module
loads it the same way the shell's own Load button does. A save made by a Lua mission loads with
its script carrying on under OP2Lua 0.8.8 and later, which stores the script's state in the save.

`--uninstall` removes `[ExternalModules] Launcher` and the whole `[Launcher]` section.

### Mission name limit

Mission DLL names can be at most **31 characters**. The engine keeps the name in
`GameStartInfo.scriptName[32]`, and one of those 32 bytes is the null terminator. It doesn't check
the length when copying the name in, so a longer name would overflow the field. `op2launcher.exe`
refuses a name that's too long, exits with code 1 and leaves the ini alone. The module and the
in-game picker enforce the same limit. Names are rejected rather than shortened, because a
shortened name would load a different DLL or none.

## Config: `op2launcher.ini`

```ini
[Launcher]
; Default mission DLL when op2launcher.exe is run with NO argument.
; A command-line argument always overrides this.
Dll=
```

## Exit codes

| Code | Meaning |
|---|---|
| `0` | OPU was started (or `--uninstall` / `--help` finished) |
| `1` | unknown option, extra argument, usage error, mission name too long, saved game not found, `OPULauncher.exe` not found, or OPU failed to start |

`0` only means OPULauncher was started. It doesn't mean the mission loaded. Check
`<OPU>\logs\Launcher.log` for that. "Could not load script file" in-game means the DLL name was
wrong or the DLL lacks the required exports.

## Building

| Platform | Command | Output |
|---|---|---|
| Windows | `build.bat` | `op2launcher.exe` (x86, static CRT) with the ConVec icon and version info from `launcher.rc` |
| Linux / WSL | `./build.sh` | `op2launcher` (native `g++ -std=c++17`; no icon or version resources) |

`build.bat` finds Visual Studio with `vswhere`. It accepts any installed edition that has the C++
x86 tools, including Build Tools and prerelease or Insiders builds. It **copies the exe to
`D:\Outpost 2\OPU`** if that folder exists. It also copies `op2launcher.ini` there, but only if
there isn't one already.

On Linux the launcher runs `wine OPULauncher.exe` from the OPU folder, so `wine` must be on `PATH`.
Copy `op2launcher` (and optionally `op2launcher.ini`) into the Wine prefix's OPU folder, e.g.
`~/.wine/drive_c/Outpost 2/OPU/`.

## Files

| File | What |
|---|---|
| `op2launcher.cpp` | the whole tool: ini install/uninstall, arg handling, starting OPU (Windows + POSIX) |
| `op2launcher.ini` | default-mission config template |
| `launcher.rc`, `ConVec.ico` | Windows icon and version info (FileDescription `OP2Launcher`) |
| `..\launcher\version.h` | **the version number** (currently 0.9.7), shared with `Launcher.dll`; bump it there |
| `CHANGELOG.md` | release history of `op2launcher.exe` |
| `build.bat`, `build.sh` | Windows / Linux builds |
