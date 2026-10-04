@echo off
rem Build input-mouser.exe (needs MinGW-w64 gcc and windres on PATH)
rem
rem   build.cmd          release build -> input-mouser.exe
rem   build.cmd debug    debug build   -> build\input-mouser-debug.exe
rem
rem MinGW's gcc links its own default-manifest.o, which clashes with ours
rem (two RT_MANIFEST #1). An empty default-manifest.o found first via -B
rem replaces it, so only src\input-mouser.manifest ends up in the exe.
rem
rem This file is ASCII-only on purpose (see mayous\build.bat).
setlocal
cd /d "%~dp0"
if not exist build mkdir build
echo.> build\empty.c
gcc -c build\empty.c -o build\default-manifest.o || exit /b 1
windres -I src src\input-mouser.rc -O coff -o build\input-mouser.res.o || exit /b 1

set "SRC=src\main.c src\config.c src\crypto.c src\net.c src\hook.c src\inject.c src\clip.c src\theme.c src\ui_common.c src\ui_main.c src\ui_layout.c src\ui_peer.c src\firewall.c src\cursor.c"
set "LIBS=-luser32 -lgdi32 -lshell32 -lcomctl32 -ldwmapi -luxtheme -lshlwapi -lole32 -luuid -lws2_32 -liphlpapi -lbcrypt -lwtsapi32 -limm32 -loleaut32"
set "WARN=-Wall -Wextra -Wno-cast-function-type -Wno-missing-field-initializers"

if /i "%~1"=="debug" (
    gcc -Bbuild/ -g -O0 %WARN% -municode -mwindows -o build\input-mouser-debug.exe %SRC% build\input-mouser.res.o %LIBS% || exit /b 1
    echo built build\input-mouser-debug.exe
    exit /b 0
)
gcc -Bbuild/ -O2 -fno-ident %WARN% -municode -mwindows -static -s -o input-mouser.exe %SRC% build\input-mouser.res.o %LIBS% || exit /b 1
for %%F in (input-mouser.exe) do echo built input-mouser.exe (%%~zF bytes)
