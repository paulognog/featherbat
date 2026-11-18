@echo off
chcp 65001 >nul
cd /d "%~dp0"
featherbat.exe --uninstall > "%TEMP%\featherbat_uninst.txt"
type "%TEMP%\featherbat_uninst.txt"
del "%TEMP%\featherbat_uninst.txt" 2>nul
echo.
pause
