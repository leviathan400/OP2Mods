#!/bin/sh
# Build op2launcher for Linux (run on Linux, or Windows WSL).
#
# This is the install/uninstall/launch front-end (op2launcher.cpp). OPU itself runs
# under Wine; this only writes outpost2.ini and execs
# `wine OPULauncher.exe`, so a normal 64-bit build is fine - it is NOT injected into
# Outpost2.exe (that is Launcher.dll, a 32-bit Windows DLL).
#
# (The ConVec icon + version info in launcher.rc are Windows-only resources; the Linux
# binary just omits them.)
set -e
cd "$(dirname "$0")"

g++ -O2 -std=c++17 -Wall -o op2launcher op2launcher.cpp
echo "Built op2launcher (Linux)."
echo
echo "Deploy: copy ./op2launcher (+ optional op2launcher.ini) into your Wine prefix's"
echo "OPU folder, next to OPULauncher.exe and outpost2.ini, e.g.:"
echo "  ~/.wine/drive_c/Outpost 2/OPU/"
echo "Then run:  ./op2launcher e04.dll   (or ./op2launcher --uninstall)"
