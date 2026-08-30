# BackgroundHotkeyThing

Small native Win32 tray app: cycle desktop wallpapers from a folder with global hotkeys, per-monitor images, favorites, NSFW modes, and almost no dependencies.

**Product / releases:** [give.academy/BGHT](http://give.academy/BGHT)

## Source layout

| Path | What |
|------|------|
| `main.c` | The whole program |
| `resource.rc` + `BackgroundHotkeyThing.ico` | Tray / window icon |
| `Makefile` | Portable MinGW build (`gcc` + `windres` on `PATH`) |
| `build.bat` / `build.sh` | One-shot MinGW build wrappers |
| `scripts/Setup-BackgroundHotkeyThing.ps1` | Validate wallpaper folder, optional `NSFW\`, shortcuts |

This is a **source-only** repo. Binaries are not checked in — build locally or grab a release from the site.

## Build

Needs MinGW `gcc` + `windres` on `PATH` (Windows, or Git Bash / MSYS2).

```bat
build.bat
build.bat clean
```

```bash
./build.sh
./build.sh clean
# or:
mingw32-make
# -> build/BackgroundHotkeyThing.exe
```

## Run

```text
BackgroundHotkeyThing.exe <path-to-images> [minutes]
```

- Images: `png` / `jpg` / `jpeg` / `bmp`, nested folders up to depth **4** (`SCAN_MAX_DEPTH`)  
- NSFW: any path segment named `NSFW` (case-insensitive), e.g. `Albums\Beach\NSFW\x.jpg`  
- Path: absolute, UNC, or relative to the **exe** directory (Unicode / wide APIs)  
- Minutes: auto-rotate interval (default 2 if omitted)

## Ignoring folders

Rename (or create) a folder so its name starts with `ignore-` and it is left out of the scan — the folder itself and everything under it. Case does not matter.

```text
Wallpapers\
  Landscapes\          <- scanned
  ignore-old\          <- skipped
  Albums\Ignore-WIP\   <- skipped
  NSFW\                <- scanned as NSFW
```

A file named `ignore-me.jpg` in a normal folder is still used. Missing files (deleted, or a parent folder renamed) are dropped from rotation when next / previous / favorites next hits them.

## Setup helper

**Average user:** double-click `scripts\Setup.bat` and follow the prompts (folder browser, shortcuts, launch).

**Power user:**

```powershell
.\scripts\Setup-BackgroundHotkeyThing.ps1 -WallpaperPath "D:\Wallpapers" -Minutes 5 -Desktop -Startup
# fully non-interactive:
.\scripts\Setup-BackgroundHotkeyThing.ps1 -WallpaperPath "D:\Wallpapers" -Minutes 5 -Desktop -Quiet
```

Scans for supported images, warns about gotchas, can create `NSFW\` and Desktop/Startup shortcuts.

## Multi-monitor

Each display gets its own wallpaper (Windows 8+). Plug/unplug is detected; a screen that comes back keeps its history.

**Manual** next / previous / save favorite / open in Explorer apply to the monitor **under the cursor**.

**Auto-rotate** (the timer) is separate. Cycle its mode with Win+Alt-U or the tray menu:

| Mode | Timer does |
|------|------------|
| All monitors | Every connected screen gets a new image |
| Round robin | One screen per tick, walking the displays |
| Random | One random screen per tick |

## Hotkeys

| Combo | Action |
|-------|--------|
| Win+Shift-N / B | Next / previous on the monitor under the cursor (hold to cycle) |
| Win+Alt-P | Pause auto-rotate |
| Win+Alt-U | Auto-rotate: All monitors / Round robin / Random |
| Win+Shift-X | NSFW Off / Combined / Only |
| Win+Shift-L | Favorites-only cycle |
| Win+Shift-A / C | Save / clear favorites |
| Win+Shift-Z | Toggle desktop icons |
| Win+Shift-O | Open current image (cursor monitor) in Explorer |
| Win+Alt-N | Toast notifications |
| Win+Alt-Q | Quit |

Settings + favorites auto-save to `BackgroundHotkeyThing.ini` in the wallpaper folder.
