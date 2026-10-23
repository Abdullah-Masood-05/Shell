<#
.SYNOPSIS
	Checks `shell.exe -config:tree` on the shipped configuration for every context.

.DESCRIPTION
	Runs `shell.exe -config:tree <root> -context:<where> -sample:<path>` from the
	build output for file, folder, drive, back, desktop and taskbar, on the
	shipped root src\bin\shell.nss, and checks the JSON the app's Menu map reads:

	  - the config loads (ok, no errors) and a tree comes back;
	  - every one of Shell's items and every modify/remove rule has a file and
	    a line, and the line really starts that declaration (menu, item, sep,
	    separator, modify or remove);
	  - the Windows menu was built for every context but the taskbar, and the
	    shipped rule that removes "Restore previous versions" reaches it for a
	    folder.

	The menus are built off-screen inside shell.exe; nothing is shown,
	registered or written outside a temporary folder (the sample file and
	folder), and the real %APPDATA% is not touched.

.PARAMETER BinDir
	Build output folder with shell.exe, shell.dll, shell.nss and imports\.
	Default: src\bin in this repository.

.EXAMPLE
	pwsh scripts\check-menu-tree.ps1
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

$temp = Join-Path ([IO.Path]::GetTempPath()) ("shell-tree-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temp | Out-Null
$sampleFile = Join-Path $temp 'sample.txt'
$sampleFolder = Join-Path $temp 'folder'
Set-Content $sampleFile 'sample'
New-Item -ItemType Directory $sampleFolder | Out-Null
$root = Join-Path $BinDir 'shell.nss'

$contexts = [ordered]@{
	file    = $sampleFile
	folder  = $sampleFolder
	drive   = $env:SystemDrive + '\'
	back    = $sampleFolder
	desktop = ''
	taskbar = ''
}

$failures = @()
$fileLines = @{}
function Get-Line([string]$path, [int]$line) {
	if (-not $fileLines.ContainsKey($path)) { $fileLines[$path] = [IO.File]::ReadAllLines($path) }
	$lines = $fileLines[$path]
	if ($line -lt 1 -or $line -gt $lines.Count) { return $null }
	return $lines[$line - 1]
}
function Test-Declared($node, [string]$what, [int]$column) {
	if (-not $node.file -or $node.line -lt 1) { return "$what has no file or line" }
	$text = Get-Line $node.file $node.line
	if ($null -eq $text) { return "$what points past the end of $($node.file)" }
	$at = $text.Substring([Math]::Min([Math]::Max($column - 1, 0), $text.Length))
	if ($at -notmatch '^(menu|item|sep|separator|modify|remove)\b') { return "$what at $($node.file):$($node.line) does not start a declaration: '$($text.Trim())'" }
	return $null
}
function Walk($items, [scriptblock]$fn) {
	foreach ($i in $items) { & $fn $i; if ($i.items) { Walk $i.items $fn } }
}

try {
	foreach ($where in $contexts.Keys) {
		$out = Join-Path $temp "tree-$where.json"
		$argList = @('-config:tree', "`"$root`"", "-context:$where")
		if ($contexts[$where]) { $argList += "`"-sample:$($contexts[$where])`"" }
		$argList += "`"-out:$out`"", '-silent'
		$p = Start-Process -FilePath (Join-Path $BinDir 'shell.exe') -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
		$json = Get-Content $out -Raw -Encoding utf8 | ConvertFrom-Json
		$problems = @()
		if ($p.ExitCode -ne 0 -or -not $json.ok) { $problems += "exit $($p.ExitCode), ok=$($json.ok), errors: $($json.errors | ConvertTo-Json -Compress)" }
		$tree = $json.tree
		if (-not $tree) { $problems += 'no tree' }
		else {
			$shellCount = 0; $systemCount = 0
			Walk $tree.shell { param($n) $script:shellCount++; $e = Test-Declared $n "Shell item '$($n.title)'" $n.column; if ($e) { $script:problems += $e } }
			foreach ($r in $tree.rules) { $e = Test-Declared $r "rule $($r.index)" $r.column; if ($e) { $problems += $e } }
			Walk $tree.system { param($n) $script:systemCount++ }
			if ($where -ne 'taskbar' -and $systemCount -eq 0) { $problems += "no Windows items ($($tree.system_error))" }
			if ($where -eq 'taskbar' -and $tree.system_error -ne 'taskbar') { $problems += 'taskbar: system_error should say taskbar' }
			if ($where -eq 'folder') {
				$restore = @(); Walk $tree.system { param($n) if ($n.id -eq 'restore_previous_versions') { $script:restore += $n } }
				if ($restore.Count -and -not $restore[0].hidden) { $problems += 'Restore previous versions is not marked hidden by modify.nss' }
			}
			'{0,-8} ok={1} shell={2} windows={3} rules={4}' -f $where, $json.ok, $shellCount, $systemCount, @($tree.rules).Count
		}
		foreach ($pr in $problems) { "  FAIL $where`: $pr"; $failures += "$where`: $pr" }
	}
}
finally {
	Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}

if ($failures.Count) { throw "$($failures.Count) problem(s) in -config:tree output" }
'-config:tree output is complete for every context.'
