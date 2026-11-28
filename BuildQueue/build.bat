@echo off
REM Build BuildQueue.dll - must be 32-bit (it hooks the 32-bit Outpost2.exe in-process).
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
cl /nologo /LD /O2 /MT /EHsc /W3 /DWIN32 buildqueue.cpp /Fe:BuildQueue.dll /link modver.res user32.lib kernel32.lib
if errorlevel 1 ( echo ---BUILD FAILED--- & exit /b 1 )
del /q buildqueue.obj BuildQueue.exp BuildQueue.lib modver.res 2>nul
echo ---BUILD OK---
endlocal
