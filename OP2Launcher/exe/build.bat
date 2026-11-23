@echo off
REM Build op2launcher.exe (x86 console) - the install/uninstall/launch front-end
REM (op2launcher.cpp), with the ConVec icon + OP2Launcher version info (launcher.rc).
REM Deploys next to OPULauncher.exe.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if "%VSPATH%"=="" ( echo Could not locate Visual Studio via vswhere. & exit /b 1 )

call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 ( echo vcvarsall x86 failed & exit /b 1 )

rc /nologo launcher.rc
if errorlevel 1 ( echo resource compile FAILED & exit /b 1 )

cl /nologo /O2 /MT /EHsc /W3 op2launcher.cpp launcher.res /link /OUT:op2launcher.exe
if errorlevel 1 ( echo build FAILED & exit /b 1 )

echo.
echo Built op2launcher.exe

REM --- deploy to the OPU folder ---
set "DEST=D:\Outpost 2\OPU"
if not exist "%DEST%\OPULauncher.exe" ( echo (OPU folder not found at "%DEST%" - copy op2launcher.exe there yourself) & goto :done )
copy /y op2launcher.exe "%DEST%\op2launcher.exe" >nul
echo Deployed op2launcher.exe to "%DEST%"
if not exist "%DEST%\op2launcher.ini" (
  copy /y op2launcher.ini "%DEST%\op2launcher.ini" >nul
  echo Deployed op2launcher.ini to "%DEST%"
) else (
  echo Left existing "%DEST%\op2launcher.ini" untouched
)

:done
echo.
echo Run from the OPU folder:  op2launcher.exe e04.dll      (install + launch)
echo                           op2launcher.exe --uninstall  (remove from outpost2.ini)
endlocal
