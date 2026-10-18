<#
.SYNOPSIS
    Writes src/shared/ShippedConfigHashes.inc: a fingerprint of every version of the shipped
    config files (src/bin/shell.nss and src/bin/imports/**/*.nss) in the git history.

.DESCRIPTION
    `shell -config:upgrade` (src/shared/ConfigUpgrade.h) replaces a user's copy of a shipped
    file only when that copy is one of these versions, i.e. the user never edited it. So the
    list must hold every version that was ever shipped, including the one in the previous
    release. CI runs this script with -Check on a full clone and fails when the committed
    file is out of date.

    Every blob of those paths in the history of -Rev (and of config/shell.nss, config/imports,
    config/en/shell.nss and config/en/imports, where Nilesoft kept the English defaults it
    shipped before src/bin did) (all commits, merges included), plus the
    files in the working tree, is normalized the same way as ConfigUpgrade.h does it:
      - a UTF-8 BOM at the start is dropped;
      - CRLF and lone CR become LF;
      - spaces and tabs at the end of each line are dropped;
      - the localization block that `shell -config:init` adds to an old shell.nss
        ("// Added by shell -config:init ..." through "// or import lang ...") is dropped,
        with one blank line next to it;
      - lines that only import the app's own files (import 'imports/settings-app*.nss') are
        dropped;
      - blank lines at the end are dropped and the lines are joined with LF.
    The fingerprint is the first 8 bytes of the SHA-256 of the result.

    Run it after changing a file under src/bin/imports or src/bin/shell.nss, and commit the
    result with that change.

.PARAMETER Rev
    The history to read (default HEAD).

.PARAMETER Check
    Only compare with the committed file; exit 1 when it is out of date (for CI).

.EXAMPLE
    pwsh scripts/update-shipped-hashes.ps1
#>
[CmdletBinding()]
param(
    [string]$Rev = 'HEAD',
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$repo = Join-Path $PSScriptRoot '..' | Resolve-Path | Select-Object -ExpandProperty Path
$outPath = Join-Path $repo 'src/shared/ShippedConfigHashes.inc'
$latin1 = [System.Text.Encoding]::Latin1  # byte <-> char one to one
$sha = [System.Security.Cryptography.SHA256]::Create()

# The same steps as config_upgrade::normalize() in src/shared/ConfigUpgrade.h.
function Get-Normalized([byte[]]$bytes) {
    $text = $latin1.GetString($bytes)
    if ($text.StartsWith("$([char]0xEF)$([char]0xBB)$([char]0xBF)")) { $text = $text.Substring(3) }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($l in $text.Split("`n")) { $lines.Add($l.TrimEnd(' ', "`t")) }

    # The block -config:init adds, and one blank line next to it.
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].StartsWith('// Added by shell -config:init')) { $start = $i; break }
    }
    if ($start -ge 0) {
        $end = -1
        for ($i = $start + 1; $i -lt $lines.Count; $i++) {
            if ($lines[$i].StartsWith('// or import lang')) { $end = $i; break }
        }
        if ($end -ge 0) {
            $from = $start; $count = $end - $start + 1
            if ($from -gt 0 -and $lines[$from - 1] -eq '') { $from--; $count++ }
            elseif ($end + 1 -lt $lines.Count -and $lines[$end + 1] -eq '') { $count++ }
            $lines.RemoveRange($from, $count)
        }
    }

    $kept = [System.Collections.Generic.List[string]]::new()
    foreach ($l in $lines) {
        if ($l -match '^[ \t]*import[ \t]+([''"])imports[\\/]settings-app[^''"]*\.nss\1$') { continue }
        $kept.Add($l)
    }
    while ($kept.Count -gt 0 -and $kept[$kept.Count - 1] -eq '') { $kept.RemoveAt($kept.Count - 1) }
    return ,$latin1.GetBytes([string]::Join("`n", $kept))
}

function Get-Fingerprint([byte[]]$bytes) {
    $h = $sha.ComputeHash([byte[]](Get-Normalized $bytes))
    return '0x' + (($h[0..7] | ForEach-Object { $_.ToString('x2') }) -join '') + 'ull'
}

# "src/bin/imports/Lang/x.nss" -> "imports/lang/x.nss"; $null for anything else.
# config/ and config/en/ held the English defaults Nilesoft shipped before src/bin did.
function Get-RelPath([string]$path) {
    $p = $path.Replace('\', '/')
    if ($p -imatch '^(?:src/bin|config|config/en)/(shell\.nss|imports/.+\.nss)$') { return $Matches[1].ToLowerInvariant() }
    return $null
}

Push-Location $repo
try {
    # Every blob these paths ever had: --raw lists the new blob of each change, -m does so
    # for merges against each parent too (a conflict resolution can make a new blob).
    $blobs = @{}  # sha -> the relative paths it had
    $log = git log $Rev --format=%H --raw --no-abbrev -m --no-renames -- src/bin/shell.nss src/bin/imports config/shell.nss config/imports config/en/shell.nss config/en/imports
    if ($LASTEXITCODE) { throw "git log failed ($LASTEXITCODE)" }
    foreach ($line in $log) {
        if ($line -notmatch '^:\d+ \d+ [0-9a-f]+ ([0-9a-f]{40,64}) [A-Z]\d*\t(.+)$') { continue }
        $blob = $Matches[1]; $rel = Get-RelPath $Matches[2]
        if (-not $rel -or $blob -match '^0+$') { continue }
        if (-not $blobs.ContainsKey($blob)) { $blobs[$blob] = [System.Collections.Generic.HashSet[string]]::new() }
        [void]$blobs[$blob].Add($rel)
    }
    if ($blobs.Count -eq 0) { throw "no history for src/bin (a shallow clone? use fetch-depth: 0)" }

    $entries = [System.Collections.Generic.HashSet[string]]::new()

    # Blob contents, all through one `git cat-file --batch`.
    $psi = [System.Diagnostics.ProcessStartInfo]::new('git', 'cat-file --batch')
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $repo
    $proc = [System.Diagnostics.Process]::Start($psi)
    $order = @($blobs.Keys | Sort-Object)
    $stream = $proc.StandardOutput.BaseStream
    foreach ($b in $order) {
        # One at a time: ask, then read the answer (no pipe can fill up and block).
        $proc.StandardInput.Write("$b`n")
        $proc.StandardInput.Flush()
        $header = [System.Text.StringBuilder]::new()
        while (($c = $stream.ReadByte()) -ne 10) {
            if ($c -lt 0) { throw "git cat-file ended early" }
            [void]$header.Append([char]$c)
        }
        $parts = $header.ToString().Split(' ')
        if ($parts.Count -ne 3 -or $parts[1] -ne 'blob') { throw "git cat-file: unexpected '$header'" }
        $size = [int]$parts[2]
        $data = [byte[]]::new($size)
        $read = 0
        while ($read -lt $size) {
            $n = $stream.Read($data, $read, $size - $read)
            if ($n -le 0) { throw "git cat-file ended early" }
            $read += $n
        }
        [void]$stream.ReadByte()  # the LF after the content
        $fp = Get-Fingerprint $data
        foreach ($rel in $blobs[$b]) { [void]$entries.Add("$rel`t$fp") }
    }
    $proc.StandardInput.Close()
    $proc.WaitForExit()

    # The working tree too, so the list can be brought up to date before committing.
    $bin = Join-Path $repo 'src/bin'
    $files = @(Get-Item (Join-Path $bin 'shell.nss')) + @(Get-ChildItem (Join-Path $bin 'imports') -Recurse -File -Filter *.nss)
    foreach ($f in $files) {
        $rel = Get-RelPath ([System.IO.Path]::GetRelativePath($repo, $f.FullName))
        if ($rel) { [void]$entries.Add("$rel`t$(Get-Fingerprint ([System.IO.File]::ReadAllBytes($f.FullName)))") }
    }
}
finally {
    Pop-Location
}

$sorted = [string[]]@($entries)
[System.Array]::Sort($sorted, [System.StringComparer]::Ordinal)
$sb = [System.Text.StringBuilder]::new()
[void]$sb.Append("// Generated by scripts/update-shipped-hashes.ps1; do not edit. CI checks that it is up to date.`n")
[void]$sb.Append("// Every version of the shipped shell.nss and imports\**\*.nss in the git history:`n")
[void]$sb.Append("// { path relative to the config folder (lower case), first 8 bytes of the SHA-256 of`n")
[void]$sb.Append("// the normalized text (see ConfigUpgrade.h) }.`n")
foreach ($e in $sorted) {
    $rel, $fp = $e.Split("`t")
    [void]$sb.Append("{ `"$rel`", $fp },`n")
}
$content = $sb.ToString()

$current = if (Test-Path $outPath) { [System.IO.File]::ReadAllText($outPath).Replace("`r`n", "`n") } else { '' }
if ($Check) {
    if ($current -ne $content) {
        Write-Host "src/shared/ShippedConfigHashes.inc is out of date: run pwsh scripts/update-shipped-hashes.ps1 and commit it."
        exit 1
    }
    Write-Host "src/shared/ShippedConfigHashes.inc is up to date ($($sorted.Count) entries)."
    exit 0
}

if ($current -ne $content) {
    [System.IO.File]::WriteAllText($outPath, $content, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Wrote $outPath ($($sorted.Count) entries)."
}
else {
    Write-Host "$outPath is up to date ($($sorted.Count) entries)."
}
