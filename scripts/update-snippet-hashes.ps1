<#
    Updates the "sha256" of every entry in src/bin/snippets/catalog.json from its
    src/bin/snippets/<id>.nss. Run it after editing a bundled snippet; the app's tests
    (settings_tests, "snippets: the catalog matches the files") fail until you do.

    The hash is the SHA-256 of the file's text without a UTF-8 BOM and with LF line
    endings, so a checkout with CRLF (core.autocrlf) gives the same value. The app
    hashes the copies in a config folder the same way (src/settings/src/Snippets.cpp).

    Usage: pwsh scripts/update-snippet-hashes.ps1 [-Check]
      -Check   change nothing; exit 1 if a hash is out of date (for CI)
#>
param([switch]$Check)
$ErrorActionPreference = 'Stop'

$dir = Join-Path $PSScriptRoot '..\src\bin\snippets'
$catalogPath = Join-Path $dir 'catalog.json'
$text = [IO.File]::ReadAllText($catalogPath)
$catalog = $text | ConvertFrom-Json

function Get-SnippetHash([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $bytes = $bytes[3..($bytes.Length - 1)]
    }
    $s = [Text.Encoding]::UTF8.GetString($bytes).Replace("`r`n", "`n")
    $sha = [Security.Cryptography.SHA256]::Create()
    -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($s)) | ForEach-Object { $_.ToString('x2') })
}

$stale = 0
foreach ($s in $catalog.snippets) {
    $file = Join-Path $dir "$($s.id).nss"
    if (-not (Test-Path $file)) { throw "catalog.json lists '$($s.id)' but $file doesn't exist" }
    $hash = Get-SnippetHash $file
    if ($hash -eq $s.sha256) { continue }
    $stale++
    Write-Host "$($s.id): $hash"
    $pattern = '(?s)("id"\s*:\s*"' + [regex]::Escape($s.id) + '".*?"sha256"\s*:\s*")[0-9a-fA-F]*(")'
    $text = [regex]::Replace($text, $pattern, { param($m) $m.Groups[1].Value + $hash + $m.Groups[2].Value }, 'None')
}
foreach ($f in Get-ChildItem $dir -Filter *.nss) {
    if (-not ($catalog.snippets | Where-Object id -eq $f.BaseName)) { throw "$($f.Name) is not in catalog.json" }
}

if ($Check) {
    if ($stale) { Write-Error "$stale snippet hash(es) out of date: run scripts/update-snippet-hashes.ps1" }
    exit 0
}
if ($stale) {
    [IO.File]::WriteAllText($catalogPath, $text, [Text.UTF8Encoding]::new($false))
    Write-Host "Updated $stale hash(es) in catalog.json"
} else {
    Write-Host 'All hashes are up to date'
}
