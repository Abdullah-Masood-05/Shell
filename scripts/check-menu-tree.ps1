<#
.SYNOPSIS
	Checks `shell.exe -config:tree` on the shipped configuration for every context.

.DESCRIPTION
	Runs `shell.exe -config:tree <root> -context:<where> -sample:<path>` from the
	build output for file, folder, drive, back, desktop, This PC and taskbar, on
	the shipped root src\bin\shell.nss, and checks the JSON the app's Menu map reads:

	  - the config loads (ok, no errors) and a tree comes back;
	  - every one of Shell's items and every modify/remove rule has a file and
	    a line, and the line really starts that declaration (menu, item, sep,
	    separator, modify or remove);
	  - the Windows menu was built for every context but the taskbar, and the
	    shipped rule that removes "Restore previous versions" reaches it for a
	    folder;
	  - every one of Shell's items says whether it shows here (yes, no, maybe).

	Then a small config of its own checks that conditions are worked out against
	the sample's selection, as the real menu would: sel.raw for This PC, the
	sample's extension, sel.count, window.is_taskbar, a menu that isn't there,
	keys (only known when the menu opens), and a remove() with sel.raw.

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
	thispc  = ''
	taskbar = ''
}

function Get-Tree([string]$rootFile, [string]$where, [string]$sample) {
	$out = Join-Path $temp ("tree-{0}-{1}.json" -f $where, [Guid]::NewGuid().ToString('N'))
	$argList = @('-config:tree', "`"$rootFile`"", "-context:$where")
	if ($sample) { $argList += "`"-sample:$sample`"" }
	$argList += "`"-out:$out`"", '-silent'
	$p = Start-Process -FilePath (Join-Path $BinDir 'shell.exe') -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
	$json = Get-Content $out -Raw -Encoding utf8 | ConvertFrom-Json
	return @{ exit = $p.ExitCode; json = $json }
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
		$r = Get-Tree $root $where $contexts[$where]
		$json = $r.json
		$problems = @()
		if ($r.exit -ne 0 -or -not $json.ok) { $problems += "exit $($r.exit), ok=$($json.ok), errors: $($json.errors | ConvertTo-Json -Compress)" }
		$tree = $json.tree
		if (-not $tree) { $problems += 'no tree' }
		else {
			$shellCount = 0; $systemCount = 0
			Walk $tree.shell { param($n) $script:shellCount++; $e = Test-Declared $n "Shell item '$($n.title)'" $n.column; if ($e) { $script:problems += $e }
				if ($n.shows -notin 'yes', 'no', 'maybe') { $script:problems += "Shell item '$($n.title)' has no shows ('$($n.shows)')" } }
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

	# Conditions worked out against the sample.
	$own = Join-Path $temp 'own'
	New-Item -ItemType Directory $own | Out-Null
	$ownRoot = Join-Path $own 'shell.nss'
	$pc = '::{20D04FE0-3AEA-1069-A2D8-08002B30309D}'
	@"
item(title='PC only' type='*' where=sel.raw=='$pc' cmd='x')
item(title='Not on PC' type='*' where=sel.raw!='$pc' cmd='x')
item(title='Text files' type='file' where=sel.file.ext=='.txt' cmd='x')
item(title='Find text' type='file' find='.txt' cmd='x')
item(title='Many' type='*' where=sel.count>1 cmd='x')
item(title='Taskbar only' type='*' where=window.is_taskbar cmd='x')
item(title='PC with Shift' type='*' where=sel.raw=='$pc' and keys.shift() cmd='x')
item(title='Shift' type='*' where=keys.shift() cmd='x')
item(title='Named' type='file' where=sel.name=='other.txt' cmd='x')
menu(title='Gone' type='*' where=sel.count>1) { item(title='Inside' cmd='x') }
remove(type='*' where=sel.raw=='$pc' find='"Properties"')
item(title='First' type='*' pos=top cmd='x')
item(title='Last' type='*' pos=bottom cmd='x')
menu(title='Empty' type='*') { item(title='Never' where=sel.count>1 cmd='x') }
menu(title='Holder' type='*') { }
modify(type='*' find='"Delete"' menu='Holder')
"@ | Set-Content $ownRoot -Encoding utf8
	$expect = [ordered]@{
		thispc  = @{ 'PC only' = 'yes'; 'Not on PC' = 'no'; 'Many' = 'no'; 'Taskbar only' = 'no'; 'PC with Shift' = 'maybe'; 'Shift' = 'maybe'; 'Gone' = 'no'; 'Inside' = 'no'
		             'Empty' = 'no'; 'Holder' = 'yes'; 'First' = 'yes'; 'Last' = 'yes' }
		file    = @{ 'PC only' = 'no'; 'Not on PC' = 'yes'; 'Text files' = 'yes'; 'Find text' = 'yes'; 'Taskbar only' = 'no'; 'PC with Shift' = 'no'; 'Named' = 'no' }
		taskbar = @{ 'PC only' = 'no'; 'Taskbar only' = 'yes'; 'Many' = 'no' }
	}
	foreach ($where in $expect.Keys) {
		$r = Get-Tree $ownRoot $where $contexts[$where]
		$problems = @()
		if ($r.exit -ne 0 -or -not $r.json.ok) { $problems += "exit $($r.exit), ok=$($r.json.ok)" }
		else {
			$got = @{}; $sample = @{}
			Walk $r.json.tree.shell { param($n) $script:got[$n.title] = $n.shows; $script:sample[$n.title] = $n.shows_sample }
			foreach ($title in $expect[$where].Keys) {
				if ($got[$title] -ne $expect[$where][$title]) { $problems += "'$title' shows '$($got[$title])', expected '$($expect[$where][$title])'" }
			}
			# The sample file stands for any file: its name decides "Named" only for it.
			if ($where -eq 'file' -and -not $sample['Named']) { $problems += "'Named' should depend on the sample" }
			if ($where -eq 'file' -and $sample['Text files']) { $problems += "'Text files' depends on the type of sample only" }
			# No file is This PC, whichever file the sample is.
			if ($where -eq 'file' -and $sample['PC only']) { $problems += "'PC only' should not depend on the sample" }
			$props = @(); Walk $r.json.tree.system { param($n) if ($n.id -eq 'properties') { $script:props += $n } }
			if ($where -eq 'thispc' -and $props.Count -and -not $props[0].hidden) { $problems += 'remove(where=sel.raw==This PC) does not hide Properties' }
			if ($where -eq 'file' -and $props.Count -and ($props[0].hidden -or @($props[0].maybe).Count)) { $problems += "remove(where=sel.raw==This PC) reaches a file's Properties" }
			# The main menu in the order it shows: every Windows item and every one of
			# your items that shows, once each, pos=top first and pos=bottom last.
			$tree = $r.json.tree
			$order = @($tree.order)
			$seen = @{}
			foreach ($o in $order) {
				$list = if ($o.from -eq 'shell') { $tree.shell } else { $tree.system }
				if ($o.index -lt 0 -or $o.index -ge @($list).Count) { $problems += "order points past the $($o.from) list ($($o.index))"; continue }
				$k = "$($o.from)/$($o.index)"
				if ($seen[$k]) { $problems += "order lists $k twice" }
				$seen[$k] = $true
			}
			for ($i = 0; $i -lt @($tree.system).Count; $i++) { if (-not $seen["system/$i"]) { $problems += "order leaves out Windows item $i" } }
			for ($i = 0; $i -lt @($tree.shell).Count; $i++) {
				$n = $tree.shell[$i]
				if (($n.shows -ne 'no') -ne [bool]$seen["shell/$i"]) { $problems += "order and shows disagree for '$($n.title)' ($($n.shows))" }
			}
			$titles = @($order | ForEach-Object { if ($_.from -eq 'shell') { $tree.shell[$_.index].title } else { "W:" + $tree.system[$_.index].title } })
			if ($where -eq 'thispc') {
				if ($titles[0] -ne 'First') { $problems += "pos=top isn't first: $($titles[0])" }
				if ($titles[-1] -ne 'Last') { $problems += "pos=bottom isn't last: $($titles[-1])" }
				$empty = @($tree.shell | Where-Object title -eq 'Empty')
				if ($empty.Count -and $empty[0].shows_why -ne 'empty') { $problems += "'Empty' should be left out as empty ($($empty[0].shows_why))" }
			}
		}
		'{0,-8} own config: {1}' -f $where, $(if ($problems.Count) { 'FAIL' } else { 'ok' })
		foreach ($pr in $problems) { "  FAIL $where (own config): $pr"; $failures += "$where (own config): $pr" }
	}
}
finally {
	Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}

if ($failures.Count) { throw "$($failures.Count) problem(s) in -config:tree output" }
'-config:tree output is complete for every context.'
