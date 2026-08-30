#Requires -Version 5.1
<#
.SYNOPSIS
  Set up BackgroundHotkeyThing against a wallpaper folder.

.DESCRIPTION
  Double-click / run with no args for a guided wizard (folder browser, plain-English prompts).

  Advanced users can pass parameters to skip questions. -Quiet is fully non-interactive.

.EXAMPLE
  # Average user: wizard
  .\Setup-BackgroundHotkeyThing.ps1

.EXAMPLE
  # Advanced: one-liner
  .\Setup-BackgroundHotkeyThing.ps1 -WallpaperPath "D:\Pics\Desktop" -Minutes 5 -Desktop -Startup -Launch -CreateNsfw

.EXAMPLE
  # Automation
  .\Setup-BackgroundHotkeyThing.ps1 -WallpaperPath "D:\Wallpapers" -Minutes 10 -Desktop -Quiet
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$WallpaperPath,

    [Parameter(Mandatory = $false)]
    [string]$ExePath,

    [Parameter(Mandatory = $false)]
    [ValidateRange(1, 1440)]
    [int]$Minutes = 5,

    [switch]$CreateNsfw,
    [switch]$NoNsfwPrompt,

    [switch]$Desktop,
    [switch]$StartMenu,
    [switch]$Startup,

    [switch]$Launch,
    [switch]$SkipLaunchPrompt,

    # no interactive questions — only do what switches request
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$AppName = 'BackgroundHotkeyThing'
$Supported = @('.png', '.jpg', '.jpeg', '.bmp')
$UnsupportedButCommon = @('.webp', '.gif', '.tif', '.tiff', '.heic', '.jfif', '.avif')

# was this parameter actually typed on the command line?
function Test-Bound([string]$Name) { $PSBoundParameters.ContainsKey($Name) }

$Interactive = -not $Quiet
# full guided tour when they basically just double-clicked / ran with no flags
$Wizard = $Interactive -and (
    -not (Test-Bound 'WallpaperPath') -and
    -not (Test-Bound 'ExePath') -and
    -not (Test-Bound 'Minutes') -and
    -not (Test-Bound 'Desktop') -and
    -not (Test-Bound 'StartMenu') -and
    -not (Test-Bound 'Startup') -and
    -not (Test-Bound 'Launch') -and
    -not (Test-Bound 'CreateNsfw')
)

function Write-Ok($msg)   { Write-Host "  [ok]  $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "  [!!]  $msg" -ForegroundColor Yellow }
function Write-Bad($msg)  { Write-Host "  [xx]  $msg" -ForegroundColor Red }
function Write-Info($msg) { Write-Host "  [--]  $msg" -ForegroundColor Cyan }
function Write-Step($n, $title) {
    Write-Host ""
    Write-Host "  Step $n — $title" -ForegroundColor White
    Write-Host "  -------------------------------------------" -ForegroundColor DarkGray
}

function Resolve-FullPathSafe([string]$p) {
    if ([string]::IsNullOrWhiteSpace($p)) { return $null }
    $p = $p.Trim().Trim('"')
    if (-not [System.IO.Path]::IsPathRooted($p)) {
        $p = Join-Path (Get-Location) $p
    }
    return [System.IO.Path]::GetFullPath($p)
}

function Read-YesNo {
    param(
        [string]$Prompt,
        [bool]$DefaultYes = $true
    )
    $hint = if ($DefaultYes) { '[Y/n]' } else { '[y/N]' }
    while ($true) {
        $a = Read-Host "  $Prompt $hint"
        if ([string]::IsNullOrWhiteSpace($a)) { return $DefaultYes }
        if ($a -match '^[Yy]') { return $true }
        if ($a -match '^[Nn]') { return $false }
        Write-Host "  Please type Y or N (or just press Enter for the default)." -ForegroundColor DarkGray
    }
}

function Read-Number {
    param(
        [string]$Prompt,
        [int]$Default,
        [int]$Min = 1,
        [int]$Max = 1440
    )
    while ($true) {
        $a = Read-Host "  $Prompt [$Default]"
        if ([string]::IsNullOrWhiteSpace($a)) { return $Default }
        $n = 0
        if ([int]::TryParse($a, [ref]$n) -and $n -ge $Min -and $n -le $Max) { return $n }
        Write-Host "  Enter a number between $Min and $Max." -ForegroundColor DarkGray
    }
}

function Invoke-FolderBrowser {
    param([string]$Description = 'Select your wallpaper folder')
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $d = New-Object System.Windows.Forms.FolderBrowserDialog
        $d.Description = $Description
        $d.ShowNewFolderButton = $true
        # topmost-ish: use a dummy form as owner so the dialog isn't lost behind windows
        $form = New-Object System.Windows.Forms.Form
        $form.TopMost = $true
        $form.Opacity = 0
        $form.ShowInTaskbar = $false
        $form.WindowState = 'Minimized'
        [void]$form.Show()
        $result = $d.ShowDialog($form)
        $form.Close()
        $form.Dispose()
        if ($result -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    } catch {
        Write-Warn "Folder browser unavailable ($($_.Exception.Message)). Paste a path instead."
    }
    return $null
}

function Invoke-FileBrowser {
    param(
        [string]$Title = 'Select BackgroundHotkeyThing.exe',
        [string]$Filter = 'BackgroundHotkeyThing|BackgroundHotkeyThing.exe|Programs (*.exe)|*.exe'
    )
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $d = New-Object System.Windows.Forms.OpenFileDialog
        $d.Title = $Title
        $d.Filter = $Filter
        $d.CheckFileExists = $true
        if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $d.FileName }
    } catch {
        Write-Warn "File browser unavailable. Paste the full path to the .exe instead."
    }
    return $null
}

function Find-DefaultExe {
    $here = $PSScriptRoot
    $candidates = @(
        (Join-Path $here 'BackgroundHotkeyThing.exe'),
        (Join-Path $here '..\build\BackgroundHotkeyThing.exe'),
        (Join-Path $here '..\BackgroundHotkeyThing.exe'),
        (Join-Path (Get-Location) 'BackgroundHotkeyThing.exe'),
        (Join-Path (Get-Location) 'build\BackgroundHotkeyThing.exe')
    )
    foreach ($c in $candidates) {
        $full = Resolve-FullPathSafe $c
        if ($full -and (Test-Path -LiteralPath $full -PathType Leaf)) { return $full }
    }
    return $null
}

function New-Shortcut {
    param(
        [string]$LinkPath,
        [string]$TargetPath,
        [string]$Arguments,
        [string]$WorkDir,
        [string]$Description
    )
    $dir = Split-Path -Parent $LinkPath
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $w = New-Object -ComObject WScript.Shell
    $sc = $w.CreateShortcut($LinkPath)
    $sc.TargetPath = $TargetPath
    $sc.Arguments = $Arguments
    $sc.WorkingDirectory = $WorkDir
    $sc.Description = $Description
    $sc.WindowStyle = 7  # minimized — tray app
    if (Test-Path -LiteralPath $TargetPath) {
        $sc.IconLocation = "$TargetPath,0"
    }
    $sc.Save()
    [System.Runtime.InteropServices.Marshal]::ReleaseComObject($w) | Out-Null
}

# matches main.c SCAN_MAX_DEPTH — root + this many nested folder levels
$script:ScanMaxDepth = 4

function Test-PathHasNsfwSegment([string]$FullPath) {
    $parts = $FullPath -split '[\\/]'
    foreach ($p in $parts) {
        if ($p -and ($p -ieq 'NSFW')) { return $true }
    }
    return $false
}

function Get-FolderScan {
    param([string]$Root)

    $sfw = New-Object System.Collections.Generic.List[object]
    $nsfw = New-Object System.Collections.Generic.List[object]
    $unsupported = @{}
    $tooLong = New-Object System.Collections.Generic.List[string]
    $nestedDirs = 0
    $ignoredDirs = 0

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return $null
    }

    # depth 0 = root; recurse while depth < ScanMaxDepth (same idea as main.c)
    function Walk([string]$Dir, [int]$Depth) {
        Get-ChildItem -LiteralPath $Dir -Force -ErrorAction SilentlyContinue | ForEach-Object {
            if ($_.PSIsContainer) {
                if ($_.Name -eq '.' -or $_.Name -eq '..') { return }
                if ($_.Name -like 'ignore-*') {
                    $script:ignoredDirsRef++
                    return
                }
                if ($Depth -lt $script:ScanMaxDepth) {
                    $script:nestedDirsRef++
                    Walk -Dir $_.FullName -Depth ($Depth + 1)
                }
            } else {
                $ext = $_.Extension.ToLowerInvariant()
                if ($Supported -contains $ext) {
                    if ($_.FullName.Length -ge 240) { [void]$tooLong.Add($_.FullName) }
                    if (Test-PathHasNsfwSegment $_.FullName) {
                        [void]$nsfw.Add($_)
                    } else {
                        [void]$sfw.Add($_)
                    }
                } elseif ($UnsupportedButCommon -contains $ext) {
                    if (-not $unsupported.ContainsKey($ext)) { $unsupported[$ext] = 0 }
                    $unsupported[$ext]++
                }
            }
        }
    }

    $script:nestedDirsRef = 0
    $script:ignoredDirsRef = 0
    Walk -Dir $Root -Depth 0
    $nestedDirs = $script:nestedDirsRef
    $ignoredDirs = $script:ignoredDirsRef

    [pscustomobject]@{
        SfwCount       = $sfw.Count
        NsfwCount      = $nsfw.Count
        TotalSupported = $sfw.Count + $nsfw.Count
        Unsupported    = $unsupported
        NestedDirs     = $nestedDirs
        IgnoredDirs    = $ignoredDirs
        TooLong        = $tooLong
        HasNsfwDir     = Test-Path -LiteralPath (Join-Path $Root 'NSFW') -PathType Container
    }
}

function Wait-IfInteractive {
    if (-not $Interactive) { return }
    # Setup.bat / double-click: don't vanish before they can read
    if ([Environment]::UserInteractive) {
        Write-Host ""
        [void](Read-Host "  Press Enter to close this window")
    }
}

# =============================================================================
#  Begin
# =============================================================================
Write-Host ""
Write-Host "  ===========================================" -ForegroundColor DarkCyan
Write-Host "   $AppName  —  setup wizard" -ForegroundColor White
Write-Host "  ===========================================" -ForegroundColor DarkCyan
if ($Wizard) {
    Write-Host "  Guided mode. Answer a few questions and you're done." -ForegroundColor DarkGray
    Write-Host "  (Power users: pass -WallpaperPath, -Minutes, -Desktop, … or -Quiet)" -ForegroundColor DarkGray
} elseif ($Quiet) {
    Write-Host "  Quiet mode — no prompts." -ForegroundColor DarkGray
} else {
    Write-Host "  Using your command-line options; will only ask for what's missing." -ForegroundColor DarkGray
}
Write-Host ""

# --- Step 1: find the program ---
if ($Wizard) { Write-Step 1 'Find the program' }

if (-not $ExePath) {
    $ExePath = Find-DefaultExe
}

if (-not $ExePath -or -not (Test-Path -LiteralPath (Resolve-FullPathSafe $ExePath) -PathType Leaf)) {
    if ($Quiet) {
        Write-Bad "Could not find BackgroundHotkeyThing.exe"
        Write-Info "Pass -ExePath 'C:\path\to\BackgroundHotkeyThing.exe'"
        exit 1
    }
    Write-Warn "Couldn't auto-find BackgroundHotkeyThing.exe next to this script."
    if (Read-YesNo "Browse for BackgroundHotkeyThing.exe?" $true) {
        $picked = Invoke-FileBrowser
        if ($picked) { $ExePath = $picked }
    }
    if (-not $ExePath) {
        $typed = Read-Host "  Or paste the full path to BackgroundHotkeyThing.exe"
        $ExePath = $typed
    }
}

$ExePath = Resolve-FullPathSafe $ExePath
if (-not $ExePath -or -not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    Write-Bad "Still can't find the program. Setup needs BackgroundHotkeyThing.exe."
    Wait-IfInteractive
    exit 1
}
Write-Ok "Program: $ExePath"

# --- Step 2: wallpaper folder ---
if ($Wizard) { Write-Step 2 'Wallpaper folder' }
else { Write-Host "" }

if (-not $WallpaperPath) {
    if ($Quiet) { Write-Bad "Quiet mode needs -WallpaperPath"; exit 1 }

    Write-Host "  This is the folder that holds your wallpaper images." -ForegroundColor DarkGray
    Write-Host "  Supported: .png  .jpg  .jpeg  .bmp" -ForegroundColor DarkGray
    Write-Host "  Folders named ignore-* (e.g. ignore-old) are skipped entirely." -ForegroundColor DarkGray
    Write-Host "  NSFW\ is optional (Win+Shift-X)." -ForegroundColor DarkGray
    Write-Host ""

    if (Read-YesNo "Open a folder browser to pick it?" $true) {
        $WallpaperPath = Invoke-FolderBrowser -Description 'Select the folder that contains your wallpapers'
    }
    if (-not $WallpaperPath) {
        $WallpaperPath = Read-Host "  Paste the folder path (or drag a folder onto this window and press Enter)"
        # strip accidental quotes from drag-drop
        if ($WallpaperPath) { $WallpaperPath = $WallpaperPath.Trim().Trim('"') }
    }
}

$WallpaperPath = Resolve-FullPathSafe $WallpaperPath
if (-not $WallpaperPath) {
    Write-Bad "No wallpaper folder given."
    Wait-IfInteractive
    exit 1
}

if (-not (Test-Path -LiteralPath $WallpaperPath -PathType Container)) {
    if ($Quiet) {
        Write-Bad "Wallpaper folder does not exist: $WallpaperPath"
        exit 1
    }
    Write-Warn "That folder doesn't exist yet: $WallpaperPath"
    if (Read-YesNo "Create it?" $true) {
        New-Item -ItemType Directory -Path $WallpaperPath -Force | Out-Null
        Write-Ok "Created $WallpaperPath"
        Write-Info "Remember to drop some images in there before launching."
    } else {
        Write-Bad "Need an existing wallpaper folder."
        Wait-IfInteractive
        exit 1
    }
}
Write-Ok "Wallpapers: $WallpaperPath"

# --- Step 3: scan ---
if ($Wizard) { Write-Step 3 'Check the folder' }
else {
    Write-Host ""
    Write-Host "  Scanning folder..." -ForegroundColor White
}

$scan = Get-FolderScan -Root $WallpaperPath

Write-Host ""
Write-Host "  What we found:" -ForegroundColor White
Write-Info "Normal (SFW) images : $($scan.SfwCount)"
Write-Info "NSFW-path images    : $($scan.NsfwCount)  (any folder segment named NSFW)"
Write-Info "Nested folders seen : $($scan.NestedDirs)  (max depth $script:ScanMaxDepth)"
if ($scan.IgnoredDirs -gt 0) {
    Write-Info "Ignored ignore-*    : $($scan.IgnoredDirs)"
}
Write-Info "Total usable        : $($scan.TotalSupported)"
Write-Host ""

$hardFail = $false
$warns = 0

if ($scan.TotalSupported -lt 1) {
    Write-Bad "No usable images yet."
    Write-Info "Add .png / .jpg / .jpeg / .bmp files to that folder, then run setup again."
    $hardFail = $true
} elseif ($scan.TotalSupported -eq 1) {
    Write-Warn "Only 1 image — rotation will be a one-hit playlist until you add more."
    $warns++
}

if ($scan.SfwCount -gt 0 -and $scan.SfwCount -lt 10) {
    Write-Warn "Only $($scan.SfwCount) normal images — you may see repeats a bit sooner."
    $warns++
}

if ($scan.Unsupported.Count -gt 0) {
    foreach ($k in $scan.Unsupported.Keys) {
        Write-Warn "$($scan.Unsupported[$k]) file(s) with $k — skipped (not supported)."
        $warns++
    }
    Write-Info "Tip: convert those to jpg or png if you want them in the rotation."
}

if ($scan.NestedDirs -gt 0) {
    Write-Info "Nested folders are included (up to depth $script:ScanMaxDepth)."
}

if ($scan.TooLong.Count -gt 0) {
    Write-Warn "$($scan.TooLong.Count) path(s) are very long — Windows may refuse them as wallpapers."
    $warns++
}

if ($WallpaperPath.StartsWith('\\')) {
    Write-Warn "Network (UNC) path — needs the share online when the app runs."
    $warns++
}

if ($WallpaperPath.Length -gt 200) {
    Write-Warn "Folder path is quite long ($($WallpaperPath.Length) chars)."
    $warns++
}

if ($hardFail) {
    Write-Host ""
    if ($Interactive -and (Read-YesNo "Open the folder in Explorer so you can add images?" $true)) {
        Start-Process explorer.exe -ArgumentList "`"$WallpaperPath`""
    }
    Write-Bad "Fix the folder contents, then re-run setup."
    Wait-IfInteractive
    exit 2
}

if ($warns -eq 0) {
    Write-Ok "Folder looks good."
} else {
    Write-Warn "You can continue — just be aware of the notes above."
    if ($Interactive -and -not $Wizard) {
        if (-not (Read-YesNo "Continue setup anyway?" $true)) {
            Write-Info "Cancelled."
            Wait-IfInteractive
            exit 0
        }
    }
}

# --- Step 4: rotation speed ---
if ($Wizard -or ($Interactive -and -not (Test-Bound 'Minutes'))) {
    if ($Wizard) { Write-Step 4 'How often to change wallpaper' }
    else { Write-Host "" }
    Write-Host "  Auto-rotate interval in minutes (you can always pause later with Win+Alt-P)." -ForegroundColor DarkGray
    Write-Host "  Examples: 1 = every minute · 5 = relaxed · 30 = chill" -ForegroundColor DarkGray
    $Minutes = Read-Number -Prompt "Minutes between changes" -Default $Minutes -Min 1 -Max 1440
}
Write-Ok "Rotation: every $Minutes minute(s)"

# --- Step 5: NSFW folder ---
if ($Wizard) { Write-Step 5 'Optional NSFW folder' }
else { Write-Host "" }

$nsfwPath = Join-Path $WallpaperPath 'NSFW'
if (-not $scan.HasNsfwDir) {
    $doNsfw = $CreateNsfw
    if (-not $doNsfw -and -not $NoNsfwPrompt -and $Interactive) {
        Write-Host "  Any folder named NSFW (anywhere under this tree) counts as NSFW mode." -ForegroundColor DarkGray
        Write-Host "  Hide/show with Win+Shift-X. Completely optional." -ForegroundColor DarkGray
        $doNsfw = Read-YesNo "Create an empty top-level NSFW folder now?" $false
    }
    if ($doNsfw) {
        New-Item -ItemType Directory -Path $nsfwPath -Force | Out-Null
        Write-Ok "Created $nsfwPath"
        Write-Info "You can also use Album\NSFW\... inside nested folders."
    } else {
        Write-Info "Skipped creating NSFW\ (you can add NSFW folders later by hand)."
    }
} else {
    Write-Ok "Top-level NSFW\ present (total NSFW-path images: $($scan.NsfwCount))"
}

# --- Step 6: shortcuts ---
$shortcutBound = (Test-Bound 'Desktop') -or (Test-Bound 'StartMenu') -or (Test-Bound 'Startup')
if ($Interactive -and -not $shortcutBound) {
    if ($Wizard) { Write-Step 6 'Shortcuts (how you start it)' }
    else { Write-Host "" }

    Write-Host "  Pick how you want to start the app:" -ForegroundColor DarkGray
    Write-Host "    1) Desktop shortcut          (recommended)" -ForegroundColor Gray
    Write-Host "    2) Start Menu shortcut" -ForegroundColor Gray
    Write-Host "    3) Start when I log into Windows" -ForegroundColor Gray
    Write-Host "    4) All of the above" -ForegroundColor Gray
    Write-Host "    5) None — I'll run it myself" -ForegroundColor Gray
    Write-Host ""

    $choice = ''
    while ($choice -notmatch '^[1-5]$') {
        $choice = Read-Host "  Choice [1]"
        if ([string]::IsNullOrWhiteSpace($choice)) { $choice = '1' }
        if ($choice -notmatch '^[1-5]$') {
            Write-Host "  Enter 1, 2, 3, 4, or 5." -ForegroundColor DarkGray
        }
    }
    switch ($choice) {
        '1' { $Desktop = $true }
        '2' { $StartMenu = $true }
        '3' { $Startup = $true }
        '4' { $Desktop = $true; $StartMenu = $true; $Startup = $true }
        '5' { }
    }
}

$argWall = if ($WallpaperPath -match '[\s"]') { "`"$WallpaperPath`"" } else { $WallpaperPath }
$arguments = "$argWall $Minutes"
$workDir = Split-Path -Parent $ExePath
$desc = "$AppName — wallpapers every $Minutes min"

if ($Desktop) {
    $desk = [Environment]::GetFolderPath('Desktop')
    $link = Join-Path $desk "$AppName.lnk"
    New-Shortcut -LinkPath $link -TargetPath $ExePath -Arguments $arguments -WorkDir $workDir -Description $desc
    Write-Ok "Desktop shortcut ready"
}
if ($StartMenu) {
    $sm = Join-Path ([Environment]::GetFolderPath('StartMenu')) "Programs\$AppName.lnk"
    New-Shortcut -LinkPath $sm -TargetPath $ExePath -Arguments $arguments -WorkDir $workDir -Description $desc
    Write-Ok "Start Menu shortcut ready"
}
if ($Startup) {
    $su = Join-Path ([Environment]::GetFolderPath('Startup')) "$AppName.lnk"
    New-Shortcut -LinkPath $su -TargetPath $ExePath -Arguments $arguments -WorkDir $workDir -Description $desc
    Write-Ok "Will start at Windows logon"
}

if (-not $Desktop -and -not $StartMenu -and -not $Startup) {
    Write-Info "No shortcuts created. Manual launch:"
    Write-Host "    `"$ExePath`" $arguments" -ForegroundColor DarkGray
}

# --- Step 7: launch ---
if ($Wizard) { Write-Step 7 'Launch' }
else { Write-Host "" }

$doLaunch = $Launch
if (-not $doLaunch -and -not $SkipLaunchPrompt -and $Interactive) {
    $doLaunch = Read-YesNo "Start $AppName now?" $true
}
if ($doLaunch) {
    Start-Process -FilePath $ExePath -ArgumentList $arguments -WorkingDirectory $workDir
    Write-Ok "Launched — look for the icon in the system tray (near the clock)."
    Write-Info "If you don't see it, click the ^ arrow in the tray to show hidden icons."
}

# --- done ---
Write-Host ""
Write-Host "  ===========================================" -ForegroundColor DarkCyan
Write-Host "   You're set!" -ForegroundColor White
Write-Host "  ===========================================" -ForegroundColor DarkCyan
Write-Host ""
Write-Host "  Handy hotkeys" -ForegroundColor White
Write-Host "  -------------------------------------------" -ForegroundColor DarkGray
Write-Host "  Win+Shift-N / B   next / previous on the monitor under the cursor (hold to keep going)"
Write-Host "  Win+Alt-P         pause / resume auto-rotate"
Write-Host "  Win+Alt-U         auto-rotate: All monitors / Round robin / Random"
Write-Host "  Win+Shift-X       NSFW mode (Off / Combined / Only)"
Write-Host "  Win+Shift-L       cycle favorites only"
Write-Host "  Win+Shift-A       save favorite (cursor monitor)"
Write-Host "  Win+Shift-C       clear favorites"
Write-Host "  Win+Shift-Z       show/hide desktop icons"
Write-Host "  Win+Shift-O       open current image (cursor monitor) in Explorer"
Write-Host "  Win+Alt-N         toast notifications on/off"
Write-Host "  Win+Alt-Q         quit"
Write-Host ""
Write-Host "  Multi-monitor: each screen gets its own image. Next/prev/fav/Explorer" -ForegroundColor DarkGray
Write-Host "  follow the cursor. Win+Alt-U only changes how the auto-rotate timer behaves." -ForegroundColor DarkGray
Write-Host "  Skip a folder in your library by renaming it ignore-* (ignore-old, Ignore-WIP, ...)." -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Settings & favorites save automatically to:" -ForegroundColor DarkGray
Write-Host "    $(Join-Path $WallpaperPath 'BackgroundHotkeyThing.ini')" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  More info: http://give.academy/BGHT" -ForegroundColor DarkGray
Write-Host ""
Write-Ok "Done."
Wait-IfInteractive
