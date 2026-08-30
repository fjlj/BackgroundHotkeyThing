@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title BackgroundHotkeyThing build

where gcc >nul 2>&1
if errorlevel 1 (
  echo [xx] gcc not found on PATH.
  echo     Install MinGW / TDM-GCC so gcc and windres are available, then retry.
  exit /b 1
)
where windres >nul 2>&1
if errorlevel 1 (
  echo [xx] windres not found on PATH.
  echo     It ships with MinGW; add the MinGW bin folder to PATH.
  exit /b 1
)

set "MAKE="
where mingw32-make >nul 2>&1
if not errorlevel 1 set "MAKE=mingw32-make"
if not defined MAKE (
  where make >nul 2>&1
  if not errorlevel 1 set "MAKE=make"
)
if not defined MAKE (
  echo [xx] mingw32-make / make not found on PATH.
  exit /b 1
)

echo [--] %MAKE% %*
%MAKE% %*
if errorlevel 1 (
  echo [xx] Build failed.
  exit /b 1
)

if exist "build\BackgroundHotkeyThing.exe" (
  echo [ok] build\BackgroundHotkeyThing.exe
)
exit /b 0
