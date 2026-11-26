# Changelog - Launcher.dll

The op2ext module that boots Outpost 2 (OPU 1.4.1) straight into a mission DLL, the picker, or a
saved game. The command-line front-end has its own changelog: [`../exe/CHANGELOG.md`](../exe/CHANGELOG.md).

The version is set in [`version.h`](version.h) and shared with `op2launcher.exe`.

## [0.9.7] - 2026-11-26

### Added
- **Filter box** at the top of the picker, focused when it opens. As you type, the list shows only
  the missions whose name starts with the text (case-insensitive), and its caption shows the count
  (e.g. "37 of 186"). Enter launches the name in the "type any DLL name" box, else the list's
  selection, else the only match left. A picked name that no longer matches the filter is cleared.
- **Mission details panel.** When a list entry stays selected for 0.5 s, the picker reads the DLL's
  exports straight from the file (the DLL is never loaded, so none of its code runs) and shows its
  `LevelDesc` name, mission type, players, max tech level and unit-mission flag (`DescBlock`),
  `MapName`, `TechtreeName` and the file's location. Mission types: 1-12 campaign mission, -1
  colony, -2 auto-demo, -3 tutorial, -4 Land Rush, -5 Space Race, -6 Resource Race, -7 Midas,
  -8 Last One Standing.

### Fixed
- **The picker now lists every mission.** It used to list only the DLLs next to `Outpost2.exe`,
  but OPU keeps most missions in per-map subfolders (`OPU\base\maps\<map>\`, `OPU\maps\<map>\`,
  including the LUA missions and official demos). It now scans the exe folder, the OPU folder and
  both maps trees, keeps only DLLs that export `DescBlock` (read from the file, never loaded) and
  shows each name once. On a full OPU install that's 186 missions, matching the game's own
  "PICK A SCRIPT" list. The hand-kept list of engine DLLs to hide is gone.

### Changed
- The picker is wider: LAUNCH and CANCEL sit side by side at the top right, with the details panel
  under them. The details are label / value pairs on two lines each, drawn as transparent text over
  the skin's background. Each line is its own label, destroyed and re-created for every mission
  rather than given new text: changing a skinned label's text made Windows repaint it plain white
  with the previous text showing through. The new labels are drawn transparent in the skin's
  label colour by the dialog itself (`WM_CTLCOLORSTATIC`). The dialog also shows keyboard cues
  from the start, so pressing Alt doesn't repaint the labels in plain Windows white.
- Picker label: "Mission / scenario DLLs:" (was "... in this folder:").

## [0.9.6] - 2026-11-22

### Added
- **Load a saved game.** New `[Launcher] Load=<save>` key. The path can be relative to the OPU
  folder (e.g. `saves\SGAME0.OP2`) or absolute.
  - It takes precedence over `Mission=` and the picker.
  - It's cleared from the ini once used, so a later launch won't load the same save again.
  - `OneShot=1` still sets `Enabled=0` afterwards, as for a mission launch.
- `op2::LoadSavedGame()` in `engine.h`. It loads a save the way the shell's own Load button does:
  it puts the file name at `TApp +0x5C`, then calls `TApp::OnLoadGame` (`0x004874F0`) and then
  `ActivateGame`. Calling `TApp::LoadGame` on its own skips loading the sprite sheets, and the game
  crashes drawing its first frame.
- `launcher.log` now records the `Load=` value and whether the load succeeded. A missing file is
  logged and nothing is loaded.

### Notes
- A save made by a Lua mission loads with its script carrying on under OP2Lua 0.8.8 and later,
  which keeps the script's state in the save. Older OP2Lua versions load the save without that
  state.

## [0.9.5] - 2026-11-21

### Fixed
- **Mission names are capped at 31 characters.** The engine keeps the name in
  `GameStartInfo.scriptName[32]` and copies it in without checking the length, so a longer name
  overflowed the field. Names that are too long are rejected rather than shortened, because a
  shortened name would load a different DLL or none.
  - The picker's edit box stops at 31 characters. Choosing a longer filename from the list shows
    a warning and keeps the picker open.
  - `DoLaunch` refuses a long name and logs why. This also covers a hand-edited `Mission=`.

### Changed
- The DLL now carries its own version (0.9.5) instead of the shared module default of 1.0.0.
  `modver.rc` accepts a per-module version through `/d MODVER_USE_VERSION_H`.
- `build.bat` finds any Visual Studio install with the C++ x86 tools, including Build Tools and
  prerelease/Insiders builds.

## [0.9] - 2026-06-25

### Added
- First release as **Launcher** (`launcher.cpp`, replacing `launchinto.cpp`).
- Launches through the engine's own debug-menu path: `TApp::OnLoadScript` then
  `TApp::ActivateGame`, run on the main thread from the shell window's message loop.
- `[Launcher] Enabled` as the everyday on/off switch, separate from op2ext's
  `[ExternalModules] Launcher` load flag. `OneShot=1` sets `Enabled=0` after one launch.
- The picker is a Win32 dialog skinned to the OP2 look through odasl's `wplEnable`/`wplDisable`.
  It lists the mission DLLs in the folder and lets you type any name.
- Paths are taken from the current directory (the OPU runtime folder), not the `Outpost2.exe`
  folder. Logs go to `logs\launcher.log`.

## 2026-06-19 - first version

- Debug DLL launcher (`launchinto.cpp`): boots OPU into a chosen mission DLL instead of going
  through the keystroke-driven debug menu.
