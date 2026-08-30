#!/usr/bin/env bash
# MinGW wrapper for Windows (Git Bash / MSYS2 / Cygwin). Not a Linux/WSL native build.
set -euo pipefail
cd "$(dirname "$0")"

uname_s="$(uname -s 2>/dev/null || echo unknown)"
case "$uname_s" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT) ;;
  *)
    echo "[!!] This produces a Win32 exe via MinGW."
    echo "     On Linux/WSL use a MinGW-w64 cross toolchain, or run build.bat on Windows."
    ;;
esac

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "[xx] $1 not found on PATH."
    echo "     Install MinGW so gcc, windres, and mingw32-make are available."
    exit 1
  }
}

need gcc
need windres

MAKE=""
if command -v mingw32-make >/dev/null 2>&1; then
  MAKE=mingw32-make
elif command -v make >/dev/null 2>&1; then
  MAKE=make
else
  echo "[xx] mingw32-make / make not found on PATH."
  exit 1
fi

echo "[--] $MAKE $*"
"$MAKE" "$@"
if [[ -f build/BackgroundHotkeyThing.exe ]]; then
  echo "[ok] build/BackgroundHotkeyThing.exe"
fi
