# UIExt: double-tap a control group to centre the camera, double-click a vehicle to select its type

A community mod (`UIExt.dll`, "UI Extension") written by **TH300**, who sent it to us.
We reverse-engineered his DLL to find out what it does, then wrote clean, commented
source that does the same.

**For OPU 1.4.1.** Tested in game on 2026-11-25.

## What it does

Adds a feature Outpost 2 shipped without:

> Press a control-group key to select that group. Press it **again** while the group
> is still selected (a "double tap") and the **camera jumps to the group.**

Without the mod, a second press does nothing more than the first.

Our own addition (not in TH300's original), `DoubleClickSelect=1`:

> **Double-click one of your vehicles** on the map and **every vehicle of yours of the same type
> that is on screen** is selected (up to the game's limit of 32, nearest to the clicked one first).

Tested in game on OPU 1.4.1 (2026-11-27): double-clicking a Lynx, a Tiger and a ConVec selected
the 7, 7 and 3 of each type on screen.

## How it works

Outpost 2's "recall control group N" is `FUN_0047bd50` @ `0x0047BD50`:

- `ECX` = the control-group array ("container"), `[esp+4]` = group index.
- Each group is a `0x41`-byte record: unit count at `+0x0C`, `u16` unit ids at `+0x0D`.
- The recall rebuilds the selection from the group's units, and **centres the view
  only if the flag at `container+0x298` is set**: `FUN_00408140` averages the units'
  positions and `FUN_00408210` scrolls the viewport there. The game never sets that
  flag.

UIExt hooks the recall. **Before** it runs, it checks whether every currently selected
unit (the live selection buffer at `0x00547210`: `[0]` = count, `[1..]` = unit ids) is in
the group being recalled, meaning you pressed the key for a group you already have
selected. If so it sets `+0x298` and the recall also centres the camera; otherwise it
clears it. It only touches the camera, so it is safe in multiplayer.

Our rebuild does this with **one** 5-byte `JMP` at the start of `FUN_0047bd50` into a
naked hook. The hook runs the check with every register saved, runs the two
instructions the `JMP` replaced (`mov eax,[esp+4]; push ebx`), and continues the game's
function at `0x0047BD55`. It checks the original bytes before patching and puts them
back on unload.

## How double-click select works

The game never sees a double-click (its window classes have no `CS_DBLCLKS`), so UIExt times the
two clicks itself, with the Windows double-click time.

- Map clicks go through `MouseCommandFilter::OnUIEvent` (`0x0041ABC0`, `__thiscall`, args `wnd,
  msg, wParam, lParam, userData`), reached through slot 0 of its vtable at `0x004CFC60`. UIExt
  replaces that one vtable entry (after checking it still holds `0x0041ABC0`) with a wrapper that
  calls the game's handler first.
- After a plain left click (no Shift/Ctrl) over the map that left exactly one of your vehicles
  selected, with the mouse on it, the wrapper notes the vehicle and the time. A second such click
  on the same vehicle in time is a double-click.
- Then: every one of your vehicles (`PlayerVehicleEnum`) with the same `map_id`, alive, inside
  the visible map area (view left/top at view surface `+0x940`/`+0x944`, size = the map window's
  client area), sorted nearest first, up to 32. The selection (`0x00547210`) is rebuilt the way
  the game's own selection-history restore does it - `Clear` (`0x0041A9C0`) then `AddUnit`
  (`0x0041A830`) per unit - then stored in the selection history (`0x0047BFD0`) and the game's
  group-select sound plays.
- Only vehicles: the game never selects several buildings. Same type means the same unit type -
  a Laser Lynx and an EMP Lynx are both Lynx.
- Selection is local UI state; no command is sent, so it is safe in multiplayer.

## Install (OPU 1.4.1)

Build (or take) `UIExt.dll`, copy it to `OPU\UIExt.dll`, and add to
`OPU\outpost2.ini`:

```ini
[ExternalModules]
UIExt=yes

[UIExt]
Dll=UIExt.dll
Enabled=1
DoubleClickSelect=1
```

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | `1` = on, `0` = off (the DLL loads but patches nothing) |
| `DoubleClickSelect` | `1` | `1` = double-click a vehicle selects all of that type on screen, `0` = off |

Settings are read once at startup, so restart the game after changing it. The
`[ExternalModules]` line must stay `yes`: OPU's module loader only accepts `yes`/`no`
there.

The log is `OPU\logs\UIExt.log`. A working start looks like:

```
==== op2uiext (recreated UIExt) startup 2026-11-25 21:03:24 ====
INSTALLED: double-click a vehicle -> select that type on screen. hook=...
INSTALLED: double-tap control group -> center camera. hook=...
```

Each double-click logs a line such as `double-click: vehicle #95 (type 5) -> 7 selected on screen`.

With `Enabled=0` it logs `DISABLED: ...` instead. If the exe's bytes are not the
expected ones it logs `VERIFY FAILED ...` and patches nothing.

## Build

```
build.bat
```

32-bit MSVC. `build.bat` finds Visual Studio through `vswhere` and links the shared
`..\modver.rc` version block. Output: `UIExt.dll`.

## TH300's original

`UIExt.dll` (5.6 KB, PE timestamp **2022-09-05**, VS2003 toolset, image base
`0x10000000`) patches the game in memory:

- No exports: it works from `DllMain`. It installs **5 reversible byte patches** into
  `Outpost2.exe` at load (`VirtualProtect`, verify, write) and reverses them at
  unload. It logs to `logUIExt.txt`, shows a `"Failed to install/uninstall patch"`
  message box on a mismatch, and writes `"[UIExt] END."` on unload.
- One patch **NOPs 27 bytes and writes a `CALL`** that sends the recall through its
  checker, which does the same subset test and then calls the
  original `FUN_0047bd50`. The others reset the `+0x298` flag at the right times.

Addresses it touches in `Outpost2.exe`: `0x408140/210/370`, `0x41B158`, `0x45CCA0`,
`0x47BD50`, `0x47F247`, `0x486D3C`, dispatcher `0x498D3E`; data `0x547210`. All of
these are byte-identical in retail 1.3.6 and OPU 1.4.1.

Our rebuild sets **and** clears the flag in its one hook on every recall, so it needs
none of the companion patches. The behaviour is the same.

## Files

| File | What |
|---|---|
| `op2uiext.c` | the source, fully commented |
| `op2uiext.def` | op2ext exports |
| `build.bat` | MSVC x86 build |
| `UIExt.dll` | prebuilt, tested on OPU 1.4.1 |
