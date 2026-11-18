@echo off
chcp 65001 >nul
cd /d "%~dp0"
featherbat.exe --quit
echo FeatherBat encerrado.
ping -n 2 127.0.0.1 >nul
