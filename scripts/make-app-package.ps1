<#
.SYNOPSIS
	Packages the app-only update (NilesoftShellPro-<version>-x64-app.zip).

.DESCRIPTION
	The in-app updater of an all-users (MSI) install downloads this zip instead of the MSI
	when the Shell core (shell.dll, shell.exe) hasn't changed, and installs it for the
	signed-in user only, without administrator rights (src/settings/src/AppChannel.h).

	It holds what the app reads from its own folder: NilesoftShellPro.exe, and the shipped
	configuration it brings personal configs up to date with (shell.nss, imports\**\*.nss)
	and snippets\ (the same as in the portable zip). No shell.dll or shell.exe: those stay
	the installed ones.

	release.yml lists every file of it, with its SHA-256, in update.json; the app installs
	nothing that doesn't match.

.PARAMETER BinDir
	Build output folder. Default: src\bin in this repository.

.PARAMETER OutDir
	Where to write the zip. Default: the same as BinDir.

.PARAMETER Version
	Version in the file name, e.g. 2.2.0 or 2.2.0-beta.1. Default: VERSION_MAJOR,
	VERSION_MINOR and VERSION_BUILD from src\shared\Resource.h.

.EXAMPLE
	pwsh scripts\make-app-package.ps1 -Version 2.2.0 -OutDir dist
#>
[CmdletBinding()]
param(
	[string]$BinDir,
	[string]$OutDir,
	[string]$Version
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

foreach ($required in 'NilesoftShellPro.exe', 'shell.nss') {
	if (-not (Test-Path -LiteralPath (Join-Path $BinDir $required) -PathType Leaf)) {
		throw "Missing in ${BinDir}: $required. Build src\settings (cmake --preset x64-release; cmake --build --preset x64-release) first."
	}
}
if (-not (Test-Path -LiteralPath (Join-Path $BinDir 'imports') -PathType Container)) { throw "Missing in ${BinDir}: imports\" }

$zip = Join-Path $OutDir "NilesoftShellPro-$Version-x64-app.zip"
$stage = Join-Path ([IO.Path]::GetTempPath()) ("nsp-app-$Version-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null

try {
	Copy-Item -LiteralPath (Join-Path $BinDir 'NilesoftShellPro.exe') -Destination $stage
	Copy-Item -LiteralPath (Join-Path $BinDir 'shell.nss') -Destination $stage
	$importsSrc = Join-Path $BinDir 'imports'
	Get-ChildItem -LiteralPath $importsSrc -Recurse -File -Filter *.nss | ForEach-Object {
		$rel = $_.FullName.Substring($importsSrc.Length).TrimStart('\')
		$dest = Join-Path (Join-Path $stage 'imports') $rel
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
		Copy-Item -LiteralPath $_.FullName -Destination $dest
	}
	$snippetsSrc = Join-Path $BinDir 'snippets'
	foreach ($sub in @('', 'rubicbg')) {
		$from = Join-Path $snippetsSrc $sub
		if (-not (Test-Path -LiteralPath $from -PathType Container)) { continue }
		$to = Join-Path (Join-Path $stage 'snippets') $sub
		New-Item -ItemType Directory -Force -Path $to | Out-Null
		Get-ChildItem -LiteralPath $from -File |
			Where-Object { $_.Extension -eq '.nss' -or $_.Name -eq 'catalog.json' -or ($sub -and $_.Name -eq 'LICENSE.txt') } |
			Copy-Item -Destination $to
	}

	if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
}
finally {
	Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Wrote $zip"
$zip
