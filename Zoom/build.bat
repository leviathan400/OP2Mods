@echo off
REM Build op2zoom.dll - must be 32-bit (it patches the 32-bit Outpost2.exe in-process).
setlocal
cd /d "%~dp0"

REM --- locate any Visual Studio install with the C++ x86 tools (incl. Build Tools / Insiders) ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if "%VSPATH%"=="" ( echo Could not locate Visual Studio via vswhere. & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall x86 failed & exit /b 1 )

rc /nologo /fo modver.res ..\modver.rc
if errorlevel 1 (echo RC FAILED & exit /b 1)
cl /nologo /O2 /MT /LD /W3 op2zoom.c /Fe:op2zoom.dll ^
   /link /DEF:op2zoom.def modver.res user32.lib kernel32.lib gdi32.lib
if errorlevel 1 ( echo ---BUILD FAILED--- & exit /b 1 )
echo ---BUILD OK---
endlocal
