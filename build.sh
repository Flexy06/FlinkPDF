#!/bin/sh
set -e
cd "$(dirname "$0")"
mkdir -p build
[ -f res/app.ico ] || (cd res && python3 mkicon.py)
x86_64-w64-mingw32-windres -c 65001 res/app.rc -O coff -o build/app.res -I res
x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-missing-field-initializers \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000006 \
  -municode -mwindows -static -s \
  src/main.cpp build/app.res -o build/FlinkPDF.exe \
  -ld2d1 -ldwrite -ldwmapi -luxtheme -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lole32 -luuid -lgdi32 -luser32 -ladvapi32 -lwinspool
ls -la build/FlinkPDF.exe
