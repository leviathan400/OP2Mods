// version.h - the ONE place the Launcher version is set.
// Used by both resource scripts, so op2launcher.exe and Launcher.dll always carry the same number:
//   - ..\exe\launcher.rc    (op2launcher.exe)          #include "..\launcher\version.h"
//   - ..\..\modver.rc       (Launcher.dll)             build.bat passes /d MODVER_USE_VERSION_H /i .
// Bump MAJOR/MINOR/PATCH here, then rebuild both.
#pragma once

#define MODVER_MAJOR 0
#define MODVER_MINOR 9
#define MODVER_PATCH 6
