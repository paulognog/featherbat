@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist build mkdir build
windres -i res\app.rc -o build\app_res.o --include-dir res || exit /b 1
set CFLAGS=-std=c11 -Os -nostdlib -ffreestanding -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -ffunction-sections -mno-stack-arg-probe -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra
set OBJS=
for %%f in (src\*.c) do (
    gcc %CFLAGS% -c "%%f" -o "build\%%~nf.o" || exit /b 1
    set OBJS=!OBJS! "build\%%~nf.o"
)
gcc -mwindows -nostdlib -s -Wl,--gc-sections -Wl,-e,wWinMainCRTStartup !OBJS! build\app_res.o -o featherbat.exe -lkernel32 -luser32 -lgdi32 -ladvapi32 -lsetupapi -lcfgmgr32 -lhid -lshell32 -lcomctl32 || exit /b 1
echo Built featherbat.exe
