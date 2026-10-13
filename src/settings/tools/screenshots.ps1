<#
.SYNOPSIS
    Renders the Settings app's pages to PNG files for the docs and the README.

.DESCRIPTION
    Builds nothing. Runs src\bin\settings.exe with its --snapshot switch, which renders the
    app's own window to <prefix>-N.bmp files and exits (no screen capture involved), then
    converts the BMPs to PNG in docs\images\settings-app.

    The app runs against a throw-away %APPDATA%: the script creates a temporary folder, sets
    APPDATA for the child processes, runs `shell.exe -silent -config:init` there and adds two
    backup snapshots and a small edit, so the pages show realistic data. Your own
    configuration is not read or written. The app reads (never writes) HKCU for its theme and
    window placement; the Home page shows the real registration state of this machine.

    Pages and themes come from the tour in src\settings\src\main.cpp (--snapshot):
      shot 1  the start page (--page)       shot 4  Appearance
      shot 2  Home                          shot 5  Home in the light theme
      shot 3  Preferences
    Shots 1 to 4 use the app's normal theme, which is the Windows app mode unless changed in
    Preferences; the "-dark" names assume Windows is in dark mode (the script warns if not).
    Shot 4 is kept only once Appearance is no longer a PlaceholderPage in ui\app.slint;
    the other "coming soon" pages are not captured. Each run starts settings.exe once
    (two runs in total). If the tour changes, update $Runs below.

    Run it again after UI changes. Build first if settings.exe is missing:
      cd src\settings; cmake --preset x64-release; cmake --build --preset x64-release

.PARAMETER OutDir
    Where the PNGs go. Default: docs\images\settings-app in the repository.

.PARAMETER MaxWidth
    Wider images are scaled down to this width. Default 1280.

.PARAMETER Only
    Produce only these images (names without .png, e.g. home-light); runs that produce none of
    them are skipped, so the app starts fewer times.

.PARAMETER WorkDir
    A new folder to use instead of one in %TEMP%. Home and the Config Editor show the config
    path, so a short neutral folder (for example D:\Projects\Demo) keeps your user name and the
    temp path out of the images. It must not exist yet and is deleted afterwards.

.PARAMETER KeepTemp
    Keep the temporary APPDATA and the BMP files (the path is printed).

.EXAMPLE
    pwsh -File src\settings\tools\screenshots.ps1 -WorkDir D:\Projects\Demo

.EXAMPLE
    pwsh -File src\settings\tools\screenshots.ps1 -Only backups-dark
#>
[CmdletBinding()]
param(
    [string]$OutDir,
    [int]$MaxWidth = 1280,
    [string[]]$Only,
    [string]$WorkDir,
    [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$bin = Join-Path $repo 'src\bin'
$settingsExe = Join-Path $bin 'settings.exe'
$shellExe = Join-Path $bin 'shell.exe'
if (-not $OutDir) { $OutDir = Join-Path $repo 'docs\images\settings-app' }

foreach ($exe in $settingsExe, $shellExe) {
    if (-not (Test-Path $exe)) {
        throw "$exe not found. Build the core (src\Shell.sln) and the app (src\settings, CMake preset x64-release) first."
    }
}

# settings.exe allows one window per session: a second start only brings the first forward.
if (Get-Process -Name settings -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -eq 'Nilesoft Shell Settings' -or $_.Path -eq $settingsExe }) {
    throw 'Nilesoft Shell Settings is already running. Close it and run this script again.'
}

# The window opens at its saved size and position (HKCU, read only); a maximized window
# gives full-screen images.
$appKey = 'HKCU:\Software\Nilesoft\Shell\Settings'
if ((Get-ItemProperty $appKey -Name WindowMaximized -ErrorAction SilentlyContinue).WindowMaximized -eq 1) {
    Write-Warning 'The app was last closed maximized and will open maximized. Restore it to a normal size and close it to get normal-size images.'
}

# Pages that are still placeholders are not captured (see ui\app.slint).
$appSlint = Get-Content -Raw (Join-Path $repo 'src\settings\ui\app.slint')
$appearanceReady = $appSlint -notmatch 'Pages\.appearance:\s*PlaceholderPage'

# Each run: arguments, and which shot number becomes which file.
$Runs = @(
    @{ Args = @('--page', 'editor'); Shots = @{ '1' = 'editor-dark'; '2' = 'home-dark'; '3' = 'preferences-dark'; '5' = 'home-light' } },
    @{ Args = @('--page', 'backups'); Shots = @{ '1' = 'backups-dark' } }
)
if ($appearanceReady) {
    $Runs[0].Shots['4'] = 'appearance-dark'
    Write-Warning 'The Appearance page is implemented, but the --snapshot tour only renders it in the dark theme; appearance-light.png is not produced.'
}

if ($Only) {
    # pwsh -File passes "a,b" as one string.
    $Only = @($Only -split ',' | ForEach-Object { $_.Trim() -replace '\.png$', '' } | Where-Object { $_ })
    foreach ($run in $Runs) {
        foreach ($k in @($run.Shots.Keys)) { if ($run.Shots[$k] -notin $Only) { $run.Shots.Remove($k) } }
    }
    $Runs = @($Runs | Where-Object { $_.Shots.Count -gt 0 })
    if (-not $Runs) { throw "No run produces any of: $($Only -join ', ')" }
}

# The "-dark" file names need the app in dark mode.
$personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$lightApps = (Get-ItemProperty $personalize -Name AppsUseLightTheme -ErrorAction SilentlyContinue).AppsUseLightTheme
$appTheme = (Get-ItemProperty $appKey -Name Theme -ErrorAction SilentlyContinue).Theme
$dark = ($appTheme -eq 2) -or ((-not $appTheme) -and ($lightApps -eq 0))
if (-not $dark) {
    Write-Warning 'The app will not start in dark mode (Windows app mode or the app''s Theme preference is light); the "-dark" images will be light.'
}

# --- a throw-away APPDATA with a config, two snapshots and an unsaved-to-snapshot edit ---

if ($WorkDir) {
    $work = [IO.Path]::GetFullPath($WorkDir)
    if (Test-Path $work) { throw "$work already exists. -WorkDir must name a new folder; it is deleted afterwards." }
} else {
    $work = Join-Path ([IO.Path]::GetTempPath()) ('nss-shots-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}
$appData = Join-Path $work 'AppData\Roaming'
New-Item -ItemType Directory -Force $appData | Out-Null
$savedAppData = $env:APPDATA

# Start-Process does not quote list arguments, and %TEMP% often contains a space.
function Join-Arguments([string[]]$list) {
    ($list | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }) -join ' '
}

function Invoke-Tool([string]$exe, [string[]]$arguments, [int]$timeoutSec) {
    $p = Start-Process -FilePath $exe -ArgumentList (Join-Arguments $arguments) -WorkingDirectory $bin -PassThru
    $null = $p.Handle  # keeps ExitCode available after exit
    if (-not $p.WaitForExit($timeoutSec * 1000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        throw "$(Split-Path $exe -Leaf) $($arguments -join ' ') did not exit within $timeoutSec s and was stopped."
    }
    $p.ExitCode
}

function New-Snapshot([string]$configDir, [datetime]$time, [string]$reason, [bool]$pinned) {
    $backups = Join-Path $configDir 'backups'
    $id = $time.ToString('yyyyMMdd-HHmmss')
    $dir = Join-Path $backups $id
    New-Item -ItemType Directory -Force $dir | Out-Null
    $sha = [Security.Cryptography.SHA256]::Create()
    $files = foreach ($f in Get-ChildItem $configDir -Recurse -File -Filter *.nss |
            Where-Object { -not $_.FullName.StartsWith($backups + '\', [StringComparison]::OrdinalIgnoreCase) } |
            Sort-Object FullName) {
        $rel = $f.FullName.Substring($configDir.Length + 1)
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        $target = Join-Path $dir $rel
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        [IO.File]::WriteAllBytes($target, $bytes)
        $hash = -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') })
        '    { "path": "' + $rel.Replace('\', '/') + '", "sha256": "' + $hash + '", "size": ' + $bytes.Length + ' }'
    }
    $manifest = "{`r`n  `"version`": 1,`r`n  `"id`": `"$id`",`r`n  `"created`": `"$($time.ToString('yyyy-MM-ddTHH:mm:ss'))`",`r`n" +
        "  `"reason`": `"$reason`",`r`n  `"root`": `"shell.nss`",`r`n  `"pinned`": $($pinned.ToString().ToLower()),`r`n" +
        "  `"files`": [`r`n" + ($files -join ",`r`n") + "`r`n  ]`r`n}`r`n"
    [IO.File]::WriteAllText((Join-Path $dir 'manifest.json'), $manifest, [Text.UTF8Encoding]::new($false))
}

function Save-Png([string]$bmpPath, [string]$pngPath) {
    $src = [Drawing.Image]::FromFile($bmpPath)
    try {
        $w = $src.Width; $h = $src.Height
        if ($w -gt $MaxWidth) { $h = [int][Math]::Round($h * $MaxWidth / $w); $w = $MaxWidth }
        # 24-bit: the window is opaque, and dropping alpha keeps the files smaller.
        $dst = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
        try {
            $g = [Drawing.Graphics]::FromImage($dst)
            $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
            $g.DrawImage($src, 0, 0, $w, $h)
            $g.Dispose()
            $dst.Save($pngPath, [Drawing.Imaging.ImageFormat]::Png)
        } finally { $dst.Dispose() }
    } finally { $src.Dispose() }
    # Recompress losslessly when a PNG optimiser is installed.
    if (Get-Command oxipng -ErrorAction SilentlyContinue) {
        & oxipng -q -o 3 --strip safe $pngPath
    } elseif (Get-Command magick -ErrorAction SilentlyContinue) {
        & magick $pngPath -strip -define png:compression-level=9 -define png:compression-filter=5 $pngPath
    }
}

$launched = @()
try {
    $env:APPDATA = $appData
    $code = Invoke-Tool $shellExe @('-silent', '-config:init') 60
    $configDir = Join-Path $appData 'Nilesoft\Shell'
    $root = Join-Path $configDir 'shell.nss'
    if ($code -ne 0 -or -not (Test-Path $root)) { throw "shell.exe -config:init failed (exit code $code)." }

    New-Snapshot $configDir (Get-Date).AddDays(-3).AddHours(-2) 'Manual snapshot' $true
    New-Snapshot $configDir (Get-Date).AddHours(-1) 'Config Editor: shell.nss' $false
    # An edit after the last snapshot, so the Backups page has a diff to show. Keeps the
    # file's byte order mark (or lack of one), as the app does.
    $bytes = [IO.File]::ReadAllBytes($root)
    $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $text = [Text.Encoding]::UTF8.GetString($bytes, $(if ($bom) { 3 } else { 0 }), $bytes.Length - $(if ($bom) { 3 } else { 0 }))
    if ($text -notmatch '// showdelay = 200') { Write-Warning 'shell.nss has no "// showdelay = 200" line; the Backups page will show no changes.' }
    [IO.File]::WriteAllText($root, $text.Replace('// showdelay = 200', 'showdelay = 200'), [Text.UTF8Encoding]::new($bom))

    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $n = 0
    foreach ($run in $Runs) {
        $n++
        $prefix = Join-Path $work "run$n"
        $arguments = @('--no-onboarding') + $run.Args + @('--snapshot', $prefix)
        Write-Host "settings.exe $($run.Args -join ' ')"
        $p = Start-Process -FilePath $settingsExe -ArgumentList (Join-Arguments $arguments) -WorkingDirectory $bin -PassThru
        $launched += $p
        # The tour takes about 10 s (a shot every 1.2 s after a short wait).
        if (-not $p.WaitForExit(60000)) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            throw 'settings.exe did not finish the snapshot tour within 60 s and was stopped.'
        }
        foreach ($shot in ($run.Shots.Keys | Sort-Object)) {
            $bmp = "$prefix-$shot.bmp"
            if (-not (Test-Path $bmp)) { throw "settings.exe did not write $bmp." }
            $png = Join-Path $OutDir ($run.Shots[$shot] + '.png')
            Save-Png $bmp $png
            $img = [Drawing.Image]::FromFile($png)
            Write-Host ('  {0,-22} {1}x{2}  {3:N0} KB' -f (Split-Path $png -Leaf), $img.Width, $img.Height, ((Get-Item $png).Length / 1KB))
            $img.Dispose()
        }
    }
} finally {
    $env:APPDATA = $savedAppData
    foreach ($p in $launched) {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    if ($KeepTemp) { Write-Host "Kept $work" }
    else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
