@echo off
setlocal
title BackgroundHotkeyThing Setup
cd /d "%~dp0"

echo.
echo   Starting BackgroundHotkeyThing setup...
echo   (A PowerShell window will guide you through a few steps.)
echo.

REM Bypass only for this process so double-click works without changing system policy.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup-BackgroundHotkeyThing.ps1" %*

if errorlevel 1 (
  echo.
  echo   Setup reported a problem. See messages above.
  pause
  exit /b 1
)

endlocal
