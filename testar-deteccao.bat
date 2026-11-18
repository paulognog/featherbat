@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo Detectando dispositivos sem fio...
echo.
featherbat.exe --once > "%TEMP%\featherbat_once.txt"
type "%TEMP%\featherbat_once.txt"
del "%TEMP%\featherbat_once.txt" 2>nul
echo.
pause
