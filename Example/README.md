# BackgroundHotkeyThing
A small program to scan a folder for png/jpg/jpeg/bmp files and cycle through them as desktop backgrounds.

Also looks for a folder named NSFW within given folder for photos for use only in an NSFW mode.

Allow showing/hiding desktop icons with hotkey.

Allow goin back for up to 50 previous backgrounds (in order of display).

Allow skipping to random next BG.

Allow pausing/favoriting/saving favorites (favorites + settings auto-save to the ini on change).

Single-instance: launching a second copy just tells you it's already running (tray icon).

Two cycle modes: Normal(All photos filtered by NSFW mode)/Only Favorites(also filtered by NSFW mode).

Three NSFW modes: Off, Combined, Only NSFW folder.

Customize rotation speed (minutes between auto-rotates; which wallpaper comes next is random).

# Usage
```
BackgroundHotkeyThing.exe <path to BG images> <number of minutes to delay rotation>
```

Path can be:
- Relative (`BGs`, `.\BGs`) — resolved against the **exe folder** (not the shell cwd), so shortcuts keep working
- Absolute (`C:\Wallpapers`, `D:\Pics\Desktop`)
- Network / UNC (`\\server\share\Wallpapers`)

# Hotkeys
- Win+Shift-N - Set next Background (hold to cycle)
- Win+Shift-B - Set previous Background (hold to cycle)
- Win+Alt-V   - Pause Auto Rotate
- Win+Shift-H - Change NSFW Mode (0/3rd press - Off, 1st press - Combined, 2nd press - Only NSFW)
- Win+Shift-L - Only Cycle favorites (will not display NSFW favorites if NSFW mode is off)
- Win+Shift-C - Clears favorites (memory + ini)

- Win+Alt-S   - Force-save settings (also auto-saves on pause/NSFW/favs-mode/notifications change)
- Win+Alt-L   - Re-load saved Settings

- Win+Shift-Z - Toggle show/hide desktop icons
- Win+Alt-N   - Toggle toast notifications
- Win+Shift-O - Open current background in File Explorer
- Win+Shift-A - Save/upsert current background into favorites (bumps to top if already favorited; auto-saves to ini)
- Win+Alt-Q   - Quit

Favorites + settings live in `BackgroundHotkeyThing.ini` inside the wallpaper folder you pass in.
