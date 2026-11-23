@echo off
REM Build Launcher as a 32-bit op2ext .ini module -> Launcher.dll
REM OP2 and op2ext are x86, so this MUST compile x86.
setlocal
cd /d "%~dp0"

REM --- locate Visual Studio's vcvarsall via vswhere ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if "%VSPATH%"=="" (
  echo Could not locate Visual Studio via vswhere. Edit build.bat to point at vcvarsall.bat.
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 ( echo vcvarsall x86 failed & exit /b 1 )

REM --- compile + link the DLL (static CRT so it runs without VC redists) ---
rc /nologo picker.rc
if errorlevel 1 ( echo resource compile FAILED & exit /b 1 )
rc /nologo /d MODVER_USE_VERSION_H /i . /fo modver.res ..\..\modver.rc
if errorlevel 1 ( echo version resource compile FAILED & exit /b 1 )

cl /nologo /O2 /MT /EHsc /W3 /DWIN32 /D_WINDOWS ^
   launcher.cpp picker.cpp ^
   /link /DLL /OUT:Launcher.dll picker.res modver.res user32.lib gdi32.lib
if errorlevel 1 ( echo build FAILED & exit /b 1 )

echo.
echo Built Launcher.dll
echo.

REM --- deploy straight into the OPU folder (no sub-folder) ---
set "DEST=D:\Outpost 2\OPU"
if exist "%DEST%\OPULauncher.exe" (
  copy /y Launcher.dll "%DEST%\Launcher.dll" >nul
  echo Deployed to "%DEST%\Launcher.dll"
) else (
  echo OPU folder not found at "%DEST%" - copy Launcher.dll there yourself.
)

echo.
echo Register in outpost2.ini:  [ExternalModules] Launcher=yes  +  [Launcher] Dll=Launcher.dll
echo (op2launcher.exe does this automatically.)
endlocal
