@echo off
REM Build AutoRepair-UI.exe - the Dear ImGui viewer for the AutoRepair module (32-bit, like the module it reads from;
REM standalone).
setlocal
cd /d "%~dp0"

REM --- locate any Visual Studio install with the C++ tools (incl. Build Tools / Insiders) ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if "%VSPATH%"=="" ( echo Could not locate Visual Studio via vswhere. & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall x86 failed & exit /b 1 )

if not exist obj mkdir obj
rc /nologo /fo obj\app.res app.rc
if errorlevel 1 ( echo ---RC FAILED--- & exit /b 1 )
cl /nologo /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /Iimgui /Fo:obj\ ^
   main.cpp ^
   imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
   imgui\backends\imgui_impl_win32.cpp imgui\backends\imgui_impl_dx11.cpp ^
   /Fe:AutoRepair-UI.exe ^
   /link /SUBSYSTEM:WINDOWS obj\app.res d3d11.lib d3dcompiler.lib dxgi.lib user32.lib gdi32.lib dwmapi.lib
if errorlevel 1 ( echo ---BUILD FAILED--- & exit /b 1 )
rmdir /s /q obj
echo ---BUILD OK---
endlocal
