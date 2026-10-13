<#
.SYNOPSIS
	Fails if any menu title in the shipped configuration evaluates empty.

.DESCRIPTION
	Runs `shell.exe -config:check <root> -titles:<where>` from the build output
	for every context (file, folder, back, desktop, taskbar, and all titles) on:

	  (a) the shipped root, src\bin\shell.nss;
	  (b) an older root shell.nss with no `import lang` block (as kept on an
	      upgraded machine, and as seeded by the user's own config repo) next to
	      the shipped imports, whose titles are loc.<name>. shell.dll must fall
	      back to imports\lang\en.nss there;
	  (c) (b) after `shell.exe -config:init` in a scratch %APPDATA%: the root
	      must gain the localization block, keep the original as shell.nss.old,
	      and load with no warnings.

	Everything happens in a temporary folder: nothing is registered, installed
	or written outside it, and the real %APPDATA% is not touched.

.PARAMETER BinDir
	Build output folder with shell.exe, shell.dll, shell.nss and imports\.
	Default: src\bin in this repository.

.EXAMPLE
	pwsh scripts\check-titles.ps1
#>
[CmdletBinding()]
param(
	[string]$BinDir = (Join-Path $PSScriptRoot '..\src\bin')
)

$ErrorActionPreference = 'Stop'
$BinDir = (Resolve-Path $BinDir).Path
foreach ($name in 'shell.exe', 'shell.dll', 'shell.nss', 'imports') {
	if (-not (Test-Path (Join-Path $BinDir $name))) { throw "$name not found in $BinDir; build src\Shell.sln first" }
}

# The default shell.nss before the imports were localized (and the user's own
# config repo's): settings, then imports, no language file.
$oldRoot = @'
settings
{
	priority=1
	exclude.where = !process.is_explorer
	showdelay = 200
	// Options to allow modification of system items
	modify.remove.duplicate=1
	tip.enabled=true
}

import 'imports/theme.nss'
import 'imports/images.nss'

import 'imports/modify.nss'

menu(mode="multiple" title="Pin/Unpin" image=icon.pin)
{
}

menu(mode="multiple" title=title.more_options image=icon.more_options)
{
}

import 'imports/terminal.nss'
import 'imports/file-manage.nss'
import 'imports/develop.nss'
import 'imports/goto.nss'
import 'imports/taskbar.nss'
import 'imports/my-tools.nss'
'@ -replace "`r?`n", "`r`n"

$utf8Bom = New-Object System.Text.UTF8Encoding($true)
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("shell-titles-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
$failures = [Collections.Generic.List[string]]::new()

# shell.exe is a GUI-subsystem program: Start-Process -Wait for its exit code.
function Invoke-Shell([string]$exe, [string[]]$arguments, [hashtable]$environment = @{}) {
	$saved = @{}
	foreach ($k in $environment.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $environment[$k]) }
	try {
		$p = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
		return $p.ExitCode
	}
	finally {
		foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
	}
}

function Test-Titles([string]$label, [string]$root, [switch]$NoWarnings) {
	foreach ($where in '', 'file', 'folder', 'back', 'desktop', 'taskbar') {
		$out = Join-Path $scratch 'titles.json'
		Remove-Item $out -ErrorAction SilentlyContinue
		$titles = if ($where) { "-titles:$where" } else { '-titles' }
		$code = Invoke-Shell (Join-Path $BinDir 'shell.exe') @('-silent', '-config:check', "`"$root`"", $titles, "`"-out:$out`"")
		$name = if ($where) { $where } else { 'all' }
		if (-not (Test-Path $out)) { $failures.Add("$label [$name]: no output (exit $code)"); continue }
		$r = Get-Content $out -Raw -Encoding utf8 | ConvertFrom-Json
		$text = @($r.titles | Where-Object state -eq 'text').Count
		$empty = @($r.titles | Where-Object state -eq 'empty')
		'{0,-28} {1,-8} ok={2} titles={3} text={4} empty={5} warnings={6}' -f $label, $name, $r.ok, @($r.titles).Count, $text, $empty.Count, @($r.warnings).Count
		if (-not $r.ok -or $code -ne 0) { $failures.Add("$label [$name]: the configuration does not load: $($r.errors | ConvertTo-Json -Compress)") }
		foreach ($e in $empty) { $failures.Add("$label [$name]: empty title $($e.source) at $($e.file):$($e.line)") }
		if ($text -lt 10) { $failures.Add("$label [$name]: only $text titles evaluated") }
		if ($NoWarnings -and @($r.warnings).Count) { $failures.Add("$label [$name]: warnings: $($r.warnings.message -join '; ')") }
	}
}

try {
	# (a) the shipped root
	Test-Titles 'shipped shell.nss' (Join-Path $BinDir 'shell.nss') -NoWarnings

	# (b) an older root next to the shipped imports
	$old = Join-Path $scratch 'old'
	New-Item -ItemType Directory $old | Out-Null
	Copy-Item (Join-Path $BinDir 'imports') (Join-Path $old 'imports') -Recurse
	[IO.File]::WriteAllText((Join-Path $old 'shell.nss'), $oldRoot, $utf8Bom)
	Test-Titles 'old shell.nss (no lang)' (Join-Path $old 'shell.nss')

	# (c) -config:init on that older root, in a scratch install and %APPDATA%
	$install = Join-Path $scratch 'install'
	$appdata = Join-Path $scratch 'appdata'
	New-Item -ItemType Directory $install, $appdata | Out-Null
	Copy-Item (Join-Path $BinDir 'shell.exe'), (Join-Path $BinDir 'shell.dll') $install
	Copy-Item (Join-Path $BinDir 'imports') (Join-Path $install 'imports') -Recurse
	[IO.File]::WriteAllText((Join-Path $install 'shell.nss'), $oldRoot, $utf8Bom)
	$code = Invoke-Shell (Join-Path $install 'shell.exe') @('-silent', '-config:init', "`"-out:$(Join-Path $scratch 'init.txt')`"") @{ APPDATA = $appdata }
	$userRoot = Join-Path $appdata 'Nilesoft\Shell\shell.nss'
	if ($code -ne 0 -or -not (Test-Path $userRoot)) { $failures.Add("-config:init failed (exit $code)") }
	else {
		Get-Content (Join-Path $scratch 'init.txt')
		$seeded = Get-Content $userRoot -Raw
		if ($seeded -notmatch '(?m)^import lang ') { $failures.Add('-config:init did not add the localization block') }
		if (-not (Test-Path "$userRoot.old") -or [IO.File]::ReadAllText("$userRoot.old") -ne [IO.File]::ReadAllText((Join-Path $install 'shell.nss'))) {
			$failures.Add('-config:init did not keep the original as shell.nss.old')
		}
		Test-Titles 'old shell.nss after init' $userRoot -NoWarnings
	}
}
finally {
	Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count) {
	$failures | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }
	exit 1
}
Write-Host 'No menu title is empty.'
