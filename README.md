# BackgroundHotkeyThing
A small program to scan a folder for png/jpg files and cycle through them as desktop backgrounds.

Also looks for a folder named NSFW within given folder for photos for use only in an NSFW mode.

Allow showing/hiding desktop icons with hotkey.

Allow goin back for up to 50 previous backgrounds (in order of display).

Allow skipping to random next BG.

Allow pausing/favoriting/saving favorites.

Two cycle modes: Normal(All photos filtered by NSFW mode)/Only Favorites.

Three NSFW modes: Off, Combined, Only NSFW folder.

Customize rotation speed (its an approximation of seconds, good enough and adds a little randomness)...

# Usage
```
BackgroundHotkeyThing.exe <path to png files> <number of seconds to delay rotation>
```

# Hotkeys
- Win+Shift-N - Set next Background
- Win+Shift-B - Set previous Background
- Win+Shift-V - Pause Auto Rotate
- Win+Shift-H - Change NSFW Mode (0/3rd press - Off, 1st press - Combined, 2nd press - Only NSFW)
- Win+Shift-L - Only Cycle favorites (will not display NSFW favorites if NSFW mode is off)
- Win+Shift-E - Export Favorited to file (located in provided wallpaper path as BackgroundHotkeyThing.ini)
- Win+Shift-C - Clears Saved favorites from Exported File (may update to also clear current loaded favorites)

- Win-Z       - Toggle show/hide desktop icons
- Win-S       - Save current background into top of favorites (sets load position to top)
- Win-F       - Load favorite at top slot (starts at top and goes backward)
- Win-Q       - Quit