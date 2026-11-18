@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo Gerando dump de dispositivos HID e Bluetooth (pode levar ~6s se houver receptor Xenta)...
echo.
featherbat.exe --dump > "%~dp0dump.txt"
type "%~dp0dump.txt"
echo.
echo Dump salvo em: %~dp0dump.txt
pause
