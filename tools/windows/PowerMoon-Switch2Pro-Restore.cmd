@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0PowerMoon-Switch2Pro-Setup.ps1" -Mode Restore
pause
