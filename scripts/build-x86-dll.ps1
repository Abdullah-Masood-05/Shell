<#
.SYNOPSIS
	Builds the 32-bit shell.dll and copies it to src\bin\shell.x86.dll.

.DESCRIPTION
	32-bit programs on 64-bit Windows (for example the Open/Save dialog of a 32-bit app)
	cannot load the 64-bit shell.dll, so the x64 install also ships a 32-bit copy as
	shell.x86.dll, next to shell.dll. `shell.exe -register` points the 32-bit registry
	view at it when the file exists.

	Both builds write src\bin\shell.dll, so the Win32 DLL is built into its own output and
	intermediate folders (src\bin\x86\ and src\dll\obj\x86\release\) and only then copied.
	Run this before building src\Shell.sln for x64: the MSI and the portable zip pick the
	file up from src\bin. Nothing is registered or installed.

.PARAMETER MSBuild
	Path to MSBuild.exe. Default: msbuild from PATH.

.EXAMPLE
	pwsh scripts\build-x86-dll.ps1
#>
[CmdletBinding()]
param(
	[string]$MSBuild = 'msbuild'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repo 'src'
$bin = Join-Path $src 'bin'
$out = Join-Path $bin 'x86'
$obj = Join-Path $src 'dll\obj\x86\release'
New-Item -ItemType Directory -Force -Path $out, $obj | Out-Null

# SolutionDir and the folders need their trailing backslash (the double one keeps the
# quote of the command line from being escaped).
& $MSBuild (Join-Path $src 'dll\Shell.vcxproj') -p:Configuration=release -p:Platform=Win32 `
	"-p:SolutionDir=$src\\" "-p:OutDir=$out\\" "-p:IntDir=$obj\\" -m -v:m -nologo
if ($LASTEXITCODE) { throw "msbuild failed with exit code $LASTEXITCODE" }

$built = Join-Path $out 'shell.dll'
if (-not (Test-Path -LiteralPath $built -PathType Leaf)) { throw "$built was not produced" }
Copy-Item -LiteralPath $built -Destination (Join-Path $bin 'shell.x86.dll') -Force
Write-Host "src\bin\shell.x86.dll <- $built"
