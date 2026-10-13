<#
.SYNOPSIS
	Packages the portable (per-user, no admin) Nilesoft Shell Pro zip.

.DESCRIPTION
	Collects shell.dll, shell.exe, settings.exe, shell.nss, imports\**, the license and
	the readme from the build output folder (src\bin by default), adds PORTABLE.txt
	and writes NilesoftShellPro-<version>-<arch>-portable.zip.

	Build the core (src\Shell.sln) and the Settings app (src\settings) first; the
	script stops if a required file is missing. It only reads the build output and
	writes the zip: nothing is registered or installed.

.PARAMETER BinDir
	Build output folder. Default: src\bin in this repository.

.PARAMETER OutDir
	Where to write the zip. Default: the same as BinDir.

.PARAMETER Version
	Version in the file name, e.g. 2.0.0 or 2.0.0-beta.1. Default: VERSION_MAJOR,
	VERSION_MINOR and VERSION_BUILD from src\shared\Resource.h.

.PARAMETER Arch
	Architecture in the file name. Only x64 is built and tested today.

.EXAMPLE
	pwsh scripts\make-portable.ps1
	pwsh scripts\make-portable.ps1 -Version 2.0.0-beta.1 -OutDir dist
#>
[CmdletBinding()]
param(
	[string]$BinDir,
	[string]$OutDir,
	[string]$Version,
	[ValidateSet('x64')]
	[string]$Arch = 'x64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $BinDir) { $BinDir = Join-Path $repo 'src\bin' }
$BinDir = (Resolve-Path -LiteralPath $BinDir).Path
if (-not $OutDir) { $OutDir = $BinDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

if (-not $Version) {
	$header = Get-Content -Raw -LiteralPath (Join-Path $repo 'src\shared\Resource.h')
	$parts = foreach ($name in 'MAJOR', 'MINOR', 'BUILD') {
		$m = [regex]::Match($header, "#define\s+VERSION_$name\s+(\d+)")
		if (-not $m.Success) { throw "VERSION_$name not found in src\shared\Resource.h" }
		$m.Groups[1].Value
	}
	$Version = $parts -join '.'
}
$Version = $Version.TrimStart('v')

# source (relative to BinDir) -> name in the zip
$files = [ordered]@{
	'shell.dll'    = 'shell.dll'
	'shell.exe'    = 'shell.exe'
	'settings.exe' = 'settings.exe'
	'shell.nss'    = 'shell.nss'
	'license.txt'  = 'LICENSE.txt'
	'readme.txt'   = 'readme.txt'
}
$missing = @($files.Keys | Where-Object { -not (Test-Path -LiteralPath (Join-Path $BinDir $_) -PathType Leaf) })
if (-not (Test-Path -LiteralPath (Join-Path $BinDir 'imports') -PathType Container)) { $missing += 'imports\' }
if ($missing) {
	throw ("Missing in ${BinDir}: $($missing -join ', '). Build src\Shell.sln (Release, x64) " +
		"and src\settings (cmake --preset x64-release; cmake --build --preset x64-release) first.")
}

$name = "NilesoftShellPro-$Version-$Arch-portable"
$zip = Join-Path $OutDir "$name.zip"
$stage = Join-Path ([IO.Path]::GetTempPath()) ("$name-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null

try {
	foreach ($src in $files.Keys) {
		Copy-Item -LiteralPath (Join-Path $BinDir $src) -Destination (Join-Path $stage $files[$src])
	}
	# imports\** (only .nss: nothing else belongs there)
	$importsSrc = Join-Path $BinDir 'imports'
	Get-ChildItem -LiteralPath $importsSrc -Recurse -File -Filter *.nss | ForEach-Object {
		$rel = $_.FullName.Substring($importsSrc.Length).TrimStart('\')
		$dest = Join-Path (Join-Path $stage 'imports') $rel
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
		Copy-Item -LiteralPath $_.FullName -Destination $dest
	}

	$portable = @"
Nilesoft Shell Pro $Version - portable ($Arch)
=================================================

A per-user copy of Nilesoft Shell Pro. Nothing here needs administrator rights.

Install
-------
1. Extract this zip to a folder you keep, for example
       %LOCALAPPDATA%\Programs\NilesoftShellPro
   Explorer loads shell.dll from this folder, so don't extract it to Downloads
   or a temporary folder, and don't move it while Shell is registered.
2. Run settings.exe, enable Shell for yourself (a per-user registration),
   then restart Explorer from the app.
   Or, from a command prompt in that folder:
       shell.exe -register -user -treat
       shell.exe -restart
   (-treat shows the classic context menu with Shell on Windows 11.)

Configuration
-------------
shell.nss and imports\ in this folder are the defaults. "shell.exe -config:init"
copies them to %APPDATA%\Nilesoft\Shell, which Shell then reads instead.
shell.exe -status prints the current state as JSON.

Update
------
Unregister and restart Explorer (see below), replace the files, register again.
shell.dll is locked while Explorer has it loaded.

Remove
------
    shell.exe -unregister -user
    shell.exe -restart
Then delete the folder (and %APPDATA%\Nilesoft\Shell if you no longer want
your configuration).

Notes
-----
- If Nilesoft Shell is also installed for all users (the MSI), the per-user
  registration takes precedence for this user.
- The binaries are not code-signed; SmartScreen may warn the first time you
  run shell.exe or settings.exe.
- settings.exe is an x64 program; on Windows on ARM it runs under emulation.
"@
	Set-Content -LiteralPath (Join-Path $stage 'PORTABLE.txt') -Value ($portable -replace "`r?`n", "`r`n") -Encoding utf8 -NoNewline

	if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
}
finally {
	Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Wrote $zip"
$zip
