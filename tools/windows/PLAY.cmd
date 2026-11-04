@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Controller-Setup.ps1" -Mode Play
if errorlevel 1 pause
