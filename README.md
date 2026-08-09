# BackgroundHotkeyThing

Small native Win32 tray app: cycle desktop wallpapers from a folder with global hotkeys, favorites, NSFW modes, and almost no dependencies.

**Product / releases:** [give.academy/BGHT](http://give.academy/BGHT)

## Source layout

| Path | What |
|------|------|
| `main.c` | The whole program |
| `resource.rc` + `BackgroundHotkeyThing.ico` | Tray / window icon |
| `Makefile` | Portable MinGW build (`gcc` + `windres` on `PATH`) |
| `scripts/Setup-BackgroundHotkeyThing.ps1` | Validate wallpaper folder, optional `NSFW\`, shortcuts |

This is a **source-only** repo. Binaries are not checked in — build locally or grab a release from the site.

## Build

```bash
mingw32-make
# -> build/BackgroundHotkeyThing.exe

mingw32-make clean
```

## Run

```text
BackgroundHotkeyThing.exe <path-to-images> [minutes]
```

- Images: `png` / `jpg` / `jpeg` / `bmp`, nested folders up to depth **4** (`SCAN_MAX_DEPTH`)  
- NSFW: any path segment named `NSFW` (case-insensitive), e.g. `Albums\Beach\NSFW\x.jpg`  
- Path: absolute, UNC, or relative to the **exe** directory (Unicode / wide APIs)  
- Minutes: auto-rotate interval (default 2 if omitted)

## Setup helper

**Average user:** double-click `scripts\Setup.bat` and follow the prompts (folder browser, shortcuts, launch).

**Power user:**

```powershell
.\scripts\Setup-BackgroundHotkeyThing.ps1 -WallpaperPath "D:\Wallpapers" -Minutes 5 -Desktop -Startup
# fully non-interactive:
.\scripts\Setup-BackgroundHotkeyThing.ps1 -WallpaperPath "D:\Wallpapers" -Minutes 5 -Desktop -Quiet
```

Scans for supported images, warns about gotchas, can create `NSFW\` and Desktop/Startup shortcuts.

## Hotkeys

| Combo | Action |
|-------|--------|
| Win+Shift-N / B | Next / previous (hold to cycle) |
| Win+Alt-P | Pause auto-rotate |
| Win+Shift-X | NSFW Off / Combined / Only |
| Win+Shift-L | Favorites-only cycle |
| Win+Shift-A / C | Save / clear favorites |
| Win+Shift-Z | Toggle desktop icons |
| Win+Shift-O | Open current image in Explorer |
| Win+Alt-N | Toast notifications |
| Win+Alt-Q | Quit |

Settings + favorites auto-save to `BackgroundHotkeyThing.ini` in the wallpaper folder.
