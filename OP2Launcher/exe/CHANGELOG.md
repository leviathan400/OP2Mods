# Changelog - OP2Launcher (op2launcher.exe)

The command-line front-end that sets up `outpost2.ini` and starts OPU. The in-game module has its own
changelog: [`../launcher/CHANGELOG.md`](../launcher/CHANGELOG.md).

The version is set in [`../launcher/version.h`](../launcher/version.h) and shared with `Launcher.dll`.

## [0.9.8] - 2026-11-26

### Changed
- Version 0.9.8, shared with `Launcher.dll` 0.9.8 (whose picker now shows its version and shows the
  mission details sooner).
- `build.bat` reports when it can't copy `op2launcher.exe` into the OPU folder instead of always
  printing "Deployed".

## [0.9.7] - 2026-11-26

### Fixed
- **A wrong argument no longer launches the game.** Before, any first argument that wasn't a known
  option was taken as a mission name, so a typo like `--pick` or `--help` was written to the ini as
  `Mission=--pick` and Outpost 2 started, ending in "Could not load script file". Now an unknown
  option (anything starting with `-`, or a short `/x`) or an extra argument prints an error and the
  usage and exits with code 1, without changing the ini or starting anything.

### Added
- `--help` / `-h` / `-?` / `/?` print the usage (exit code 0). The usage now lists `--load` too.

### Changed
- Version 0.9.7, shared with `Launcher.dll` 0.9.7 (whose picker now lists every mission DLL and
  has a filter box).

## [0.9.6] - 2026-11-22

### Added
- **`--load <save>` / `-l <save>`** boots straight into a saved game, e.g.
  `op2launcher.exe --load saves\SGAME0.OP2`. The path can be relative to the OPU folder or
  absolute.
  - The file is checked before anything is changed. If it's missing, the launcher exits with
    code 1 and leaves the ini alone.
  - It sets `[Launcher] Load=<save>`, and the module clears that once the save is loaded.
- Needs `Launcher.dll` 0.9.6 or later. Older modules ignore `Load=`.

### Changed
- Every run now removes any `Load=` left over from a run that never reached the shell, so an old
  save can't override what you asked for this time.

## [0.9.5] - 2026-11-21

### Fixed
- **Mission names longer than 31 characters are refused** before the ini is touched (exit code 1).
  The engine's `GameStartInfo.scriptName[32]` can't hold more, and `Launcher.dll` enforces the
  same limit.

### Changed
- Version info now reads 0.9.5 (it was 1.0.0.0), taken from `../version.h`.
- `build.bat` finds any Visual Studio install with the C++ x86 tools, including Build Tools and
  prerelease/Insiders builds.

### Added
- `README.md` covering usage, what each mode writes to the ini, exit codes and building.

## [0.9] - 2026-06-25

### Added
- First release of `op2launcher.exe`:
  - `op2launcher <mission.dll>` boots straight into a mission.
  - Run with no argument, it uses `[Launcher] Dll=` from `op2launcher.ini`.
  - `--picker` boots into the in-game picker.
  - `--uninstall` removes the module from `outpost2.ini` and leaves `Launcher.dll` on disk.
- Registers itself in `outpost2.ini` on first run and never overwrites an existing setup.
- Every launch is one-shot: `Enabled=1` and `OneShot=1`, and the module switches itself off
  afterwards.
- Cross-platform. `build.bat` builds the Windows exe (with the ConVec icon and version info) and
  `build.sh` builds a Linux binary that starts OPU with `wine OPULauncher.exe`.
